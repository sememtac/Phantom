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

// CO5300 registers
#define CO_SLPOUT    0x11
#define CO_INVOFF    0x20
#define CO_DISPON    0x29
#define CO_CASET     0x2A
#define CO_PASET     0x2B
#define CO_RAMWR     0x2C
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
    co_window(0, y, SCR_W - 1, y + h - 1);

    spi_transaction_t* t = &s_tx[s_tx_slot];
    s_tx_slot ^= 1;

    memset(t, 0, sizeof(*t));
    t->flags     = SPI_TRANS_MODE_QIO;   // data on 4 lines; cmd/addr single
    t->cmd       = 0x32;
    t->addr      = ((uint32_t)CO_RAMWR) << 8;
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
    // Shared I2C for touch + IMU. 400 kHz keeps the per-frame touch read down
    // around 0.5 ms; at the 100 kHz default it costs several percent of a frame.
    Wire.begin(IIC_SDA, IIC_SCL);
    Wire.setClock(400000);

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
// IMU
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
