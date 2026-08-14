#pragma once

// ===========================================================================
// THE INSTRUMENT SURGE -- the second episodic match event
//
// Mechanism in vg_event.h, tuning in cfg_events.h. This file is what the amplitude MEANS,
// which is the one thing the scheduler never learns.
//
// WHY IT PUBLISHES A NUMBER INSTEAD OF DOING SOMETHING, which is the interesting part and
// the reason a second event was worth building.
//
// vg_anomaly's `apply` writes the world directly -- vg_arena_warp_set, vg_shake_rumble --
// and that works because it owns what it writes. The arena warp is a value nobody else
// sets, and the rumble is a request on a channel where the loudest wins.
//
// This event owns nothing. Every lever it wants -- vg_canopy_warp, vg_hud_warp,
// vg_hud_jitter -- is a value vg_render.cpp computes fresh every frame from the throttle
// and the damage strain. An `apply` that called them would be overwritten inside the same
// frame, and an apply that ran after the renderer would erase the speed bow instead. There
// is no ordering that makes writing them correct.
//
// So it publishes, and vg_render folds the number in where it already computes those
// three. That is the systems rule stated the other way round: a module that must hand
// state to another module needs a header to hand it from.
//
// THIS IS A LIMIT ON vg_event AND IT IS WORTH KNOWING. The interface assumes `apply` can
// act on the world. Half the time it cannot, and the event has to become state that
// somebody else reads. vg_event is still the right shape -- the schedule, the dice, the
// easing and the freeze rule were all reused untouched -- but "apply does the work" is
// true of the anomaly and not of events in general.
// ===========================================================================

// HOW BADLY THE PANEL IS LYING, 0 clear and 1 at the worst of an episode. Already eased
// and already scaled by the round and by this episode's peak, so a reader multiplies by
// its own constant from cfg_events.h and adds -- no arithmetic of its own.
//
// READ EVERY FRAME BY vg_render, in three places. Written only by the event's apply, and
// by the clear below.
struct Surge {
    float level;
};

extern Surge vg_surge;

// Roll this match's surges before it starts. `round_t` is 0 at the first round and 1 at
// the final, the same ramp vg_anomaly_begin_match takes and worked out by the same caller.
void vg_surge_begin_match(float round_t);

// Advance one frame. Cheap and safe every frame of every state. `frozen` holds the clock
// without ending the episode -- see vg_event_step; the broadcast is busy after a kill.
void vg_surge_step(float dt, bool frozen);

// Back to a panel that can be trusted.
//
// NEEDED, unlike the anomaly's rumble, and for the reason the clear rule has produced
// every other time: level is a VALUE that persists until something sets it otherwise, not
// a per-frame request that lapses. It also relied on the memset of `vg` for exactly as
// long as it took to notice it is not in `vg` at all.
void vg_surge_clear(void);
