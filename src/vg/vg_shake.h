#pragma once

// ===========================================================================
// THE KNOCK BUS
//
// Everything that physically jolts the airframe goes through here, so the view
// and the panel move for one agreed reason and every source is tuned against
// one scale.
//
// Before this there was a single float, vg.shake, written directly from two
// places that both meant "a missile hit the player". Anything else that should
// have been felt -- a warhead going off beside the canopy, a fighter crossing
// the nose, flying through the fire of something that just died -- did nothing
// at all, because there was nowhere to say it.
//
// TWO CHANNELS, because a blow and a rattle are not the same claim:
//
//   HIT     An event. Discrete, and it ACCUMULATES inside a frame: two things
//           going off together should be felt harder than one. Then it decays
//           fast, because an impact is over.
//
//   RUMBLE  A condition. Renewed every frame it still applies, and the loudest
//           reason WINS rather than summing -- two reasons to be shaking is
//           still one shaking. It eases in and out instead of switching, since
//           the thing being modelled is a body resonating, not a flag.
//
// Sources apply their own falloff before calling. 1.0 on the hit channel is a
// missile striking the player, and every other source in the game was judged
// against that one.
//
// The speed buzz stays OUTSIDE this: vg.buzz is the airframe vibrating under
// its own power rather than something striking it, and it is already tuned.
// Amplitudes and rates are in cfg_flight.h.
// ===========================================================================

// An impact. `amount` is relative to a missile hit on the player.
void vg_shake_hit(float amount);

// A sustained condition, for THIS frame. Must be called every frame it holds.
void vg_shake_rumble(float amount);

// Decay both channels and publish this frame's view offset into
// vg.shake_x / vg.shake_y, which the camera already reads.
void vg_shake_update(float dt);

// Everything to zero. For a state change, where a knock must not survive.
void vg_shake_clear(void);

float vg_shake_level(void);          // hit channel
float vg_shake_rumble_level(void);   // rumble channel, smoothed

// This frame's PANEL offset, in pixels, on its own clock and smaller than the
// view's. A HUD shaking in lockstep with the world just reads as one bigger
// shake; the two disagreeing is what says the panel is bolted to a machine
// rather than painted on the lens. See HUD_SHAKE_MAX.
void vg_shake_hud(float* x, float* y);
