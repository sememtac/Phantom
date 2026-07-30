// Standalone implementation of the vg_port.h seam for the
// Waveshare ESP32-S3-Touch-AMOLED-2.16.
//
// This drives the CO5300 over QSPI directly instead of going through
// Arduino_GFX. The reason is measured: Arduino_GFX's writePixels() walks every
// pixel through a CPU byte-swap into a bounce buffer, which cost ~13 ms per
// frame on top of the ~11.5 ms the wire actually needs. There is no zero-copy
// path in that library -- write16bitBeRGBBitmapR1() transposes for rotation, so
// it copies too.
//
// Here, band buffers are already stored big-endian (see VGC() in vg_config.h),
// so a blit is one DMA transaction straight out of the rasteriser's memory.
//
// Protocol, as used by every QSPI AMOLED of this family:
//   command : cmd 0x02, addr = reg << 8, params on 1 line
//   pixels  : cmd 0x32, addr = 0x2C00,   data on 4 lines (QIO)
// Command and address phases stay single-line in both cases. Each transaction
// is self-contained, so hardware CS is fine -- no need for the manual CS
// juggling Arduino_GFX does to hold a chip-select across chunked writes.
//
// Drop this file when embedding the game in another firmware and supply a port
// built on that firmware's own display / touch / IMU layer instead.

#include "vg_port.h"
#include "vg_config.h"
#include <Arduino.h>
#include <Wire.h>
#include <driver/spi_master.h>
#include <esp_heap_caps.h>
#include <SensorQMI8658.hpp>
#include <TouchDrvCSTXXX.hpp>
#include <Preferences.h>

// ---- Board pin map (Waveshare ESP32-S3-Touch-AMOLED-2.16) ----
#define LCD_CS       12
#define LCD_SCLK     38
#define LCD_SDIO0     4
#define LCD_SDIO1     5
#define LCD_SDIO2     6
#define LCD_SDIO3     7
#define LCD_RESET     2

#define IIC_SDA      15
#define IIC_SCL      14

#define TP_INT       11
#define TP_RST        2      // physically shared with LCD_RESET
#define CST9220_ADDR 0x5A

// The two GPIO buttons. The third physical button is the AXP2101 power key,
// which is reachable only through the PMU over I2C -- not worth pulling
// XPowersLib back in for.
#define BTN_A_GPIO    0      // BOOT
#define BTN_B_GPIO   18

#define LCD_HOST     SPI2_HOST
#define LCD_CLOCK_HZ 80000000

// Set the address window ONCE per frame and stream the fifteen bands into it as
// memory-continue writes, instead of re-windowing before every band. The window
// commands are polled transactions and each costs far more in driver overhead
// than the few bits it puts on the wire -- 30 of them per frame was ~0.5 ms.
//
// Requires the controller to implement RAMWRC (0x3C). If it does not, bands 1..14
// restart at the top of the window and the display shows only the first band
// repeated -- unmistakable, and this flag is the single-line revert.
#define LCD_STREAM_FRAME 1

// CO5300 registers
#define CO_SLPOUT    0x11
#define CO_INVOFF    0x20
#define CO_DISPON    0x29
#define CO_CASET     0x2A
#define CO_PASET     0x2B
#define CO_RAMWR     0x2C
#define CO_RAMWRC    0x3C     // write memory continue -- resumes where 0x2C left off
#define CO_MADCTL    0x36
#define CO_PIXFMT    0x3A
#define CO_BRIGHT    0x51
#define CO_CTRLD1    0x53
#define CO_WCE       0x58
#define CO_BRIGHTHBM 0x63
#define CO_SPIMODE   0xC4

static spi_device_handle_t s_spi = nullptr;

static SensorQMI8658   s_imu;
static bool            s_imu_ok = false;
static TouchDrvCST92xx s_touch;
static bool            s_touch_ok = false;

// ---------------------------------------------------------------------------
// Low-level
// ---------------------------------------------------------------------------

// Up to 4 parameter bytes, carried in tx_data so the caller can pass a stack
// buffer without worrying about DMA-capable memory.
static void co_cmd(uint8_t reg, const uint8_t* params, int n) {
    spi_transaction_t t = {};
    t.cmd  = 0x02;
    t.addr = ((uint32_t)reg) << 8;
    if (n > 0) {
        t.flags  = SPI_TRANS_USE_TXDATA;
        t.length = (size_t)n * 8;
        for (int i = 0; i < n && i < 4; i++) t.tx_data[i] = params[i];
    }
    spi_device_polling_transmit(s_spi, &t);
}

static inline void co_cmd0(uint8_t reg)             { co_cmd(reg, nullptr, 0); }
static inline void co_cmd1(uint8_t reg, uint8_t v)  { co_cmd(reg, &v, 1); }

static void co_window(int x0, int y0, int x1, int y1) {
    uint8_t c[4] = { (uint8_t)(x0 >> 8), (uint8_t)x0, (uint8_t)(x1 >> 8), (uint8_t)x1 };
    uint8_t p[4] = { (uint8_t)(y0 >> 8), (uint8_t)y0, (uint8_t)(y1 >> 8), (uint8_t)y1 };
    co_cmd(CO_CASET, c, 4);
    co_cmd(CO_PASET, p, 4);
}

// ---------------------------------------------------------------------------
// Panel
// ---------------------------------------------------------------------------

// The ESP32-S3's SPI length register is 18 bits, so one transaction tops out at
// 2^18 bits = 32768 bytes. BAND_H is sized so a whole band fits in one.
static_assert(SCR_W * BAND_H * 2 <= 32768,
              "band exceeds the ESP32-S3 SPI max transaction size; lower BAND_H");

// Two transaction descriptors, alternating, because a queued transaction's
// descriptor must stay valid until its result is reaped.
static spi_transaction_t s_tx[2];
static int  s_tx_slot    = 0;
static bool s_tx_pending = false;

void vg_panel_wait(void) {
    if (!s_tx_pending) return;
    spi_transaction_t* done = nullptr;
    spi_device_get_trans_result(s_spi, &done, portMAX_DELAY);
    s_tx_pending = false;
}

void vg_panel_push_band(int y, int h, const uint16_t* pixels) {
    if (!s_spi) return;

    // The address-window commands are polled transactions, and IDF forbids
    // those while anything is queued -- and in any case the window must not
    // move until the previous band's pixels have finished landing. So drain
    // first; this is also what makes the caller's buffer ping-pong safe.
    vg_panel_wait();

#if LCD_STREAM_FRAME
    // One window for the whole screen, then fourteen continuations. The
    // controller keeps its own write pointer, so each band simply carries on
    // from where the last one stopped.
    const bool first = (y == 0);
    if (first) co_window(0, 0, SCR_W - 1, SCR_H - 1);
#else
    const bool first = true;
    co_window(0, y, SCR_W - 1, y + h - 1);
#endif

    spi_transaction_t* t = &s_tx[s_tx_slot];
    s_tx_slot ^= 1;

    memset(t, 0, sizeof(*t));
    t->flags     = SPI_TRANS_MODE_QIO;   // data on 4 lines; cmd/addr single
    t->cmd       = 0x32;
    t->addr      = ((uint32_t)(first ? CO_RAMWR : CO_RAMWRC)) << 8;
    t->length    = (size_t)SCR_W * (size_t)h * 16;   // bits
    t->tx_buffer = pixels;

    // Queue and return: the caller now rasterises the next band into its other
    // buffer while this one is on the wire. This is the whole point -- the CPU
    // used to busy-wait through all 11.5 ms of transfer per frame.
    if (spi_device_queue_trans(s_spi, t, portMAX_DELAY) == ESP_OK) s_tx_pending = true;
}

void vg_panel_brightness(uint8_t level) {
    if (!s_spi) return;
    vg_panel_wait();               // polled command; nothing may be queued
    co_cmd1(CO_BRIGHT, level);
}

static void panel_clear(void) {
    const int CH = 16;   // divides 480 exactly, and well under the 32 KB limit
    uint16_t* buf = (uint16_t*)heap_caps_calloc(SCR_W * CH, 2, MALLOC_CAP_DMA);
    if (!buf) return;
    for (int y = 0; y < SCR_H; y += CH) vg_panel_push_band(y, CH, buf);
    vg_panel_wait();     // the buffer is about to go away
    heap_caps_free(buf);
}

bool vg_panel_init(void) {
    // Shared I2C for touch + IMU. The per-frame touch read is pure frame time --
    // measured at ~0.93 ms at 400 kHz, which is 6% of a 16.67 ms budget for
    // reading a handful of registers. Fast-mode plus cuts it to roughly a third
    // of that. If touch ever goes unreliable, this is the first thing to halve.
    Wire.begin(IIC_SDA, IIC_SCL);
    Wire.setClock(1000000);

    pinMode(LCD_RESET, OUTPUT);
    digitalWrite(LCD_RESET, HIGH); delay(10);
    digitalWrite(LCD_RESET, LOW);  delay(200);
    digitalWrite(LCD_RESET, HIGH); delay(200);

    spi_bus_config_t buscfg = {};
    buscfg.mosi_io_num     = LCD_SDIO0;
    buscfg.miso_io_num     = LCD_SDIO1;
    buscfg.sclk_io_num     = LCD_SCLK;
    buscfg.quadwp_io_num   = LCD_SDIO2;
    buscfg.quadhd_io_num   = LCD_SDIO3;
    buscfg.data4_io_num    = -1;
    buscfg.data5_io_num    = -1;
    buscfg.data6_io_num    = -1;
    buscfg.data7_io_num    = -1;
    // One whole band in a single transaction: 480 * BAND_H * 2 bytes.
    buscfg.max_transfer_sz = SCR_W * BAND_H * 2 + 64;
    buscfg.flags           = SPICOMMON_BUSFLAG_MASTER | SPICOMMON_BUSFLAG_GPIO_PINS;

    if (spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO) != ESP_OK) {
        Serial.println("vg_panel_init: spi_bus_initialize failed");
        return false;
    }

    spi_device_interface_config_t devcfg = {};
    devcfg.command_bits   = 8;
    devcfg.address_bits   = 24;
    devcfg.dummy_bits     = 0;
    devcfg.mode           = 0;
    devcfg.clock_source   = SPI_CLK_SRC_DEFAULT;
    devcfg.clock_speed_hz = LCD_CLOCK_HZ;
    devcfg.spics_io_num   = LCD_CS;    // transactions are self-contained
    devcfg.flags          = SPI_DEVICE_HALFDUPLEX;
    devcfg.queue_size     = 2;

    if (spi_bus_add_device(LCD_HOST, &devcfg, &s_spi) != ESP_OK) {
        Serial.println("vg_panel_init: spi_bus_add_device failed");
        return false;
    }

    co_cmd0(CO_SLPOUT);
    delay(120);
    co_cmd1(0xFE, 0x00);          // page select: user commands
    co_cmd1(CO_SPIMODE, 0x80);
    co_cmd1(CO_PIXFMT, 0x55);     // 16 bit/px
    co_cmd1(CO_CTRLD1, 0x20);
    co_cmd1(CO_BRIGHTHBM, 0xFF);
    co_cmd1(CO_MADCTL, 0x00);     // RGB order, no flips
    co_cmd0(CO_DISPON);
    co_cmd1(CO_BRIGHT, 210);
    co_cmd1(CO_WCE, 0x00);        // contrast enhancement off
    co_cmd0(CO_INVOFF);
    delay(10);

    panel_clear();
    Serial.printf("vg_panel_init: CO5300 up @%d MHz QSPI\n", LCD_CLOCK_HZ / 1000000);
    return true;
}

// ---------------------------------------------------------------------------
// Buttons
// ---------------------------------------------------------------------------

void vg_buttons_init(void) {
    pinMode(BTN_A_GPIO, INPUT_PULLUP);
    pinMode(BTN_B_GPIO, INPUT_PULLUP);
}

uint8_t vg_buttons_read(void) {
    uint8_t m = 0;
    if (digitalRead(BTN_A_GPIO) == LOW) m |= VG_BTN_A;
    if (digitalRead(BTN_B_GPIO) == LOW) m |= VG_BTN_B;
    return m;
}

// ---------------------------------------------------------------------------
// The power key
//
// PWR is not on a GPIO and no pin scan will find it. It belongs to the AXP2101,
// and the only way to see it from software is that chip's interrupt registers,
// over the I2C bus the touch panel and the IMU already share.
//
// THE BUS RUNS AT 1 MHz FOR THE TOUCH PANEL AND THE AXP2101 IS A 400 kHz PART.
// That is why the first version of this saw nothing at all: the registers were
// right, the bits were right, and every transaction was simply out of spec. The
// clock is dropped for the handful of bytes this needs and put back afterwards,
// because the touch read is per-frame and wants the speed.
//
// Only two register groups are touched, and neither can affect a power rail:
// 0x40..0x42 gate which events are reported, and 0x48..0x4A latch what happened
// and are cleared by writing the bits back.
//
// Register 0x49, from the AXP2101's IRQ2 group:
//   bit 0  release edge     bit 1  press edge
//   bit 2  long press       bit 3  short press
// ---------------------------------------------------------------------------
#define AXP2101_ADDR  0x34
#define AXP_IRQ_EN    0x40
#define AXP_IRQ_ST    0x48

#define AXP_PKEY_POS   0x01     // in 0x49
#define AXP_PKEY_NEG   0x02
#define AXP_PKEY_LONG  0x04
#define AXP_PKEY_SHORT 0x08

// Fast enough for the panel, too fast for the PMU.
#define IIC_HZ_FAST   1000000
#define IIC_HZ_PMU     400000

// Polling every frame would spend three I2C transactions and two clock changes
// on a key nobody presses. 50 ms is far below the shortest press anyone can make.
#define PMU_POLL_MS        50

static bool     s_pmu_present  = false;
static uint32_t s_pmu_last_ms  = 0;
static bool     s_pwr_short    = false;   // latched until read
static uint8_t  s_pmu_seen[3]  = { 0, 0, 0 };

static bool pmu_write(uint8_t reg, const uint8_t* v, int n) {
    Wire.beginTransmission(AXP2101_ADDR);
    Wire.write(reg);
    for (int i = 0; i < n; i++) Wire.write(v[i]);
    return Wire.endTransmission() == 0;
}

static bool pmu_read(uint8_t reg, uint8_t* v, int n) {
    Wire.beginTransmission(AXP2101_ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return false;
    if ((int)Wire.requestFrom((int)AXP2101_ADDR, n) != n) return false;
    for (int i = 0; i < n; i++) v[i] = (uint8_t)Wire.read();
    return true;
}

bool vg_pmu_init(void) {
    Wire.setClock(IIC_HZ_PMU);

    // ONLY the power-key events. Enabling everything was the probe's expedient
    // and it would now be actively wrong: battery and VBUS interrupts fire on
    // their own schedule and would leave the key's bit indistinguishable from
    // the charger being plugged in.
    const uint8_t en[3] = { 0x00, AXP_PKEY_POS | AXP_PKEY_NEG
                                 | AXP_PKEY_LONG | AXP_PKEY_SHORT, 0x00 };
    s_pmu_present = pmu_write(AXP_IRQ_EN, en, 3);

    if (s_pmu_present) {
        uint8_t st[3];
        if (pmu_read(AXP_IRQ_ST, st, 3)) pmu_write(AXP_IRQ_ST, st, 3);   // clear
    }

    Wire.setClock(IIC_HZ_FAST);
    return s_pmu_present;
}

// Poll, and latch a short press until somebody collects it. A press has to
// survive not being looked at on the exact frame it happened.
static void pmu_poll(void) {
    if (!s_pmu_present) return;
    const uint32_t now = millis();
    if (now - s_pmu_last_ms < PMU_POLL_MS) return;
    s_pmu_last_ms = now;

    Wire.setClock(IIC_HZ_PMU);

    uint8_t st[3];
    if (pmu_read(AXP_IRQ_ST, st, 3) && (st[0] | st[1] | st[2])) {
        pmu_write(AXP_IRQ_ST, st, 3);            // write the bits back to clear
        s_pmu_seen[0] |= st[0];
        s_pmu_seen[1] |= st[1];
        s_pmu_seen[2] |= st[2];
        if (st[1] & AXP_PKEY_SHORT) s_pwr_short = true;
    }

    Wire.setClock(IIC_HZ_FAST);
}

bool vg_pmu_pwr_pressed(void) {
    pmu_poll();
    if (!s_pwr_short) return false;
    s_pwr_short = false;
    return true;
}

void vg_pmu_seen(uint8_t* st3) {
    st3[0] = s_pmu_seen[0]; st3[1] = s_pmu_seen[1]; st3[2] = s_pmu_seen[2];
}

// What is actually on the bus, and what the PMU holds, once at boot. Kept
// because "nothing happened" has several very different causes -- no such chip,
// a chip that is not an AXP2101, enable writes that did not stick, a bus out of
// spec -- and telling them apart from a distance is otherwise guesswork.
void vg_pmu_dump(void) {
    Wire.setClock(IIC_HZ_PMU);

    Serial.print("I2C:");
    for (uint8_t a = 1; a < 0x7F; a++) {
        Wire.beginTransmission(a);
        if (Wire.endTransmission() == 0) Serial.printf(" %02X", a);
    }
    Serial.println();

    uint8_t v[4];
    if (pmu_read(0x03, v, 1)) Serial.printf("PMU id 03: %02X\n", v[0]);
    else                      Serial.println("PMU id 03: read failed");
    if (pmu_read(AXP_IRQ_EN, v, 3))
        Serial.printf("PMU en 40-42: %02X %02X %02X\n", v[0], v[1], v[2]);
    if (pmu_read(AXP_IRQ_ST, v, 3))
        Serial.printf("PMU st 48-4A: %02X %02X %02X\n", v[0], v[1], v[2]);

    Wire.setClock(IIC_HZ_FAST);
}

// ---------------------------------------------------------------------------
// IMU
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Persistent storage (NVS)
// ---------------------------------------------------------------------------

static Preferences s_prefs;
static bool        s_store_ok = false;

bool vg_store_init(void) {
    // Read/write. The Arduino core has already brought NVS up by this point;
    // if the partition is missing or full this returns false and the game runs
    // on perfectly well, just forgetfully.
    s_store_ok = s_prefs.begin("phantom", false);
    if (!s_store_ok) Serial.println("vg_store_init: NVS unavailable");
    return s_store_ok;
}

bool vg_store_load(void* data, unsigned len) {
    if (!s_store_ok) return false;
    // Check first. Asking for a key that is not there is the normal state of a
    // brand new device, but Preferences logs it at ERROR level, which puts a
    // frightening line in the boot output of a board that is working perfectly.
    if (!s_prefs.isKey("save")) return false;
    return s_prefs.getBytes("save", data, len) == len;
}

bool vg_store_save(const void* data, unsigned len) {
    if (!s_store_ok) return false;
    return s_prefs.putBytes("save", data, len) == len;
}

// ---------------------------------------------------------------------------

bool vg_imu_init(void) {
    if (!s_imu.begin(Wire, QMI8658_L_SLAVE_ADDRESS, IIC_SDA, IIC_SCL)) {
        Serial.println("vg_imu_init: QMI8658 not found");
        s_imu_ok = false;
        return false;
    }
    // A UI app can get away with ~20 Hz here because it only wants a rotation
    // quadrant. Flying needs a fresh sample every frame.
    s_imu.configAccelerometer(SensorQMI8658::ACC_RANGE_4G,
                              SensorQMI8658::ACC_ODR_250Hz,
                              SensorQMI8658::LPF_MODE_2);
    s_imu.enableAccelerometer();
    s_imu_ok = true;
    Serial.println("vg_imu_init: QMI8658 up @250Hz");
    return true;
}

bool vg_imu_read(float* ax, float* ay, float* az) {
    if (!s_imu_ok) return false;
    return s_imu.getAccelerometer(*ax, *ay, *az);
}

// ---------------------------------------------------------------------------
// Touch
// ---------------------------------------------------------------------------

bool vg_touch_init(void) {
    s_touch.setPins(TP_RST, TP_INT);
    if (!s_touch.begin(Wire, CST9220_ADDR, IIC_SDA, IIC_SCL)) {
        Serial.println("vg_touch_init: CST9220 not found");
        s_touch_ok = false;
        return false;
    }
    s_touch.setMaxCoordinates(SCR_W, SCR_H);
    s_touch.setSwapXY(true);
    s_touch.setMirrorXY(true, false);
    pinMode(TP_INT, INPUT_PULLUP);
    s_touch_ok = true;
    Serial.println("vg_touch_init: CST9220 up");
    return true;
}

int vg_touch_read(uint16_t* xs, uint16_t* ys) {
    if (!s_touch_ok) return 0;

    // Zeroed, because the controller does not always fill every slot it counts.
    int16_t tx[VG_MAX_TOUCH] = {0};
    int16_t ty[VG_MAX_TOUCH] = {0};

    uint8_t want = s_touch.getSupportTouchPoint();
    if (want > VG_MAX_TOUCH) want = VG_MAX_TOUCH;

    // Polled, not interrupt-latched: the game reads once per frame and needs
    // the *current* set of contacts, including "all fingers lifted", which an
    // edge-triggered latch does not give you.
    uint8_t n = s_touch.getPoint(tx, ty, want);
    if (n > VG_MAX_TOUCH) n = VG_MAX_TOUCH;

    int out = 0;
    for (uint8_t i = 0; i < n; i++) {
        // Drop (0,0) slots. As a finger lifts, the controller can report a count
        // that still includes its slot but with the coordinates cleared, and
        // raw (0,0) maps to panel (0,479) -- inside the throttle strip and below
        // its travel, which reads as "thumb slammed to idle" and silently zeroed
        // the throttle. The real bottom-left pixel is not worth the bug.
        if (tx[i] == 0 && ty[i] == 0) continue;

        // The controller is configured swap+mirror, which lands its output a
        // quarter turn off the panel; this undoes that.
        uint16_t px = (uint16_t)ty[i];
        uint16_t py = (uint16_t)(SCR_H - 1 - tx[i]);

        // Then undo the display rotation, so everything above this seam works
        // in one logical frame -- buttons up -- and never sees panel space.
#if VG_ROTATE == 1
        xs[out] = (uint16_t)(SCR_H - 1 - py);
        ys[out] = px;
#elif VG_ROTATE == 2
        xs[out] = (uint16_t)(SCR_W - 1 - px);
        ys[out] = (uint16_t)(SCR_H - 1 - py);
#elif VG_ROTATE == 3
        xs[out] = py;
        ys[out] = (uint16_t)(SCR_W - 1 - px);
#else
        xs[out] = px;
        ys[out] = py;
#endif
        out++;
    }
    return out;
}
