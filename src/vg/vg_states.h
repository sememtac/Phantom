#pragma once
#include <stdint.h>
// Tap and VgInput, which the state handlers below take. vg_states.h needed neither
// while it held only TvState; the handlers arrived from vg_sim.h, which had them via
// vg_game.h all along, and the compiler said so on the first build after the move.
#include "vg_game.h"

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

// --- MOVED FROM vg_sim.h -------------------------------------------------
//
// The state machine's own interface. Fourteen of these were filed under a
// vg_cockpit.cpp banner in vg_sim.h, which was accurate when it was written and
// stopped being so when 41057fd moved the handlers into vg_states.cpp.

// The menus' own world motion: enough drift to stop the backdrop looking like a
// still, and nothing that can be flown into.
void vg_menu_world(float dt);

// Put the menu backdrop up. Free unless a venue has displaced it, so moving
// between the title, the bracket and the repair screen costs nothing.
void vg_use_menu_sky(void);

// One frame of the set turning on or off between two states.
void vg_tv_update(float dt);

// Start the flight phase of a match: arena, racks, opponent, panel boot.
//
// Called BY NAME rather than hung on VG_PLAYING's entry hook, and it must stay
// that way. VG_HIT returns control to VG_PLAYING through vg_state_go, so an entry
// hook here would rebuild the arena, empty the racks and respawn the opponent
// after every hit the player takes. See the note on VgStateDef::leave.
void vg_begin_flight(void);

// One frame of each state, one function per STATES row. VG_PLAYING and VG_HIT
// share vg_upd_playing: taking a hit does not change what flying is, only what
// the panel is doing about it.
void vg_upd_attract(float dt, const VgInput* in, const Tap* tap);
void vg_upd_entry(float dt, const VgInput* in, const Tap* tap);
void vg_upd_select(float dt, const VgInput* in, const Tap* tap);
void vg_upd_repair(float dt, const VgInput* in, const Tap* tap);
void vg_upd_bracket(float dt, const VgInput* in, const Tap* tap);
void vg_upd_intro(float dt, const VgInput* in, const Tap* tap);
void vg_upd_playing(float dt, const VgInput* in, const Tap* tap);
void vg_upd_kill(float dt, const VgInput* in, const Tap* tap);
void vg_upd_pause(float dt, const VgInput* in, const Tap* tap);
void vg_upd_course(float dt, const VgInput* in, const Tap* tap);
void vg_upd_round_won(float dt, const VgInput* in, const Tap* tap);
void vg_upd_over(float dt, const VgInput* in, const Tap* tap);
void vg_upd_won(float dt, const VgInput* in, const Tap* tap);

// The lookup: run whatever the current state's row says to run.
void vg_state_update(float dt, const VgInput* in, const Tap* tap);
