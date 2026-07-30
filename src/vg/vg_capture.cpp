#include "vg_capture.h"
#include "vg_config.h"
#include "vg_replay.h"
#include <Arduino.h>
#include <esp_heap_caps.h>

// Off, or streaming. There used to be two capture modes the host could arm
// directly and both are gone, because neither could hold 60fps and 60fps is the
// point of the project. Live stalled the loop on every send and dropped the game
// to 15; smooth ran the simulation on a fake clock, which is 60fps video of a
// board in slow motion. Streaming is now driven only by replay, where the frame
// being sent was already produced at a real 60fps and is merely being read back.
static int      s_mode  = 0;
static uint32_t s_index = 0;

// Runs are emitted through a staging buffer rather than written one at a time.
// Serial.write per pair would spend more time in the driver than on the wire.
static uint8_t  s_buf[8192];
static int      s_len = 0;

static inline void flush(void);

bool  vg_capture_active(void) { return s_mode != 0; }

// Does NOT discard the staging buffer. Zeroing s_len here throws away bytes
// whose band header has already gone out claiming them, and because those bytes
// never reach vg_link_write they are missing from the device's own byte count
// too -- the loss is invisible from both ends at once.
void vg_capture_set(int mode) {
    // Drain before going idle. The host must have every byte of the last frame
    // before the device reports that the session finished.
    s_mode  = mode;
    s_index = 0;
}
bool  vg_link_busy(void) { return s_mode != 0 || vg_replay_mode() != VG_RP_OFF; }

// A blocking write is correct while a session runs and wrong at every other
// time. During a session a host is reading, and a dropped byte truncates a frame
// it then waits for. Outside a session nobody is reading, and a blocking write
// freezes the game -- see the note in setup().
//
// Replay owns this, because replay is what knows a session has started. It has
// to be on before the first announce: the host waits for "PLAYING", and with
// writes non-blocking that line was dropped and the render never began.
void vg_link_blocking(bool on) { Serial.setTxTimeoutMs(on ? 5000 : 0); }

// Serial.write RETURNS A COUNT, and it is not always the count you asked for.
// The USB CDC ring is finite and the call gives up after a timeout, so a busy
// link produces a short write -- and ignoring that silently discards the tail.
//
// This is what made replay die on a different frame every run: a 35KB frame
// through a 16KB ring fills it twice, and whenever the host paused, a few
// hundred bytes of the last band evaporated. The band header had already
// declared a length the device then failed to deliver, so the host sat waiting
// for bytes that no longer existed while the device moved on to the next frame.
// Both sides behaving perfectly, deadlocked.
static uint32_t s_wr_bytes = 0, s_wr_short = 0, s_wr_stall = 0;
static uint32_t s_band_mismatch = 0;
static uint32_t s_begins = 0, s_ends = 0;

void vg_link_write(const void* p, int n) {
    const uint8_t* b = (const uint8_t*)p;
    s_wr_bytes += (uint32_t)n;
    // Bounded. A host that stops reading mid-capture used to spin here for ever,
    // and a device that hangs is worse than a capture that desyncs -- the host
    // resynchronises on the next frame, and the player gets their game back.
    int spins = 0;
    while (n > 0) {
        const size_t k = Serial.write(b, (size_t)n);
        if (k < (size_t)n) s_wr_short++;
        if (k == 0) {
            s_wr_stall++;
            if (++spins > 3000) return;      // ~3s, then give up on these bytes
            delay(1);
            continue;
        }
        spins = 0;
        b += k;
        n -= (int)k;
    }
}

void vg_link_stats(uint32_t* bytes, uint32_t* shorts, uint32_t* stalls,
                   uint32_t* mismatch) {
    *bytes = s_wr_bytes; *shorts = s_wr_short; *stalls = s_wr_stall;
    *mismatch = s_band_mismatch;
}

void vg_capture_frame_counts(uint32_t* begins, uint32_t* ends) {
    *begins = s_begins; *ends = s_ends;
}

void vg_link_stats_reset(void) {
    s_wr_bytes = s_wr_short = s_wr_stall = s_band_mismatch = 0;
    s_begins = s_ends = 0;
}

static inline void put(const void* p, int n) {
    const uint8_t* b = (const uint8_t*)p;
    while (n > 0) {
        const int room = (int)sizeof(s_buf) - s_len;
        const int take = (n < room) ? n : room;
        memcpy(s_buf + s_len, b, (size_t)take);
        s_len += take;
        b     += take;
        n     -= take;
        if (s_len == (int)sizeof(s_buf)) flush();
    }
}

static inline void flush(void) {
    if (s_len) { vg_link_write(s_buf, s_len); s_len = 0; }
}

void vg_capture_poll(void) {
    while (Serial.available()) {
        const int c = Serial.read();
        // Replay commands first: they read their own payload straight off the
        // stream, so they must not be mistaken for capture bytes.
        if (vg_replay_command(c)) continue;
        if (c == 's' && s_mode) {
            s_mode = 0;
            Serial.printf("\nvg_capture: DONE %u frames\n", (unsigned)s_index);
        } else if (c == 'b' && !s_mode) {
            // Link benchmark. Blasts a fixed blob with no rendering and no
            // encoding, so the number that comes back is the write path alone.
            // Worth having permanently: capture throughput is the whole
            // constraint on this feature, and "is it the wire or is it us"
            // cannot be answered from the host side.
            // Derive the total from sizeof(s_buf). It was hard-coded at 1MB,
            // and when the staging buffer grew from 4K to 8K the loop started
            // sending 2MB while the report still divided by one. The rate came
            // back at half the truth, which is the kind of number that gets
            // believed and then reasoned from.
            memset(s_buf, 0xA5, sizeof(s_buf));
            const uint32_t total = 256u * (uint32_t)sizeof(s_buf);
            const uint32_t t0 = micros();
            for (int i = 0; i < 256; i++) vg_link_write(s_buf, (int)sizeof(s_buf));
            const uint32_t el = micros() - t0;
            Serial.printf("\nvg_capture: BENCH %u KB in %u us = %.3f MB/s\n",
                          (unsigned)(total / 1024u), (unsigned)el,
                          (float)total / (float)el);
        }
    }
}

void vg_capture_frame_begin(void) {
    if (!s_mode) return;
    s_begins++;
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

// A colour table for one band, so a run costs 2 bytes instead of 3.
//
// A run is a count and a colour: one byte and two bytes. Almost all of a band
// is a few colours repeated, so the colour is the part worth removing. Measured
// over 900 bands of real play: the median band holds 19 different colours and
// the largest holds 87. No band went above 256, so an 8-bit index always fits.
// A frame goes from 36.7 KB to 25.3 KB, which is 1.45 times smaller.
//
// The table is an open-addressing hash with 512 slots for at most 256 colours,
// so a free slot always exists and the probe always ends. It lives in internal
// RAM. PSRAM is what made the delta method slower than the bytes it saved.
#define PAL_SLOTS 512

static uint16_t s_pal_key[PAL_SLOTS];
static uint8_t  s_pal_val[PAL_SLOTS];
static uint8_t  s_pal_used[PAL_SLOTS];
static uint16_t s_pal_list[256];
static int      s_pal_n;

static inline int pal_slot(uint16_t c) {
    uint32_t hsh = ((uint32_t)c * 2654435761u) >> 23;
    int i = (int)(hsh & (PAL_SLOTS - 1));
    while (s_pal_used[i] && s_pal_key[i] != c) i = (i + 1) & (PAL_SLOTS - 1);
    return i;
}

// The index of this colour. Adds the colour if it is new. -1 if the table is
// full, and then the caller sends the band with a colour in every run.
static inline int pal_get(uint16_t c) {
    const int i = pal_slot(c);
    if (s_pal_used[i]) return s_pal_val[i];
    if (s_pal_n >= 256) return -1;
    s_pal_used[i]  = 1;
    s_pal_key[i]   = c;
    s_pal_val[i]   = (uint8_t)s_pal_n;
    s_pal_list[s_pal_n] = c;
    return s_pal_n++;
}

void vg_capture_band(int y, int h, const uint16_t* px) {
    if (!s_mode) return;

    const int n = SCR_W * h;

    // Pass 1: count the runs and collect the colours.
    memset(s_pal_used, 0, sizeof(s_pal_used));
    s_pal_n = 0;
    int pruns = 0;
    bool paletted = true;
    for (int i = 0; i < n; ) {
        const uint16_t v = px[i];
        int run = 1;
        while (i + run < n && px[i + run] == v && run < 255) run++;
        if (pal_get(v) < 0) { paletted = false; break; }
        i += run;
        pruns++;
    }

    if (paletted) {
        const uint8_t  hdr[4] = { 'P', 'H', 'B', 'P' };
        const uint16_t sy = (uint16_t)y, sh = (uint16_t)h;
        const uint16_t ncol = (uint16_t)s_pal_n;
        const uint32_t bytes = (uint32_t)pruns * 2u;
        put(hdr, 4);
        put(&sy, 2);
        put(&sh, 2);
        put(&ncol, 2);
        put(&bytes, 4);
        put(s_pal_list, s_pal_n * 2);

        for (int i = 0; i < n; ) {
            const uint16_t v = px[i];
            int run = 1;
            while (i + run < n && px[i + run] == v && run < 255) run++;
            const uint8_t pair[2] = { (uint8_t)run, (uint8_t)s_pal_val[pal_slot(v)] };
            put(pair, 2);
            i += run;
        }
        flush();
        return;
    }
    // More than 256 colours in one band. Not seen in 900 sampled bands, but the
    // path below sends the colour in every run and is always correct.

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

    int emitted = 0;
    for (int i = 0; i < n; ) {
        const uint16_t v = px[i];
        int run = 1;
        while (i + run < n && px[i + run] == v && run < 255) run++;
        const uint8_t trio[3] = { (uint8_t)run, (uint8_t)(v & 0xFF), (uint8_t)(v >> 8) };
        put(trio, 3);
        i += run;
        emitted++;
    }

    // Out at the end of every band, so the staging buffer never carries one
    // band's payload into the next. It is the same bytes either way, and it
    // means a band is either wholly sent or not started.
    flush();

    // The header promised `runs`; anything else and the host is left waiting on
    // bytes that will never come, which is indistinguishable from a dead link.
    // The two passes read the same buffer, so this can only fire if that buffer
    // changed underneath us -- worth knowing rather than assuming.
    if (emitted != runs) s_band_mismatch++;
}

void vg_capture_frame_end(void) {
    if (!s_mode) return;
    s_ends++;
    const uint8_t hdr[4] = { 'P', 'H', 'E', 'N' };
    put(hdr, 4);
    put(&s_index, 4);
    flush();
    // Push the frame out before returning. Without this the last bytes wait in
    // the USB driver until more traffic moves them, and the host waits for
    // those bytes, and neither side continues.
    Serial.flush();
    s_index++;
}
