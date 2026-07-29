#include "vg_replay.h"
#include "vg_game.h"
#include "vg_ship.h"
#include "vg_capture.h"
#include <Arduino.h>
#include <esp_random.h>
#include <string.h>

// Seeds drawn in a single frame. Two is the real maximum today (a sky
// regenerate takes a kind and a seed); the headroom costs 64 bytes and means a
// future draw cannot silently truncate the log and desynchronise a replay.
#define RP_MAX_RAND 16

// The persisted progress a session starts from. Mirrors what vg_save_load
// applies, rather than the SaveRecord itself -- this only has to survive the
// round trip to the host and back, so it does not need the on-flash layout and
// does not break when that layout changes.
struct RpSave {
    uint16_t credits;
    char     callsign[4];
    uint8_t  ship;
    uint8_t  champion;
    float    hue;
};

static int      s_mode   = VG_RP_OFF;
static uint32_t s_index  = 0;
static uint32_t s_rand[RP_MAX_RAND];
static int      s_rand_n = 0;    // logged this frame (RECORD) / supplied (PLAY)
static int      s_rand_i = 0;    // next to hand out (PLAY)

int  vg_replay_mode(void)          { return s_mode; }
bool vg_replay_suppress_save(void) { return s_mode == VG_RP_PLAY; }

uint32_t vg_replay_rand(void) {
    if (s_mode == VG_RP_PLAY) {
        // Running out means the recording and the playback disagree about how
        // many seeds this frame needs, which is a desync. Returning a constant
        // keeps it reproducible instead of pulling in a fresh hardware value
        // and making the divergence worse every frame.
        return (s_rand_i < s_rand_n) ? s_rand[s_rand_i++] : 0u;
    }
    const uint32_t v = esp_random();
    if (s_mode == VG_RP_RECORD && s_rand_n < RP_MAX_RAND) s_rand[s_rand_n++] = v;
    return v;
}

// --- blocking host reads ---------------------------------------------------

static bool rd(void* p, int n, uint32_t to_ms) {
    uint8_t* b = (uint8_t*)p;
    uint32_t last = millis();
    int got = 0;
    while (got < n) {
        if (Serial.available()) { b[got++] = (uint8_t)Serial.read(); last = millis(); }
        else if (millis() - last > to_ms) return false;
        // Yield rather than spin. This loop is the whole frame budget while
        // replaying, and starving the USB driver task would stop the very
        // bytes we are waiting to be answered.
        else delay(0);
    }
    return true;
}

// --- progress snapshot -----------------------------------------------------

static void snapshot(RpSave* s) {
    s->credits  = (uint16_t)((vg.credits < 0) ? 0 : vg.credits);
    memcpy(s->callsign, vg.callsign, 4);
    s->ship     = (uint8_t)vg.ship;
    s->champion = vg.champion ? 1u : 0u;
    s->hue      = vg.trail_hue;
}

static void restore(const RpSave* s) {
    vg.credits = (int)s->credits;
    for (int i = 0; i < 3; i++) {
        const char c = s->callsign[i];
        vg.callsign[i] = (c >= 'A' && c <= 'Z') ? c : 'A';
    }
    vg.callsign[3] = 0;
    vg.ship        = (s->ship < SHIP_CLASSES) ? (ShipClass)s->ship : SHIP_AEGIS;
    vg.spec        = vg_spec(vg.ship);
    vg.champion    = (s->champion != 0);
    vg.trail_hue   = s->hue;
    // Hull follows the class, exactly as loading a save does.
    vg.health_max  = vg.spec->hull;
    vg.health      = vg.health_max;
}

// --- record ----------------------------------------------------------------

static void begin_record(void) {
    s_mode   = VG_RP_RECORD;
    s_index  = 0;
    s_rand_n = 0;

    // A session must start from a state the replay can reach. Mid-game is not
    // one: the bracket, the ship, the arena and every position would have to be
    // serialised. Restarting means the whole session is reproducible from a
    // seed list and an input log, which is a few hundred bytes.
    vg_game_init();

    RpSave sv;
    snapshot(&sv);

    const uint16_t ver = 1, blob = (uint16_t)sizeof(VgInput);
    const uint8_t  nr  = (uint8_t)s_rand_n;
    vg_link_write("PHRH", 4);
    vg_link_write(&ver, 2);
    vg_link_write(&blob, 2);
    vg_link_write(&nr, 1);
    vg_link_write(s_rand, (int)nr * 4);
    vg_link_write(&sv, (int)sizeof(sv));
    Serial.flush();

    s_rand_n = 0;                 // per-frame log starts clean
}

void vg_replay_note_frame(float dt, const VgInput* in) {
    if (s_mode != VG_RP_RECORD) return;
    const uint8_t nr = (uint8_t)s_rand_n;
    vg_link_write("PHRC", 4);
    vg_link_write(&s_index, 4);
    vg_link_write(&dt, 4);
    vg_link_write(&nr, 1);
    vg_link_write(s_rand, (int)nr * 4);
    vg_link_write(in, (int)sizeof(VgInput));
    s_index++;
    s_rand_n = 0;
}

// --- play ------------------------------------------------------------------

static void begin_play(void) {
    uint16_t ver = 0, blob = 0;
    uint8_t  nr  = 0;
    RpSave   sv;

    if (!rd(&ver, 2, 3000) || !rd(&blob, 2, 3000) || !rd(&nr, 1, 3000)) return;
    if (ver != 1 || blob != sizeof(VgInput) || nr > RP_MAX_RAND) {
        // Refusing is the point. A session recorded against a different VgInput
        // layout would replay as garbage input rather than as an error.
        Serial.printf("\nvg_replay: REJECT ver %u blob %u (want 1/%u)\n",
                      (unsigned)ver, (unsigned)blob, (unsigned)sizeof(VgInput));
        return;
    }
    if (nr && !rd(s_rand, (int)nr * 4, 3000)) return;
    if (!rd(&sv, sizeof(sv), 3000)) return;

    s_rand_n = nr;
    s_rand_i = 0;
    s_mode   = VG_RP_PLAY;

    vg_game_init();               // consumes the recorded seeds
    restore(&sv);

    s_index = 0;
    s_rand_n = 0;
    vg_link_stats_reset();
    vg_capture_set(VG_CAP_STREAM);   // every replayed frame is streamed
    Serial.printf("\nvg_replay: PLAYING\n");
}

bool vg_replay_next(float* dt, VgInput* in) {
    if (s_mode != VG_RP_PLAY) return false;

    char tag[4];
    // Generous: the host has to encode and write the previous frame before it
    // asks for the next one, and a slow disk should not end the session.
    if (!rd(tag, 4, 30000)) { vg_replay_command('E'); return false; }
    if (memcmp(tag, "PHRP", 4) != 0) { vg_replay_command('E'); return false; }

    uint8_t nr = 0;
    if (!rd(dt, 4, 5000) || !rd(&nr, 1, 5000) || nr > RP_MAX_RAND) {
        vg_replay_command('E');
        return false;
    }
    if (nr && !rd(s_rand, (int)nr * 4, 5000)) { vg_replay_command('E'); return false; }
    if (!rd(in, sizeof(VgInput), 5000))       { vg_replay_command('E'); return false; }

    s_rand_n = nr;
    s_rand_i = 0;
    s_index++;
    return true;
}

// --- command ---------------------------------------------------------------

bool vg_replay_command(int c) {
    if (c == 'R' && s_mode == VG_RP_OFF) {
        begin_record();
        return true;
    }
    if (c == 'P' && s_mode == VG_RP_OFF) {
        begin_play();
        return true;
    }
    if (c == 'E' && s_mode != VG_RP_OFF) {
        const bool was_play = (s_mode == VG_RP_PLAY);
        s_mode = VG_RP_OFF;
        if (was_play) vg_capture_set(VG_CAP_OFF);
        uint32_t wb, ws, wt, wm;
        vg_link_stats(&wb, &ws, &wt, &wm);
        uint32_t fb, fe;
        vg_capture_frame_counts(&fb, &fe);
        Serial.printf("\nvg_replay: END %u frames  wrote %u bytes  short %u  "
                      "stall %u  mismatch %u  begins %u ends %u\n",
                      (unsigned)s_index, (unsigned)wb, (unsigned)ws,
                      (unsigned)wt, (unsigned)wm, (unsigned)fb, (unsigned)fe);
        return true;
    }
    return false;
}
