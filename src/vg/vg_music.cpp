#include "vg_music.h"
#include "vg_game.h"
#include "vg_tourney.h"
#include "generated/score_tournament.h"
#include "generated/score_round16.h"
#include "generated/score_quarterfinal.h"
#include "generated/score_semifinal.h"
#include "generated/score_phantom.h"

#define TUNE(sym) SCORE_##sym##_STEPS,                                        \
                  (int)(sizeof(SCORE_##sym##_STEPS) /                         \
                        sizeof(SCORE_##sym##_STEPS[0])),                      \
                  SCORE_##sym##_NOTES, SCORE_##sym##_LEN_MS

// In enum order, like the STATES table and the cue table. Positional, so the
// enum and this read the same way side by side.
static const VgTuneDef TUNES[TUNE_COUNT] = {
    { "tournament",   TUNE(TOURNAMENT)   },
    { "round16",      TUNE(ROUND16)      },
    { "quarterfinal", TUNE(QUARTERFINAL) },
    { "semifinal",    TUNE(SEMIFINAL)    },
    { "phantom",      TUNE(PHANTOM)      },
};

static VgTune s_now  = TUNE_COUNT;
static bool   s_held = false;   // held by the pause screen

const char* vg_music_name(VgTune t) {
    return ((int)t < TUNE_COUNT) ? TUNES[t].name : "none";
}

VgTune vg_music_now(void) {
    return s_now;
}

VgTune vg_music_for_round(unsigned char round) {
    switch (round) {
    case 0:  return TUNE_ROUND16;
    case 1:  return TUNE_QUARTER;
    case 2:  return TUNE_SEMI;
    case 3:  return TUNE_PHANTOM;
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
    // THE WAY IN IS SILENT. The title card, the callsign screen and the hangar
    // carry no tune. They used to share the theme that is now the FINAL's, which
    // gave the piece away in the first ten seconds of a new game and left the
    // last fight with nothing the player had not already heard.
    case VG_ATTRACT:
    case VG_ENTRY:
    case VG_SELECT:
        return TUNE_COUNT;

    // The table, and the repair bay reached from it. One screen in two parts.
    case VG_REPAIR:
    case VG_BRACKET:
        return TUNE_TOURNEY;

    // A match, and the rounds either side of a hit. HIT and KILL are still the
    // fight, so the music must not stop and start across them.
    case VG_PLAYING:
    case VG_HIT:
    case VG_KILL:
        return vg_music_for_round(vt.round);

    // THE SCORECARD HOLDS WHATEVER WAS PLAYING. This is the beat between the
    // last shot and the table, and cutting the music at the kill would end the
    // fight twice. It matters most after the FINAL, where this screen is the
    // first half of the ending: vg_tourney_resolve advances the round counter
    // while this handler runs, so asking vg_music_for_round here would change
    // the answer underneath the player. Holding cannot.
    case VG_ROUND_WON:
        return s_now;

    // THE WIN SCREEN IS THE REST OF THAT ENDING. The final's theme runs on,
    // unbroken, through the champion card and the rumour and the name -- the
    // player finds out who they are over the music of the pilot they beat.
    // It stops when they tap, because a tap goes to ATTRACT and ATTRACT is
    // silent.
    case VG_WON:
        return TUNE_PHANTOM;

    // NOT the launch cutscene, not the course, and not a loss. INTRO has its own
    // drama, the course has no stakes, and VG_OVER should land in silence.
    default:
        return TUNE_COUNT;
    }
}

void vg_music_update(void) {
    // THE PAUSE SCREEN HOLDS THE MUSIC. A pause suspends a screen rather than
    // being one, so the tune is not changed and not restarted -- it is held, and
    // it goes on from the same bar when the screen comes down.
    //
    // The cost is that the music slider on that screen has nothing to move while
    // it is being dragged. That is the trade the player asked for.
    if (vg.state == VG_PAUSE) {
        if (!s_held) {
            s_held = true;
            vg_score_pause(true);
        }
        return;
    }
    if (s_held) {
        s_held = false;
        vg_score_pause(false);
    }

    const VgTune w = want();
    if (w == s_now && (w == TUNE_COUNT || vg_score_playing())) return;
    vg_music_play(w, true);
}
