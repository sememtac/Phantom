#pragma once
#include "vg_game.h"   // the cutscene flies a Ship, and Ship is defined there

// ===========================================================================
// LAUNCH CUTSCENE
//
// The camera work that opens a match: a fixed viewpoint inside the torus, an
// entry gate for each fighter, and a rally-style fly-by as they come through.
//
// It lives apart from the state machine because it changes for entirely
// different reasons. The flow decides WHEN a match begins; this decides what
// that looks like, and every edit to it so far has been about framing, pacing
// or lens work rather than about game state.
// ===========================================================================

// Run one frame. Returns true when the cutscene has finished and the caller
// should hand over to the cockpit -- the handover itself belongs to the flow,
// not here, because it is about starting a match rather than about a shot.
bool vg_cine_update(float dt, bool skip);

// Drop the cutscene ship, its ribbon and any open gate. Also used by the death
// sequence, which borrows the same ship to stand in as the player's wreck.
void vg_cine_clear(void);

// THE SHOT'S OWN STATE. It was in VgGame, where a module's state goes when the
// module has nowhere to declare it -- and this module has had a header all along,
// so there was never a reason beyond nobody moving it.
//
// It carries a Ship, which is why this header includes vg_game.h. That is not a
// cycle: vg_game.h includes no module headers, and vg_cine.h is included only from
// translation units. Worth stating, because the obvious worry on reading the include
// above is exactly that.
struct CineState {
    Ship  ship;
    bool  on;

    // Entry gate: a lit plane in the pilot's own colour that the cutscene ship
    // emerges through. Held as a centre and two in-plane axes rather than as a
    // normal, so it rides the world rotation the same way everything else does
    // and never has to be rebuilt from a basis.
    Vec3  gate_pos, gate_r, gate_u;
    float gate_t;    // counts down; the plane is drawn while positive
    float gate_hue;
    float hold;      // >0 while the ship is still waiting behind the gate
};

extern CineState vg_cine;

// EVERYTHING to zero, which vg_cine_clear deliberately does not do -- that one
// keeps the ship where it is because the death sequence borrows it as the player's
// wreck, and it is called mid-shot.
//
// This is the one vg_game_init needs. The struct relied on that function's memset
// of `vg` and no longer sits inside it, and begin_record restarts the game through
// vg_game_init before every recording -- so without this a session could open with
// a cutscene still switched on from whatever ran before it.
void vg_cine_reset(void);
