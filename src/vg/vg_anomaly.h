#pragma once

// ===========================================================================
// THE ANOMALY: the arena's weather. See ANOM_CHANCE_ROUND0 in cfg_world.h.
//
// The arena is a different place in space every round, and something can be wrong
// with the place. Nothing explains it. The broadcast announces it and announces
// when it passes, and the broadcast does not know what it is either.
//
// The pacing -- rolled up front, arriving after a wait, on fast and off slow, maybe
// twice -- is not this file's. That is vg_event, and this is one row of it plus the
// two handlers that say what the amplitude means: the tunnel moves, and the airframe
// feels it.
//
// IT IS NOT IN vg_arena.cpp, beside the field it drives, and that is deliberate. The
// field is geometry and has no idea what a round is, while this is a match event that
// rolls dice, watches a clock and puts a line on the broadcast. Arena code that knew
// about the IFT would be the wrong shape.
// ===========================================================================

// Roll this match's weather, and set the tunnel round. Called from vg_match_start.
//
// `round_t` is 0 at the first round and 1 at the final -- worked out by the caller,
// which is the one place that already holds the bracket. See vg_event.h.
void vg_anomaly_begin_match(float round_t);

// One frame. `frozen` holds the episode where it is without ending it; the caller
// passes it while the broadcast is busy with the end of the round.
void vg_anomaly_step(float dt, bool frozen);

// Put the tunnel back to perfectly round, whatever was happening. For the menus and
// the attract loop, which are not a match and do not step anything.
void vg_anomaly_clear(void);
