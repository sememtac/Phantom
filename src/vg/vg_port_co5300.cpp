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
#include "vg_prof.h"
#include <Arduino.h>
#include <Wire.h>
#include <driver/spi_master.h>
#include <esp_heap_caps.h>
#include <SensorQMI8658.hpp>
#include <TouchDrvCSTXXX.hpp>
#include <Preferences.h>
#include <ESP_I2S.h>
extern "C" {
#include "../third_party/es8311.h"
}

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

// ---- Audio (ES8311 mono codec + onboard speaker, I2S) ----
// Pins from Waveshare's factory example for this board. The codec shares the I2C
// bus above at 0x18 -- which is why it showed up in the bus scan long before
// anything was playing.
#define SND_I2S_MCLK   42
#define SND_I2S_BCLK    9
#define SND_I2S_WS     45
#define SND_I2S_DOUT    8
#define SND_I2S_DIN    10
#define SND_PA_PIN     46     // power-amp enable, HIGH = on
#define SND_ES8311_ADDR 0x18

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

// ONE BAND ON THE WIRE AND ONE ALREADY QUEUED BEHIND IT.
//
// This used to drain the queue before every band, which left the wire dark for a
// whole round trip fifteen times a frame: DMA completes, ISR runs, the render task
// wakes, returns into push_band, and only then is the next band handed over.
// Measured as blit 12588 against a wire floor of 11520 -- 590 us of that gap was the
// gaps, with nothing overrunning its window.
//
// Keeping one transaction queued behind the one in flight removes the CPU from the
// path entirely: the SPI engine starts the next band the instant the current one
// ends, because it already has it.
//
// TWO IS THE CEILING, and it is devcfg.queue_size that sets it. Going deeper would
// need a fourth band buffer for no further gain -- one queued is already enough to
// keep the engine fed.
#define PANEL_QUEUE_MAX 2

// THREE DESCRIPTORS, ONE PER BAND BUFFER, and they rotate in lockstep with them. Two
// would provably do -- the slot two pushes back cannot still be outstanding when at
// most two are -- but pairing a descriptor with the buffer it describes removes the
// need to make that argument every time this is read.
static spi_transaction_t s_tx[3];
static int s_tx_slot    = 0;
static int s_outstanding = 0;

// Reap exactly one. The driver returns them in the order they were queued.
static void panel_reap(void) {
    if (s_outstanding <= 0) return;
    spi_transaction_t* done = nullptr;
    // BOUNDED. This waited forever, and a transfer that never completes --
    // whatever wedged it -- then took the whole game with it: watchdog reset,
    // "died in flush". Two seconds is a thousand times the longest legitimate
    // transfer; past that the transfer is abandoned and the frame degrades
    // instead of the game dying. The counter is printed by the caller's
    // telemetry, so a wedge is visible the second it starts.
    if (spi_device_get_trans_result(s_spi, &done, pdMS_TO_TICKS(2000)) != ESP_OK) {
        static uint32_t s_wedges = 0;
        Serial.printf("vg_panel: DMA wait timed out (%lu)\n",
                      (unsigned long)++s_wedges);
    }
    s_outstanding--;
}

// Drain everything. Called at the top of a flush, before the window command, and
// wherever a caller is about to take a band buffer away.
void vg_panel_wait(void) {
    while (s_outstanding > 0) panel_reap();
}

void vg_panel_push_band(int y, int h, const uint16_t* pixels) {
    if (!s_spi) return;

#if LCD_STREAM_FRAME
    // One window for the whole screen, then fourteen continuations. The
    // controller keeps its own write pointer, so each band simply carries on
    // from where the last one stopped.
    const bool first = (y == 0);
#else
    const bool first = true;
#endif

    // DRAINED ONLY FOR THE WINDOW COMMAND. It is a polled transaction and IDF
    // forbids one while anything is queued -- but with LCD_STREAM_FRAME on, only the
    // first band issues one. Bands 1..14 used to drain for a reason that did not
    // apply to them, and that drain was the wire's idle time.
    //
    // Otherwise: make room for one, keeping the queue as deep as it is allowed to be.
    if (first) vg_panel_wait();
    else       while (s_outstanding >= PANEL_QUEUE_MAX) panel_reap();

#if LCD_STREAM_FRAME
    if (first) co_window(0, 0, SCR_W - 1, SCR_H - 1);
#else
    co_window(0, y, SCR_W - 1, y + h - 1);
#endif

    spi_transaction_t* t = &s_tx[s_tx_slot];
    s_tx_slot = (s_tx_slot + 1) % 3;

    memset(t, 0, sizeof(*t));
    t->flags     = SPI_TRANS_MODE_QIO;   // data on 4 lines; cmd/addr single
    t->cmd       = 0x32;
    t->addr      = ((uint32_t)(first ? CO_RAMWR : CO_RAMWRC)) << 8;
    t->length    = (size_t)SCR_W * (size_t)h * 16;   // bits
    t->tx_buffer = pixels;

    // Queue and return: the caller now rasterises the next band into its other
    // buffer while this one is on the wire. This is the whole point -- the CPU
    // used to busy-wait through all 11.5 ms of transfer per frame.
    if (spi_device_queue_trans(s_spi, t, portMAX_DELAY) == ESP_OK) s_outstanding++;
}

static void panel_clear(void) {
    const int CH = 16;   // divides 480 exactly, and well under the 32 KB limit
    uint16_t* buf = (uint16_t*)heap_caps_calloc(SCR_W * CH, 2, MALLOC_CAP_DMA);
    if (!buf) return;
    // ONE BUFFER FOR EVERY CHUNK, so this cannot use the queue depth the frame loop
    // does: a second chunk in flight would be reading a buffer the next push is about
    // to hand over again. Harmless today because every chunk is the same zeroes, and
    // drained anyway rather than left as a trap for whoever makes this draw something.
    for (int y = 0; y < SCR_H; y += CH) {
        vg_panel_push_band(y, CH, buf);
        vg_panel_wait();
    }
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

// ---------------------------------------------------------------------------
// THE I2C LOCK, and it is not about the bus.
//
// esp32-hal-i2c already takes a per-bus semaphore around each transfer, so two
// cores cannot interleave on the WIRE. What it does not protect is TwoWire itself:
// rxBuffer, rxIndex, rxLength and txBuffer are members of the one shared Wire
// object, so two threads doing beginTransmission / requestFrom / read() interleave
// their BUFFERS even though each individual transfer is serialised.
//
// That is what `pmu FFFFFF` was -- every interrupt bit set, the accumulator
// saturating on a three-byte read that came back as somebody else's bytes. The
// first guess was the bus clock, since this file switches between 1 MHz and 400 kHz
// and the setting is global; that was wrong, and it is worth recording as wrong
// because it would have been fixed by moving the clock switch and the fault would
// have stayed.
//
// So the lock spans the whole LOGICAL transaction -- address, transfer and
// read-out, plus any clock change around it -- and every runtime I2C entry point
// below takes it. The inits do not need to: they all run in setup(), on one thread,
// before any task exists.
// ---------------------------------------------------------------------------
static SemaphoreHandle_t s_i2c_mux = nullptr;

static void i2c_lock_init(void) {
    if (!s_i2c_mux) s_i2c_mux = xSemaphoreCreateMutex();
}
static inline void i2c_take(void) {
    if (s_i2c_mux) xSemaphoreTake(s_i2c_mux, portMAX_DELAY);
}
static inline void i2c_give(void) {
    if (s_i2c_mux) xSemaphoreGive(s_i2c_mux);
}

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
    i2c_lock_init();
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
//
// CALLED ONLY FROM THE SENSOR TASK, and that is a correctness requirement rather
// than a preference. Note the two setClock calls below: this part needs the bus at
// 400 kHz where the touch controller and the IMU run it at 1 MHz, and the clock is
// GLOBAL BUS STATE. While the reads all sat on the render thread that was merely a
// pair of switches; the moment the sensors moved to core 0 it became a race, and it
// showed up immediately as `pmu FFFFFF` in the telemetry -- every interrupt bit set,
// which is the accumulator saturating on reads that failed and returned 0xFF
// because they went out at 1 MHz.
//
// One owner for the bus fixes it at the root, where a mutex would only have made
// the clock switching safe while leaving it on the render thread. Now every I2C
// transaction in the running game -- touch, IMU and this -- happens on one task, so
// the switches are serialised by there being nobody else to race.
void vg_pmu_poll(void) {
    if (!s_pmu_present) return;
    const uint32_t now = millis();
    if (now - s_pmu_last_ms < PMU_POLL_MS) return;
    s_pmu_last_ms = now;

    // Held across the clock change as well as the transfers: leaving the bus at
    // 400 kHz for somebody else is merely slow, but sharing TwoWire's rxBuffer is
    // the fault this lock exists for.
    i2c_take();
    Wire.setClock(IIC_HZ_PMU);

    uint8_t st[3];
    if (pmu_read(AXP_IRQ_ST, st, 3) && (st[0] | st[1] | st[2])) {
        pmu_write(AXP_IRQ_ST, st, 3);            // write the bits back to clear
        s_pmu_seen[0] |= st[0];
        s_pmu_seen[1] |= st[1];
        s_pmu_seen[2] |= st[2];
        if (st[1] & AXP_PKEY_SHORT)
            __atomic_store_n(&s_pwr_short, true, __ATOMIC_RELEASE);
    }

    Wire.setClock(IIC_HZ_FAST);
    i2c_give();
}

bool vg_pmu_pwr_pressed(void) {
    // COLLECTS, no longer polls -- the sensor task does that on the core that owns
    // the bus. The latch is what makes the split safe, and it was already here for
    // the same reason: a press must survive not being looked at on the frame it
    // happened, and now it must also survive being set by the other core.
    //
    // An exchange rather than a test-and-clear. The set happens on core 0 and the
    // clear here on core 1, so the read-modify-write has to be one operation or a
    // press arriving between the test and the clear is swallowed.
    return __atomic_exchange_n(&s_pwr_short, false, __ATOMIC_ACQ_REL);
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

bool vg_store_diag_load(void* data, unsigned len) {
    if (!s_store_ok) return false;
    if (!s_prefs.isKey("diag")) return false;
    return s_prefs.getBytes("diag", data, len) == len;
}

bool vg_store_diag_save(const void* data, unsigned len) {
    if (!s_store_ok) return false;
    return s_prefs.putBytes("diag", data, len) == len;
}

// ---------------------------------------------------------------------------

bool vg_imu_init(void) {
    i2c_lock_init();
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
    i2c_take();
    const bool ok = s_imu.getAccelerometer(*ax, *ay, *az);
    i2c_give();
    return ok;
}

// ---------------------------------------------------------------------------
// Touch
// ---------------------------------------------------------------------------

bool vg_touch_init(void) {
    i2c_lock_init();
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

    // LOCKED, and this one is easy to forget because it is the read that did NOT
    // move to another core -- which is exactly why it needs the lock: one unlocked
    // participant defeats the mutex for everybody. Left out on the first attempt,
    // and `pmu 0000FF` stayed on the telemetry until it went in.
    i2c_take();
    uint8_t want = s_touch.getSupportTouchPoint();
    if (want > VG_MAX_TOUCH) want = VG_MAX_TOUCH;

    // Polled, not interrupt-latched: the game reads once per frame and needs
    // the *current* set of contacts, including "all fingers lifted", which an
    // edge-triggered latch does not give you.
    uint8_t n = s_touch.getPoint(tx, ty, want);
    i2c_give();
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

// ---------------------------------------------------------------------------
// Audio
//
// The codec is configured at the SAMPLE RATE THE GAME GENERATES, not at 44.1k.
// Every sample here is synthesised on the fly (vg_sfx.cpp), and generating twice
// as many of them to feed a rate nothing needs would double the only cost that
// is not already free.
//
// I2S runs STEREO with both slots because the part expects a stereo frame; the
// mono sample is simply written to both. That doubles the bytes on the wire and
// nothing else -- the wire is not the constraint.
// ---------------------------------------------------------------------------
static I2SClass s_i2s;
static bool     s_audio_ok = false;

bool vg_audio_init(void) {
    pinMode(SND_PA_PIN, OUTPUT);
    digitalWrite(SND_PA_PIN, LOW);          // amp off until the codec is up

    s_i2s.setPins(SND_I2S_BCLK, SND_I2S_WS, SND_I2S_DOUT, SND_I2S_DIN, SND_I2S_MCLK);
    // THE FRAME NEVER WAITS FOR THE SPEAKER. I2SClass::write inherits
    // Stream's timeout, which defaults to ONE SECOND per chunk when the DMA
    // ring is full -- and the ring fills, because the sample clock and the
    // codec's real rate drift by a few samples a second, so after some minutes
    // of play every write blocked at the speaker's pace. Profiling showed the
    // synth stage at five milliseconds; the mixing was under one, and the rest
    // was this wait. Zero means write what fits and return, which is what the
    // short-write branch below the write call was always written to handle.
    s_i2s.setTimeout(0);
    if (!s_i2s.begin(I2S_MODE_STD, VG_AUDIO_RATE, I2S_DATA_BIT_WIDTH_16BIT,
                     I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_BOTH)) {
        Serial.println("vg_audio: I2S init failed");
        return false;
    }

    // 400 kHz for the codec, exactly as for the PMU. The bus runs at 1 MHz for
    // the touch panel and the ES8311 is a 400 kHz part -- the same out-of-spec
    // clock that made the power key look like it did not exist.
    Wire.setClock(400000);
    es8311_handle_t es = es8311_create(0, SND_ES8311_ADDR);
    bool ok = (es != nullptr);
    if (ok) {
        const es8311_clock_config_t clk = {
            false, false, true, VG_AUDIO_RATE * 256, VG_AUDIO_RATE
        };
        ok = (es8311_init(es, &clk, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16) == ESP_OK);
        if (ok) {
            es8311_sample_frequency_config(es, clk.mclk_frequency, clk.sample_frequency);
            es8311_microphone_config(es, false);
            // The codec's own gain stays fixed and loud-ish; the game's mix is
            // done in the mixer, where it can be per-sound and per-setting.
            es8311_voice_volume_set(es, 70, NULL);
        }
    }
    Wire.setClock(1000000);

    if (!ok) { Serial.println("vg_audio: ES8311 init failed"); return false; }

    // Amp on and left on. Gating it per sound is what makes a speaker click, and
    // these cues are short and frequent -- the click would be louder than the cue.
    digitalWrite(SND_PA_PIN, HIGH);
    s_audio_ok = true;
    Serial.printf("vg_audio: ES8311 up @%d Hz\n", VG_AUDIO_RATE);

    return true;
}

// Driven by the CLOCK, and only for the inline path now -- see the note in
// vg_port.h. The buffer-space question has no honest answer through this API, but
// the rate does: 22050 samples a second, however many microseconds have passed.
static uint32_t s_audio_us = 0;

// The delivery instrument. See vg_prof.h for what these are and why blocked time
// is the number to read rather than a modelled queue depth.
uint32_t g_audio_blocked_us = 0;
uint32_t g_audio_short      = 0;

int vg_audio_due(void) {
    if (!s_audio_ok) return 0;

    const uint32_t now = micros();
    if (s_audio_us == 0) {
        s_audio_us = now;
        return 512;              // prime the buffer with some slack to run on
    }
    const uint32_t dt = now - s_audio_us;
    s_audio_us = now;

    int n = (int)(((uint64_t)dt * VG_AUDIO_RATE) / 1000000u);
    // A long frame gets a GAP, not a stall. Writing the whole backlog would
    // block until the DMA drained it, which is the frame paying for the audio
    // instead of the other way round.
    if (n > 1024) n = 1024;
    return n;
}

// The one place that talks to the codec. `wait_ms` is how long it may sit there
// when the DMA ring is full: zero for anything on the render thread, and a real
// timeout for the audio task, which is on the other core with nothing better to
// do than wait.
//
// The timeout is per-call state shared by both callers, which is safe for exactly
// one reason: vg_sfx_update parks the audio task and waits for the acknowledgement
// before the render thread touches any of this. The same handshake that stops both
// sides mixing at once stops both sides setting this.
static int audio_push(const int16_t* samples, int n, uint32_t wait_ms) {
    if (!s_audio_ok || n <= 0) return 0;

    s_i2s.setTimeout(wait_ms);
    // Doubled into stereo in blocks rather than one sample at a time: a call per
    // sample would spend more time in the driver than on the arithmetic that
    // produced them.
    static int16_t st[128 * 2];
    int done = 0;
    while (done < n) {
        int take = n - done;
        if (take > 128) take = 128;
        for (int i = 0; i < take; i++) {
            st[i * 2]     = samples[done + i];
            st[i * 2 + 1] = samples[done + i];
        }
        const size_t want = (size_t)take * 4;
        const size_t got  = s_i2s.write((uint8_t*)st, want);
        done += (int)(got / 4);
        // A short write is also an ESP_LOGE inside this driver -- once per call,
        // over USB CDC, on whichever core asked. Which is why the design above it
        // is built to not produce them rather than to tolerate them.
        if (got < want) break;
    }
    return done;
}

int vg_audio_write(const int16_t* samples, int n) {
    return audio_push(samples, n, 0);
}

// LET THE CODEC SET THE PACE. This is the one that made the crackle go away, and
// what it does is wait.
//
// Every earlier attempt had something on this side of the seam deciding how many
// samples were owed -- from a clock, from a modelled ring depth, from a target
// that ratcheted. All of them were guessing at a number the hardware already
// knows, and the guesses failed in different directions: a queue that could only
// ever get shallower, a target that ratcheted to zero and silenced the board, a
// holding buffer that turned every pass into a logged error.
//
// The codec knows. Hand it a fixed chunk and let the write return when there is
// room, and the ring runs as full as it can, the loop is paced by the sample clock
// itself, and no part of this file has to model anything. The only reason this was
// not always the answer is that the write used to be on the render thread, where
// waiting would have been the frame paying for the audio -- the note in vg_port.h
// about a two-minute frozen screen is what that cost. It is on core 0 now.
//
// 50 ms rather than forever: if the codec ever stops draining, the task should
// come back and find out rather than disappear into the driver.
#define VG_AUDIO_WAIT_MS 50

int vg_audio_write_paced(const int16_t* samples, int n) {
    const uint32_t t0 = micros();
    const int done = audio_push(samples, n, VG_AUDIO_WAIT_MS);
    // Wall time inside the write. Nearly all of it is the wait when things are
    // healthy; the copy and the driver's own work are tens of microseconds.
    g_audio_blocked_us += micros() - t0;
    g_audio_short      += (uint32_t)(n - done);
    return done;
}
