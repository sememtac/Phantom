#include "vg_capture.h"
#include "esp_task_wdt.h"
#include "vg_config.h"
#include "vg_replay.h"
#include "vg_crumb.h"
#include "vg_sim.h"     // the VFX bench commands
#include "vg_raster.h"  // the 'q' antialiasing toggle
#include "vg_canopy_set.h"  // the 'k' bench selects a drawing to measure
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_log.h>

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

// Set by 'd', cleared by the read. One window's worth, not a mode.
static bool s_detail = false;
bool vg_capture_want_detail(void) {
    const bool w = s_detail;
    s_detail = false;
    return w;
}

// A blocking write is correct while a session runs and wrong at every other
// time. During a session a host is reading, and a dropped byte truncates a frame
// it then waits for. Outside a session nobody is reading, and a blocking write
// freezes the game -- see the note in setup().
//
// Replay owns this, because replay is what knows a session has started. It has
// to be on before the first announce: the host waits for "PLAYING", and with
// writes non-blocking that line was dropped and the render never began.
void vg_link_blocking(bool on) { Serial.setTxTimeoutMs(on ? 5000 : 0); }

// NOTHING WRITES TO THE PORT WHILE A SESSION OWNS IT -- including the core.
//
// Every deliberate print in this firmware is already wrapped in `if (!vg_link_busy())`. The
// Arduino core's own logging is not, and it goes to the same Serial. That is not theoretical:
// the I2S driver logs ESP_ERR_TIMEOUT at ESP_LOG_ERROR every 22 ms while the audio ring
// settles after a reset, and a record starting in that window took one frame and then read
// `[  2900][E][ESP_I2S.cpp` where a frame record should have been.
//
// It survived for a long time by luck. The host scans for the session header before it starts
// reading frames, so an error burst that arrived BEFORE the header was skipped. Making the
// game faster moved the header in front of the burst and the luck ran out.
//
// A sink rather than a log level, because it needs no knowing when to turn off and it catches
// every component at once -- a driver added next year is covered without anyone remembering
// this. The output is dropped, not buffered: it is diagnostic text, the session is not, and a
// queue that grows while a two minute render runs is a leak with a good excuse.
static int (*s_log_prev)(const char*, va_list) = nullptr;

static int log_sink(const char* fmt, va_list ap) {
    if (vg_link_busy()) return 0;
    return s_log_prev ? s_log_prev(fmt, ap) : 0;
}

void vg_link_guard_logs(void) {
    if (!s_log_prev) s_log_prev = esp_log_set_vprintf(log_sink);
}

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

static uint32_t s_wr_giveups = 0;

// Whether this host wants audio in the stream. Off until it says so; see the
// 'A' command.
static int s_audio_on = 0;

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
            if (++spins > 3000) { s_wr_giveups++; return; }  // ~3s, then drop it
            // Waiting on a host is not a hang. Bounded at three seconds, well
            // inside the dog's ten, but fed anyway so the bound is the only
            // thing that ends it.
            esp_task_wdt_reset();
            delay(1);
            continue;
        }
        spins = 0;
        b += k;
        n -= (int)k;
    }
    s_wr_giveups = 0;       // a write that completed means somebody is reading
}

// A host that has stopped reading. Bounding each write was not enough on its own:
// the session stayed armed, so EVERY frame paid three seconds a band for ever,
// which is a dead machine rather than a slow one.
//
// FOUR, not two. Two killed a perfectly healthy recording: a host that is merely
// busy for six seconds -- writing a file, or garbage collecting -- looks exactly
// like a host that has gone, and the session died under it. Twelve seconds does
// not, and the thing being prevented is a freeze that lasts until the board is
// unplugged, so the cost of waiting a little longer to be sure is nothing.
#define LINK_DEAD_GIVEUPS 4
static bool link_dead(void) { return s_wr_giveups >= LINK_DEAD_GIVEUPS; }

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

// --- the band checksum -----------------------------------------------------
//
// A LENGTH CHECK IS NOT ENOUGH, and believing it was cost an entire evening.
//
// The host already refuses a band whose runs do not add up to the right number
// of pixels, or that names a colour outside its own table. What neither test can
// see is a byte flipped INSIDE a colour value, or a run length that happens to
// still sum correctly. Those decode cleanly to the wrong picture.
//
// The symptom is brutal to diagnose: two renders of one recording come back with
// different frames, in a scattered pattern, and it reads as a simulation that is
// not reproducible. It is not -- and the giveaway was that some frames MATCHED
// again afterwards, which a diverged simulation can never do. Without a checksum
// there was no way to tell a wire fault from a game fault, so the whole replay
// harness rested on nothing.
//
// Adler-32, because it is two adds and two compares a byte, and because the host
// gets it from zlib for free and so cannot disagree about the algorithm.
static uint32_t s_ck_a, s_ck_b;

static inline void ck_reset(void) { s_ck_a = 1; s_ck_b = 0; }

static inline void ck_add(const void* p, int n) {
    const uint8_t* b = (const uint8_t*)p;
    uint32_t a = s_ck_a, c = s_ck_b;
    while (n-- > 0) {
        a += *b++;      if (a >= 65521u) a -= 65521u;
        c += a;         if (c >= 65521u) c -= 65521u;
    }
    s_ck_a = a; s_ck_b = c;
}

static inline uint32_t ck_value(void) { return (s_ck_b << 16) | s_ck_a; }

// Everything the host has to reconstruct the band from goes through here.
static inline void putck(const void* p, int n) { ck_add(p, n); put(p, n); }

void vg_capture_poll(void) {
    // THE GAME MUST NEVER BE LEFT BLOCKING ON A LINK NOBODY IS READING.
    //
    // A blocking write is correct only while a session is running, because only
    // then is a host draining the port. Turning it back off is the 'E' command's
    // job -- and a host that dies, disconnects, or aborts mid-record never sends
    // one. The timeout then stays at five seconds forever, and with a capture
    // still armed that is fifteen band writes of five seconds each: over a
    // minute per frame, which is not a hitch, it is a dead machine.
    //
    // That is not hypothetical. It is the best explanation on the evidence for a
    // 130-second frame this recorded in the launch cutscene, and the host that
    // aborted was our own recording tool.
    //
    // Cheap enough to assert every frame, and an invariant beats remembering.
    if (!s_mode && vg_replay_mode() == VG_RP_OFF) vg_link_blocking(false);

    while (Serial.available()) {
        const int c = Serial.read();
        // Replay commands first: they read their own payload straight off the
        // stream, so they must not be mistaken for capture bytes.
        if (vg_replay_command(c)) {
            // AND STOP DRAINING THE MOMENT A REPLAY BEGINS.
            //
            // begin_play() runs inside this loop and announces PLAYING from
            // inside it. The host is waiting for exactly that line and starts
            // sending frame entries the instant it arrives -- while this loop is
            // still going. Those entries then get read HERE, one byte at a time,
            // as commands: 'P' is refused because the mode is no longer OFF, and
            // 'H', 'R', the length and the whole input structure fall through as
            // unknown bytes and are silently dropped.
            //
            // The device then sits in vg_replay_next waiting for an entry the
            // host believes it already sent, the host sits in read_frame waiting
            // for a frame, and the render dies with "link went quiet" having
            // never produced a single frame. From outside it looks exactly like
            // the device ignoring 'P'.
            //
            // Entries belong to vg_replay_next. Once it owns the stream, this
            // loop must not touch another byte.
            if (vg_replay_mode() != VG_RP_OFF) break;
            continue;
        }
        if (c == 'q') {
            // ANTIALIASING, ON AND OFF, WHILE FLYING. A compile-time switch would
            // mean comparing two flights from memory, which is no way to judge how
            // something looks -- flipping it mid-fight puts the two versions of the
            // same scene a second apart.
            const bool on = !vg_rast_aa_master_on();
            vg_rast_aa_master(on);
            if (!vg_link_busy()) Serial.println(on ? "hud aa: on" : "hud aa: off");
        } else if (c == 'd' && !vg_link_busy()) {
            // The full breakdown, once, on the next telemetry window. See
            // vg_capture_want_detail -- these lines are asked for, not broadcast.
            s_detail = true;
        } else if (c == 'c' && !vg_link_busy()) {
            // The last timed replay's cost, on demand. See vg_replay_report_cost: catching
            // it as it went past was a race the host lost every time.
            if (!vg_replay_report_cost()) Serial.println("vg_replay: no timed run yet");
        } else if (c == 'k' && !vg_link_busy()) {
            // WHAT THE CANOPY COSTS, from anywhere -- the attract loop included.
            //
            // The drawing only appears in a match, so the frame counter needs a pilot,
            // and it reports a band's slower half rather than the pass. This is the
            // whole pass on one core, so a change to the table or the inner loop can be
            // judged in one flash instead of one flight.
            //
            // AND "FROM ANYWHERE" WAS NOT TRUE, which is what this selection is for.
            //
            // vg_canopy_bench zeroes its output and returns when no drawing is selected,
            // and on a menu none is: a canopy is chosen on the way into flight. So the
            // bench answered every question asked from the title screen with zeros, and
            // the host tool graded them and printed "there is room for it". A tool that
            // says a too-heavy drawing is fine is worse than no tool, and this one said it
            // three times in a row after three separate flashes.
            //
            // So the bench is given something to measure. The saved profile's hull first,
            // because that is the one the author is flying; failing that, whichever hull
            // has a drawing at all, so a reading taken right after a flash still lands.
            // The hull is named in the output either way -- one number for a drawing is
            // only meaningful next to which drawing it was.
            const VgCanopy* prev = vg_canopy_current();
            const VgCanopy* use  = prev;
            ShipClass       hull = vg.ship;
            if (!use) {
                use = vg_canopy_for(vg.ship);
                for (int i = 0; !use && i < SHIP_CLASSES; i++) {
                    use = vg_canopy_for((ShipClass)i);
                    if (use) hull = (ShipClass)i;
                }
            }
            // Swapped and put back. Safe here and only here: vg_capture_poll runs at the
            // top of the frame, so the previous frame's raster has finished with s_can and
            // the next one has not started. The restore costs one lazy LUT rebuild.
            if (use != prev) vg_canopy_use(use);
            VgCanopyCost k;
            vg_canopy_bench(&k);
            if (use != prev) vg_canopy_use(prev);
            if (!vg_link_busy()) {
                // `hull` goes LAST. The host reads the microseconds as the second
                // whitespace-separated token, so nothing may be inserted before them.
                Serial.printf("canopy: %lu us rigid, %lu us warped, %lu us intro, %d blocks, "
                              "%d flat px, %d literal px, hull %s\n",
                              (unsigned long)k.us, (unsigned long)k.warp_us,
                              (unsigned long)k.intro_us,
                              k.blocks, k.flat_px, k.lit_px,
                              use ? vg_spec(hull)->name : "none");
            }
        } else if (c == 'y' && !vg_link_busy()) {
            // 'y', NOT 's'. The host sends 's' to stop a capture -- phantom_link.close does
            // it unconditionally -- and there is an `else if (c == 's' && s_mode)` further
            // down that has done that since long before these benches existed. Claiming 's'
            // here shadowed it: a recording could no longer be stopped, and the byte meant to
            // stop it regenerated the backdrop and refilled a live band buffer instead. One
            // frame came back.
            //
            // THE BACKDROP, split into prep and fill, with a checksum.
            //
            // The checksum is what makes this safe to optimise against: the fill must come
            // out bit-identical, and its own comments record two changes that were chosen
            // because they could not move a rounding. A number that has to match is worth
            // more than a careful reading of the diff.
            VgSkyCost s;
            vg_sky_bench(&s);
            if (!vg_link_busy()) {
                Serial.printf("sky: prep %lu us, fill %lu us, tinted %lu us, sum %08lx\n",
                              (unsigned long)s.prep_us, (unsigned long)s.fill_us,
                              (unsigned long)s.tint_us,
                              (unsigned long)s.sum);
            }
        } else if (c == 'g' && !vg_link_busy()) {
            // The glyph nest, plain against hoisted, over identical text. `same` is the
            // part that matters: a faster loop that draws different pixels is not faster.
            VgGlyphCost k{};
            k.same = true;
            vg_glyph_bench(&k);
            if (!vg_link_busy()) {
                Serial.printf("glyph: ref %lu us, now %lu us, %d glyphs x %d bands, %s\n",
                              (unsigned long)k.ref_us, (unsigned long)k.now_us, k.glyphs,
                              (int)NUM_BANDS, k.same ? "IDENTICAL" : "DIFFERENT");
            }
        } else if (c == 'l' && !vg_link_busy()) {
            VgLineCost k{};
            vg_line_bench(&k);
            if (!vg_link_busy()) {
                Serial.printf("line: ref %lu us, now %lu us, %d lines, %ld px, %s\n",
                              (unsigned long)k.ref_us, (unsigned long)k.now_us,
                              k.lines, k.px, k.same ? "IDENTICAL" : "DIFFERENT");
            }
        } else if (c == 'm' && !vg_link_busy()) {
            // WHAT THE BLENDED PATH COSTS, and what the branchless pair would buy.
            //
            // 'l' above benches the OPAQUE walk. This is the other one: lit HUD lines
            // through plot_delta and canopy members through blend_px, both of which still
            // do the naive per-channel form that px_add replaced in the canopy for 14.7%.
            //
            // `now` here is NOT what ships -- it is a candidate held inside the bench, so
            // this prices the open target without the game running a line of it. Read
            // IDENTICAL as the licence to adopt it: it is a memcmp of both banks after a
            // fan that saturates and borrows in every field, and a faster blend that moves
            // one pixel is not faster.
            VgBlendCost k{};
            vg_blend_bench(&k);
            if (!vg_link_busy()) {
                Serial.printf("blend: line ref %lu us now %lu us | span ref %lu us now %lu us"
                              " | %d lines %ld line px %ld span px, %s\n",
                              (unsigned long)k.line_ref_us, (unsigned long)k.line_now_us,
                              (unsigned long)k.span_ref_us, (unsigned long)k.span_now_us,
                              k.lines, k.line_px, k.span_px,
                              k.same ? "IDENTICAL" : "DIFFERENT");
            }
        } else if (c == 'A') {
            // ASKED FOR, never assumed. Audio in the capture stream is an extra
            // chunk inside each frame, and a host that predates it treats an
            // unknown tag as a desync -- so it drops every frame and renders
            // nothing while its progress bar sits still, which looks exactly
            // like a hang and reports nothing.
            //
            // That is not hypothetical: the packaged recorder is built ahead of
            // time and was three days older than the change. Sending only when
            // asked means an old host is simply an old host.
            s_audio_on = 1;
        } else if (c == '!') {
            // REBOOT, because the host cannot.
            //
            // This board is an ESP32-S3 on native USB CDC: DTR and RTS are
            // virtual and drive nothing, so the auto-reset circuit every other
            // ESP32 tool relies on does not exist here. reset_board() in
            // phantom_link.py pulsed RTS for months and only ever slept.
            //
            // That is not cosmetic. A render has to start from a known state or
            // the simulation it replays is not the one that was recorded, and
            // without a reset it started from wherever the game happened to be
            // -- which is why the same session rendered to three different
            // pictures on three runs and looked like a determinism bug.
            Serial.println("\nvg: rebooting");
            Serial.flush();
            delay(30);          // let the line drain before the world stops
            esp_restart();
        } else if (c == 'z') {
            // Forget the diagnostic record. Wanted from the host because the
            // alternative was building and flashing a one-line firmware to clear
            // a stale worst-case, which is how the last one got cleared.
            vg_crumb_reset();
            Serial.println("\nvg_crumb: cleared");
        } else if ((c == 'x' || c == 'X') && !s_mode
                   && vg_replay_mode() == VG_RP_OFF) {
            // The VFX bench. Barred during a capture or a replay: it draws from
            // the seeded RNG, so firing one mid-replay would put the simulation
            // off the sequence the recording was made from.
            if (c == 'x') {
                const int w = vg_vfx_step_preset();
                vg_vfx_fire(w);
                Serial.printf("\nvg_vfx: %s\n", vg_vfx_name(w));
            } else {
                const bool on = (vg_vfx_auto_period() <= 0.0f);
                vg_vfx_auto(on ? 1.6f : 0.0f);
                Serial.printf("\nvg_vfx: auto %s\n", on ? "ON, every 1.6s" : "OFF");
            }
        } else if (c == 's' && s_mode) {
            s_mode = 0;
            s_audio_on = 0;
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

    // The host is gone. End the session rather than spending the rest of the run
    // three seconds a band talking to nobody -- a permanent freeze from the
    // player's side, and indistinguishable from a crash.
    //
    // Replay is left alone: it is driven by the host asking for each frame, so a
    // host that stopped simply stops asking, and vg_replay has its own timeout.
    if (link_dead()) {
        s_mode = 0;
        s_len  = 0;
        vg_link_blocking(false);
        return;
    }
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
        ck_reset();
        putck(s_pal_list, s_pal_n * 2);

        for (int i = 0; i < n; ) {
            const uint16_t v = px[i];
            int run = 1;
            while (i + run < n && px[i + run] == v && run < 255) run++;
            const uint8_t pair[2] = { (uint8_t)run, (uint8_t)s_pal_val[pal_slot(v)] };
            putck(pair, 2);
            i += run;
        }
        const uint32_t ck = ck_value();   // over the palette AND the runs
        put(&ck, 4);
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
    ck_reset();

    int emitted = 0;
    for (int i = 0; i < n; ) {
        const uint16_t v = px[i];
        int run = 1;
        while (i + run < n && px[i + run] == v && run < 255) run++;
        const uint8_t trio[3] = { (uint8_t)run, (uint8_t)(v & 0xFF), (uint8_t)(v >> 8) };
        putck(trio, 3);
        i += run;
        emitted++;
    }
    const uint32_t ck = ck_value();
    put(&ck, 4);

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

// Audio rides INSIDE the frame, as one more chunk the host dispatches on, and it
// is held here until the frame closes rather than written when it arrives.
//
// The mixer runs before vg_rast_flush, which is where the frame is opened and
// closed -- so writing on arrival put the chunk between one frame's end tag and
// the next frame's header, where the host is looking for 'PHFR', finds 'PHAU',
// and rescans. The frames survived that and the audio was silently discarded,
// every frame, with nothing reporting a problem.
static int16_t s_au[512];
static int     s_au_n = 0;

void vg_capture_audio(const int16_t* samples, int n) {
    if (!s_mode || !s_audio_on || n <= 0) return;
    if (n > (int)(sizeof(s_au) / sizeof(s_au[0]))) n = (int)(sizeof(s_au) / sizeof(s_au[0]));
    memcpy(s_au, samples, (size_t)n * 2);
    s_au_n = n;
}

void vg_capture_audio_off(void) { s_audio_on = 0; s_au_n = 0; }

void vg_capture_frame_end(void) {
    if (!s_mode) return;
    s_ends++;

    if (s_au_n > 0) {
        const uint8_t ahdr[4] = { 'P', 'H', 'A', 'U' };
        const uint16_t count = (uint16_t)s_au_n;
        put(ahdr, 4);
        put(&count, 2);
        put(s_au, s_au_n * 2);
        s_au_n = 0;
    }

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
