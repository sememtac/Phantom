#include "vg_shake.h"
#include "vg_game.h"
#include "vg_sim.h"
#include "vg_glitch.h"
#include <math.h>

static float s_hit  = 0.0f;   // impact channel
static float s_want = 0.0f;   // rumble REQUESTED this frame, consumed by update
static float s_rum  = 0.0f;   // rumble as actually felt, eased toward s_want

void vg_shake_hit(float amount) {
    if (amount <= 0.0f) return;
    s_hit += amount;
    // Capped. Two things at once should be worse than one; six should not be six
    // times worse, or a busy moment throws the view off the panel entirely.
    if (s_hit > SHAKE_HIT_MAX) s_hit = SHAKE_HIT_MAX;
}

void vg_shake_rumble(float amount) {
    if (amount > s_want) s_want = amount;
}

void vg_shake_clear(void) {
    s_hit = s_want = s_rum = 0.0f;
    vg.shake_x = vg.shake_y = 0.0f;
}

float vg_shake_level(void)        { return s_hit; }
float vg_shake_rumble_level(void) { return s_rum; }

// The amplitude both outputs are built from, so the view and the panel can never
// disagree about how hard the airframe is being hit -- only about which way it is
// moving at this instant.
static inline float shake_amp(void) {
    return s_hit * SHAKE_HIT_PX + s_rum * SHAKE_RUMBLE_PX;
}

void vg_shake_update(float dt) {
    if (s_hit > 0.0f) {
        s_hit -= dt * SHAKE_HIT_DECAY;
        if (s_hit < 0.0f) s_hit = 0.0f;
    }

    // Eased both ways. A condition that snaps on and off reads as a rendering
    // fault; a body that starts and stops resonating does neither.
    const float k = 1.0f - expf(-dt * SHAKE_RUMBLE_RATE);
    s_rum += (s_want - s_rum) * k;
    if (s_rum < 0.0005f) s_rum = 0.0f;   // no permanent sub-pixel tremble
    // CONSUMED. A rumble source has to say so again next frame, which means a
    // source that stops being true stops being felt without having to remember
    // to cancel itself.
    s_want = 0.0f;

    const float amp = shake_amp();
    if (amp > 0.0f) {
        // Re-rolled every frame rather than swept, which is what makes it read as
        // being struck instead of as a wobble. The buzz and the VG_OVER tumble add
        // themselves on top of this in vg_world_step.
        vg.shake_x = vg_frand(-amp, amp);
        vg.shake_y = vg_frand(-amp, amp);
    } else {
        vg.shake_x = vg.shake_y = 0.0f;
    }
}

void vg_shake_hud(float* x, float* y) {
    *x = *y = 0.0f;
    const float amp = shake_amp() * SHAKE_HUD_RATIO;
    if (amp <= 0.0f) return;
    // 23Hz: its own bucket, clear of the speed strain at 47 and the damage
    // flicker at 31, so three things moving the panel never resolve into one
    // motion. Hashed off state_t rather than drawn from the RNG because this runs
    // in the DRAW, which must not touch the simulation's random stream.
    vg_glitch_offset(vg.state_t, 23.0f, amp, x, y);
}
