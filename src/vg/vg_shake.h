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
// --- the knock bus ---------------------------------------------------------
// See vg_shake.h. 1.0 on the hit channel is a missile striking the player, and
// every other source is scaled against it, so these numbers are the whole feel
// of being knocked about in one place.
//
// The first two are deliberately unchanged from the single-source version this
// replaced: a missile hit has to land exactly as hard as it always did, or the
// refactor is a retune wearing a refactor's clothes.
#define SHAKE_HIT_PX         13.0f    // view px at 1.0
#define SHAKE_HIT_DECAY      2.6f     // per second
// Two at once is worse than one; six is not six times worse. Without a ceiling a
// busy moment throws the view clean off the panel.
#define SHAKE_HIT_MAX        1.8f
// A rattle is not a blow, so half the amplitude of one.
#define SHAKE_RUMBLE_PX      6.5f
#define SHAKE_RUMBLE_RATE    9.0f     // how fast it follows the requested level
// The panel moves less than the view -- see HUD_SHAKE_MAX for why it moves at all.
#define SHAKE_HUD_RATIO      0.22f

// THIS FRAME'S VIEW OFFSET, in pixels, which the camera reads. It was on VgGame and
// the level behind it was already here -- so the published output lived apart from
// the thing that computes it, for no reason except that this module had a header and
// nobody moved the two fields into it.
//
// REPUBLISHED ONLY BY vg_shake_update, and that runs from vg_world_step -- so out of
// flight nothing rewrites these while vg_render_frame still reads them every frame in
// every state. That is why vg_shake_clear exists and why the state changes call it.
struct Shake {
    float x, y;
};

extern Shake vg_shake;

void vg_shake_hit(float amount);

// A sustained condition, for THIS frame. Must be called every frame it holds.
void vg_shake_rumble(float amount);

// Decay both channels and publish this frame's view offset into vg_shake,
// which the camera already reads.
void vg_shake_update(float dt);

// Everything to zero -- both channels and the published offset. For a state change,
// where a knock must not survive, and for vg_game_init, which is what begin_record
// restarts the game through before a recording.
void vg_shake_clear(void);


// This frame's PANEL offset, in pixels, on its own clock and smaller than the
// view's. A HUD shaking in lockstep with the world just reads as one bigger
// shake; the two disagreeing is what says the panel is bolted to a machine
// rather than painted on the lens. See HUD_SHAKE_MAX.
void vg_shake_hud(float* x, float* y);
