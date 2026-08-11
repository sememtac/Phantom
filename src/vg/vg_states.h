#pragma once
#include <stdint.h>

// ===========================================================================
// THE STATE MACHINE'S OWN THINGS
//
// vg_states.cpp was the only translation unit in the game with no header. Its
// types and its state lived in vg_game.h, which is the struct every file
// includes -- so the transition's three working fields were public to code that
// has no business knowing a transition is running.
//
// This is deliberately NOT the whole of vg_states.cpp's surface. vg_state_go and
// vg_state_cut stay declared in vg_game.h for now: they are reached from most of
// the game and moving them is an include churn worth doing on its own, not
// underneath a state move.
// ===========================================================================

enum TvPhase : uint8_t {
    TV_NONE = 0,
    TV_OUT,        // old scene fading and collapsing to the band
    TV_HOLD,       // dead air: black, nothing at all
    TV_IN          // new scene opening back up out of it
};

#define TV_OUT_TIME 0.40f
// Dead air. Without it the set goes out and comes straight back, and the turning
// ON -- which is the half worth watching -- has nothing to arrive out of. A
// second of nothing is what makes the next thing an arrival rather than a wipe.
#define TV_HOLD_TIME 1.00f
#define TV_IN_TIME  0.55f

// WHERE A TRANSITION HAS GOT TO. Three fields, and the renderer and the audio
// both read them -- the picture collapses to a band and the engines go quiet on
// the same clock, so they have to be reading one.
//
// `to` is a VgState held as a byte, because this header does not need to know the
// state enum to carry a number across the join.
struct TvState {
    uint8_t phase;   // TvPhase
    uint8_t to;      // VgState, entered at the join
    float   t;
};

extern TvState vg_tv;

// Back to no transition. Called from vg_game_init, because the memset of `vg`
// that used to cover these three fields does not reach them any more -- and
// begin_record restarts the game through vg_game_init before every recording.
void vg_tv_clear(void);
