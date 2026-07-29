#pragma once

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
