#include "vg_capture.h"
#include "vg_config.h"
#include <Arduino.h>

// Frames per second the RECORDING plays at. Nothing to do with how fast the
// device manages to send them -- it is only the dt the simulation is stepped
// with, so it decides how much motion happens between captured frames.
#define CAP_FPS   30.0f

// 0 idle, 1 fixed-step, 2 live.
//
// Fixed step gives smooth video of a game running in slow motion: the
// simulation is told a frame took 1/30s however long it really took, so motion
// between captured frames is always the same and the recording is perfect.
//
// Live leaves the clock alone. The game runs at its own pace and whatever gets
// sent is what was genuinely on the panel at that moment, so the recording is
// real time -- at the handful of frames a second the link can carry, because
// every send stalls the loop while it goes out. Choppy and true, against
// smooth and slowed.
static int      s_mode  = 0;
static uint32_t s_index = 0;

// Runs are emitted through a staging buffer rather than written one at a time.
// Serial.write per pair would spend more time in the driver than on the wire.
static uint8_t  s_buf[8192];
static int      s_len = 0;

bool  vg_capture_active(void) { return s_mode != 0; }

// Only fixed-step overrides the clock. Live returns zero, meaning "use real
// time", which is the entire difference between the two modes.
float vg_capture_dt(void)     { return (s_mode == 1) ? (1.0f / CAP_FPS) : 0.0f; }

static inline void put(const void* p, int n) {
    const uint8_t* b = (const uint8_t*)p;
    while (n > 0) {
        const int room = (int)sizeof(s_buf) - s_len;
        const int take = (n < room) ? n : room;
        memcpy(s_buf + s_len, b, (size_t)take);
        s_len += take;
        b     += take;
        n     -= take;
        if (s_len == (int)sizeof(s_buf)) { Serial.write(s_buf, (size_t)s_len); s_len = 0; }
    }
}

static inline void flush(void) {
    if (s_len) { Serial.write(s_buf, (size_t)s_len); s_len = 0; }
}

void vg_capture_poll(void) {
    while (Serial.available()) {
        const int c = Serial.read();
        if ((c == 'c' || c == 'l') && !s_mode) {
            s_mode  = (c == 'l') ? 2 : 1;
            s_index = 0;
            // Announced on a line of its own so the host can sync before any
            // binary arrives, and so a human watching the monitor can see why
            // the game has suddenly gone slow.
            Serial.printf("\nvg_capture: ARMED %s %dx%d rot %d\n",
                          (s_mode == 2) ? "LIVE" : "FIXED",
                          SCR_W, SCR_H, VG_ROTATE);
        } else if (c == 's' && s_mode) {
            s_mode = 0;
            Serial.printf("\nvg_capture: DONE %u frames\n", (unsigned)s_index);
        } else if (c == 'b' && !s_mode) {
            // Link benchmark. Blasts a fixed blob with no rendering and no
            // encoding, so the number that comes back is the write path alone.
            // Worth having permanently: capture throughput is the whole
            // constraint on this feature, and "is it the wire or is it us"
            // cannot be answered from the host side.
            memset(s_buf, 0xA5, sizeof(s_buf));
            const uint32_t t0 = micros();
            for (int i = 0; i < 256; i++) Serial.write(s_buf, sizeof(s_buf));
            const uint32_t el = micros() - t0;
            Serial.printf("\nvg_capture: BENCH 1MB in %u us = %.3f MB/s\n",
                          (unsigned)el, 1048576.0f / (float)el);
        }
    }
}

void vg_capture_frame_begin(void) {
    if (!s_mode) return;
    const uint8_t hdr[4] = { 'P', 'H', 'F', 'R' };
    const uint16_t w = SCR_W, h = SCR_H;
    const uint8_t  rot = VG_ROTATE, fmt = VG_CAP_FMT_RLE16;
    put(hdr, 4);
    put(&s_index, 4);
    put(&w, 2);
    put(&h, 2);
    put(&rot, 1);
    put(&fmt, 1);
}

void vg_capture_band(int y, int h, const uint16_t* px) {
    if (!s_mode) return;

    const int n = SCR_W * h;

    // Run-length over 16-bit pixels. This is worth doing rather than sending
    // raw because of how the backdrop is drawn: the fill writes one sampled
    // colour across eight pixels at a time, so open sky is already runs, and
    // the vector art on top is thin. Typical frames come out three to five
    // times smaller, which is the difference between two captured frames a
    // second and closer to eight.
    //
    // Counted first, then emitted. Staging the encoded band in a scratch buffer
    // so its length could go in the header wanted 46KB of RAM -- worst-case RLE
    // is three bytes per pixel -- which is a sixth of the internal heap given up
    // permanently for a debug feature. Two passes over 15k pixels costs tens of
    // microseconds and the frame is about to spend milliseconds on the wire
    // anyway, so the trade is not close.
    //
    // The length stays in the header regardless: the host needs it to skip a
    // damaged band and resynchronise rather than losing the rest of the run.
    int runs = 0;
    for (int i = 0; i < n; ) {
        const uint16_t v = px[i];
        int run = 1;
        while (i + run < n && px[i + run] == v && run < 255) run++;
        i += run;
        runs++;
    }

    const uint8_t  hdr[4] = { 'P', 'H', 'B', 'D' };
    const uint16_t sy = (uint16_t)y, sh = (uint16_t)h;
    const uint32_t bytes = (uint32_t)runs * 3u;
    put(hdr, 4);
    put(&sy, 2);
    put(&sh, 2);
    put(&bytes, 4);

    for (int i = 0; i < n; ) {
        const uint16_t v = px[i];
        int run = 1;
        while (i + run < n && px[i + run] == v && run < 255) run++;
        const uint8_t trio[3] = { (uint8_t)run, (uint8_t)(v & 0xFF), (uint8_t)(v >> 8) };
        put(trio, 3);
        i += run;
    }
}

void vg_capture_frame_end(void) {
    if (!s_mode) return;
    const uint8_t hdr[4] = { 'P', 'H', 'E', 'N' };
    put(hdr, 4);
    put(&s_index, 4);
    flush();
    s_index++;
}
