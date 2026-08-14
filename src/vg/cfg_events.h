#pragma once

// ===========================================================================
// EPISODIC MATCH EVENTS -- the tuning, one block per event
//
// The mechanism is vg_event.h: one clock, five phases, one amplitude, and two function
// pointers. This file is the DATA half -- the thirteen numbers that make one event
// different from another, and nothing else.
//
// It exists because a second event proved cfg_world.h was the wrong home. The anomaly
// warps the arena, so its numbers sat with the arena; the surge attacks the cockpit and
// would have had to sit there too, next to asteroid density, for no reason but that the
// first event happened to be about the world. One event fitted. Two did not.
//
// EVERY BLOCK HERE HAS THE SAME THIRTEEN NUMBERS IN THE SAME ORDER, matching
// MatchEventSpec field for field, so a new event is a copy of a block and a re-tune
// rather than a design problem. Keep it that way.
//
// THE ROLLS COME FROM vg_frand -- the recorded stream -- so a replay reproduces the same
// weather. That also means ADDING AN EVENT CHANGES EVERY EXISTING RECORDING: the extra
// draws at match start shift the stream for everything after. See vg_event.h, and expect
// to retake the regression baseline whenever this file gains a block.
// ===========================================================================

// ---------------------------------------------------------------------------
// THE ARENA ANOMALY -- the venue itself distorts, mid-fight
//
// The first event, and the one the mechanism was extracted from. Nothing explains it. The
// broadcast announces it and announces when it passes, and the broadcast does not know
// what it is either. See IFT_ANOMALY.
// ---------------------------------------------------------------------------

// The chance a match has any disturbance at all, at round 0 and at the final. A quiet match is
// part of the mechanic: if it happened every time it would be scenery rather than an event.
// PROTOTYPE VALUES, and the low end is deliberately generous so it can be seen while it is
// being judged -- a real curve probably starts nearer 0.25.
#define ANOM_CHANCE_ROUND0   0.70f
#define ANOM_CHANCE_FINAL    0.90f
// A match that has one can have a second. Rolled once at the start, so the pacing of a match
// is decided before it begins and not drifted into.
#define ANOM_CHANCE_SECOND   0.35f
// Seconds of quiet before an episode. The first is short enough to be met while the fight is
// still fresh; a later one waits longer.
#define ANOM_WAIT_MIN        14.0f
#define ANOM_WAIT_MAX        40.0f
// How long it stays once it has arrived.
#define ANOM_HOLD_MIN        16.0f
#define ANOM_HOLD_MAX        34.0f
// The arrival and the departure are NOT symmetrical, and that asymmetry is the whole read of
// it. Coming on fast enough to notice mid-turn is what makes it an event; going away slowly is
// what makes it feel like something passing rather than something switched off.
#define ANOM_ONSET_S         1.8f
#define ANOM_FADE_S          6.0f
// An episode does not always reach the round's full strength. Scaled by this, so two matches in
// the same round are not the same weather. The floor is high: the variation is there to keep two
// episodes from being identical, not to produce a disturbance nobody notices.
#define ANOM_PEAK_MIN        0.75f
#define ANOM_PEAK_MAX        1.00f

// THE AIRFRAME FEELS IT TOO, and this is the half that makes it cost the player something. The
// moving wall takes away room; the rumble takes away the shot. Guns are aimed down the nose, so
// a view that will not sit still is a real handicap for as long as it lasts -- which is the
// point, and also why this number is nothing like the others on the bus.
//
// Far below every other rumble source (PASS_RUMBLE 1.50, FIRE_RUMBLE 1.15) because those are
// MOMENTS -- a fighter crossing the nose, flying through a wreck -- and this is a condition that
// holds for half a minute. At 0.35 it is about 2 px of continuous view movement: enough to spoil
// a long shot and to be felt the whole time, not enough to be punishing to look at.
//
// It rides the same eased ramp as the geometry, so the shaking arrives with the ground moving
// and leaves with it.
#define ANOM_RUMBLE          0.35f

// ---------------------------------------------------------------------------
// THE INSTRUMENT SURGE -- the panel stops being trustworthy, mid-fight
//
// The second event, and it exists to TEST the mechanism as much as to be played: if a
// second event were not a row plus an apply and an announce, vg_event would be the wrong
// shape, and it is far cheaper to learn that at two instances than at four.
//
// IT DELIBERATELY ATTACKS SOMETHING THE ANOMALY DOES NOT. The anomaly moves the world --
// the arena warp and the rumble. This one leaves the world alone and goes after the
// cockpit: the frame flexes, the instruments bow and jitter, and none of it is a lie the
// player can check against anything. A second event that reached for the arena warp and
// the rumble channel would have tested nothing except that the scheduler can run twice.
//
// It is also the event that found the mechanism's one real limit. The anomaly's `apply`
// WRITES the world, because it owns what it writes. The surge cannot: every instrument
// lever -- vg_canopy_warp, vg_hud_warp, vg_hud_jitter -- is a value vg_render.cpp
// recomputes every frame from speed and strain, so an apply that called them would be
// overwritten within the same frame. So the surge PUBLISHES an amplitude instead and the
// renderer folds it in where it already computes those three. See vg_surge.h.
// ---------------------------------------------------------------------------

// Rarer than the anomaly and rarer at the start. Losing the instruments is a harsher thing
// to be handed than a moving wall, and a fight that opens with it reads as broken rather
// than as eventful.
#define SURGE_CHANCE_ROUND0  0.30f
#define SURGE_CHANCE_FINAL   0.60f
// One is the story; two is a fault report. The anomaly can come back because the venue
// passing through something twice is weather -- instruments failing twice is a different
// claim about the ship.
#define SURGE_CHANCE_SECOND  0.15f
// The amplitude a round-0 match may reach, against the final's 1.0. Lower than the
// anomaly's floor because this one costs the player information rather than room.
#define SURGE_ROUND0         0.55f
// Later than the anomaly's window on purpose. If both fire in the same match they should
// not arrive together -- two events at once is not two events, it is noise.
#define SURGE_WAIT_MIN       26.0f
#define SURGE_WAIT_MAX       62.0f
// SHORTER THAN THE ANOMALY'S HOLD, and this is the number most likely to want retuning.
// Flying with a wall closing in is playable for half a minute; flying with instruments you
// cannot read is not, and the moment it stops being tense it is just annoying.
#define SURGE_HOLD_MIN       7.0f
#define SURGE_HOLD_MAX       14.0f
// FASTER IN AND OUT THAN THE ANOMALY, both ends. Electrical failure has no mass: it
// arrives as a snap rather than as something moving in, and it either clears or it does
// not. The anomaly's slow fade reads as something passing by; that would be wrong here.
#define SURGE_ONSET_S        0.35f
#define SURGE_FADE_S         2.2f
// A wider spread than the anomaly's, so a mild surge is a flicker worth noticing and a bad
// one is genuinely hard to fly through.
#define SURGE_PEAK_MIN       0.55f
#define SURGE_PEAK_MAX       1.00f

// WHAT THE AMPLITUDE BUYS AT EACH OF THE THREE PLACES vg_render folds it in.
//
// All three are ADDED to what the renderer already computed, never substituted, so the
// speed-driven bow and the damage strain still read normally underneath.

// Extra HUD bow, on top of the speed curve. HUD_WARP_SPEED_MIN..1.0 is the normal range,
// so 0.6 roughly doubles the bend at rest without ever reaching the quantiser's ceiling.
#define SURGE_HUD_WARP       0.60f
// Panel shake, in pixels. Deliberately close to HUD_SHAKE_MAX: the instruments should move
// about as much as a hit makes them move, but continuously, which is the unsettling part.
#define SURGE_HUD_JITTER     2.4f
// How hard the canopy frame itself flexes. Small -- the frame is structure, and a cockpit
// visibly bending is a different and much louder claim than instruments misreading.
#define SURGE_FRAME_FLEX     0.18f
// The jitter's own frequency, fed to vg_glitch_offset. Well away from the damage glitch's
// 47.0 so the two do not beat against each other into a slow throb when both are up.
#define SURGE_JITTER_HZ      31.0f
