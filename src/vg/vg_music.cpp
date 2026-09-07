#include "vg_music.h"
#include "vg_game.h"
#include "vg_tourney.h"
#include "generated/score_motif.h"
#include "generated/score_semifinal.h"

#define TUNE(sym) SCORE_##sym##_STEPS,                                        \
                  (int)(sizeof(SCORE_##sym##_STEPS) /                         \
                        sizeof(SCORE_##sym##_STEPS[0])),                      \
                  SCORE_##sym##_NOTES, SCORE_##sym##_LEN_MS

// In enum order, like the STATES table and the cue table. Positional, so the
// enum and this read the same way side by side.
static const VgTuneDef TUNES[TUNE_COUNT] = {
    { "title",     TUNE(MOTIF)     },
    { "semifinal", TUNE(SEMIFINAL) },
};

static VgTune s_now = TUNE_COUNT;

const char* vg_music_name(VgTune t) {
    return ((int)t < TUNE_COUNT) ? TUNES[t].name : "none";
}

VgTune vg_music_now(void) {
    return s_now;
}

VgTune vg_music_for_round(unsigned char round) {
    // ONE ROUND HAS A THEME SO FAR. The rest are silent rather than borrowing
    // the title's: a fight under the attract music would say the wrong thing,
    // and silence is honest about what is written.
    switch (round) {
    case 2:  return TUNE_SEMI;
    default: return TUNE_COUNT;
    }
}

void vg_music_play(VgTune t, bool loop) {
    if ((int)t >= TUNE_COUNT) {
        s_now = TUNE_COUNT;
        vg_score_stop();
        return;
    }
    if (t == s_now && vg_score_playing()) return;   // already on; do not restart
    const VgTuneDef* d = &TUNES[t];
    s_now = t;
    vg_score_play(d->steps, d->n_steps, d->notes, d->len_ms, loop);
}

// What belongs to the screen the game is on.
static VgTune want(void) {
    switch (vg.state) {
    // EVERY MENU BEFORE A MATCH, not the title alone. The theme used to stop the
    // moment the screen was touched, because ATTRACT is only the first of five
    // screens the player crosses on the way to a fight and the other four had no
    // tune. A menu is a menu.
    case VG_ATTRACT:
    case VG_ENTRY:
    case VG_SELECT:
    case VG_REPAIR:
    case VG_BRACKET:
        return TUNE_TITLE;

    // NOT the launch cutscene, and not the screens after a match. INTRO has its
    // own drama and the results screens have their own mood; both would want
    // their own tune rather than the title's. Silent until they have one.

    // A match, and the rounds either side of a hit. HIT and KILL are still the
    // fight, so the music must not stop and start across them.
    case VG_PLAYING:
    case VG_HIT:
    case VG_KILL:
        return vg_music_for_round(vt.round);

    // A pause suspends a screen rather than being one, and the volume sliders
    // live on it: stopping the music there would take away the thing the music
    // slider is for. Keep whatever was playing.
    case VG_PAUSE:
        return s_now;

    default:
        return TUNE_COUNT;
    }
}

void vg_music_update(void) {
    const VgTune w = want();
    if (w == s_now && (w == TUNE_COUNT || vg_score_playing())) return;
    vg_music_play(w, true);
}
