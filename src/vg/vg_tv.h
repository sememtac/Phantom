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

// Start a transition toward state `to`. The DECISION to cut is vg_states.cpp's -- it
// silences the audio and the broadcast first, which is policy about what a cut means.
void vg_tv_begin(uint8_t to);

// Advance the schedule. Returns true on the single frame the join is due, which is the
// end of the dead air rather than the start of it: joining at the start would let the new
// scene run for a whole second behind a black screen, and the first thing a scene does is
// start talking.
// `join` is called at the moment the dead air ends, BEFORE the phase advances and before
// the set-striking sound -- which is exactly where it happened when this clock lived in
// vg_states.cpp. A state's enter hook runs inside it, so the ordering is observable.
bool vg_tv_step(float dt, void (*join)(uint8_t to));

// Drive the four controls from wherever the schedule has got to. Called once a frame by
// the renderer, which used to hold this curve itself.
void vg_tv_apply(void);

// Back to no transition, for vg_game_init -- the three fields relied on the memset of vg
// until they left it, and begin_record restarts through that function.
void vg_tv_clear(void);

// THE SET TURNING ON AND OFF -- the broadcast transition, lifted out of vg_band.cpp.
//
// It was 208 lines in the middle of the raster half, and it belonged to none of it: four
// statics nothing else reads, one call site, and no shared state with the primitives, the
// canopy or the scanlines it sat between. What made it safe to move is that its call site
// is behind vg_tv_active() -- so the frame reaches this module only during a transition,
// and a cross-unit call on a path that runs for half a second costs nothing.
//
// RENAMED ON THE WAY OUT, from vg_rast_tv/vg_rast_tv_active. The prefix and the file are
// not allowed to disagree twice: see the note on vg_hud_ in vg_raster.h, which is the same
// fault left in place because moving those functions was worse than living with it. Here
// there was nothing to weigh -- the code was moving anyway.

// The transition's three controls, all 0..1 and clamped. Called from vg_render.cpp, which
// drives them off the state clock.
//
//   open  how much of the height is picture at all, centred -- the aperture
//   wide  ...and of the width: the dot before the line
//   wash  how far the lit part is washed toward white
//   dim   how far what remains is faded toward black
void vg_tv_set(float open, float wide, float wash, float dim);

// Whether any of the four is away from its resting value. The band pass is skipped
// entirely when this is false, which is what keeps the effect off the frame's bill.
bool vg_tv_active(void);

// One band, in place. Call only when vg_tv_active() -- it does not check.
void vg_tv_band(uint16_t* band, int by0);
