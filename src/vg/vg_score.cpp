#include "vg_score.h"
#include "vg_sfx.h"     // vg_vol

static const VgScoreStep* s_steps = nullptr;
static const SynthLayer*  s_notes = nullptr;
static int   s_n      = 0;
static int   s_next   = 0;
static int   s_len_ms = 0;
static float s_t      = 0.0f;   // milliseconds from the start of the score
static bool  s_loop   = false;
static bool  s_on     = false;

void vg_score_play(const VgScoreStep* steps, int n_steps,
                   const SynthLayer* notes, int len_ms, bool loop) {
    if (!steps || !notes || n_steps <= 0 || len_ms <= 0) return;
    s_steps = steps;
    s_notes = notes;
    s_n     = n_steps;
    s_len_ms = len_ms;
    s_next  = 0;
    s_t     = 0.0f;
    s_loop  = loop;
    s_on    = true;
}

void vg_score_stop(void) {
    s_on = false;
}

void vg_score_pause(bool held) {
    // The position is untouched either way, so letting go carries on from the
    // bar it was holding.
    if (s_steps) s_on = !held;
}

bool vg_score_playing(void) {
    return s_on;
}

void vg_score_update(float dt) {
    if (!s_on || !s_steps) return;

    s_t += dt * 1000.0f;

    while (s_next < s_n && (float)s_steps[s_next].t_ms <= s_t) {
        // THE POOL IS SHARED WITH THE CUES. Near the top of it the music stops
        // adding notes, so an alert always has somewhere to sound. See vg_score.h.
        if (vg_synth_live() < VG_SCORE_VOICES) {
            // A COPY, so the player's music setting can scale it. The table in
            // flash is const and the same row is used again every loop.
            //
            // The sfx setting scales the whole bus afterwards in vg_sfx.cpp, so
            // it moves the music as well. That is the mixer this game has: one
            // bus, one pool. `music` is the balance between the two, not an
            // independent output.
            SynthLayer l = s_notes[s_steps[s_next].note];
            // SQUARED, like the sfx setting in vg_sfx.cpp. A slider that moves a
            // level has to be squared to feel even to the ear, and both settings
            // must do the same thing or the balance between them moves as they
            // are dragged. It also matters off the board: tools/score_audio.py
            // previews at music squared, and the preview has to be the truth.
            //
            // It was not squared, and the theme clipped the mixer sixty times a
            // second on the device while the preview measured a peak of 0.58.
            l.gain *= vg_vol.music * vg_vol.music;
            vg_synth_layer(&l, 1.0f);
        }
        s_next++;
    }

    if (s_t >= (float)s_len_ms) {
        if (s_loop) {
            // Keep the remainder rather than zeroing, or the loop would gain a
            // fraction of a frame every time round and drift out of tempo.
            s_t -= (float)s_len_ms;
            s_next = 0;
        } else {
            s_on = false;
        }
    }
}
