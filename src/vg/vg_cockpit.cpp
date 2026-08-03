#include "vg_sim.h"
#include "vg_sfx.h"

// What the cockpit does about things that have already happened: the caution
// annunciators, and the panel's own decay timers.
//
// This module exists because two separate pieces of work ended up calling the
// same thing a lodger. hud_decay was left in vg_flight.cpp with a note saying it
// wanted a home, and the alerts were about to be filed under the state machine
// for no better reason than sitting next to it. Neither is simulation and
// neither is drawing: it is the panel reacting, which is its own subject.

// The caution annunciators: how close the thing is, how fast that should blink,
// and the beep that goes with each time it lights.
//
// HERE, not in the HUD, for two reasons. The phase has to be advanced by dt and
// the draw does not have one; and a sound triggered from drawing code is a sound
// that does not happen when the panel is not drawn, which is a very strange rule
// for a warning.
//
// `k` is 0 at the far edge of the alert's range and 1 at the thing itself, so
// THE RATE IS THE RANGE: a player who cannot spare attention for a number can
// still feel a rhythm getting faster. Both alerts share this because they now
// behave identically -- the missile alert had its own double-beat shape and it
// read as a flicker.
static void alert_step(float* phase, bool* lit, bool active, float k, float dt,
                       SfxId cue) {
    if (!active) { *phase = 0.0f; *lit = false; return; }

    if (k < 0.0f) k = 0.0f; else if (k > 1.0f) k = 1.0f;
    const float period = ALERT_FLASH_SLOW
                       + (ALERT_FLASH_FAST - ALERT_FLASH_SLOW) * k;

    // Advanced, never sampled. The rate follows the period smoothly and the
    // phase cannot jump when the period changes.
    *phase += dt / period;
    if (*phase >= 1.0f) *phase -= (float)(int)*phase;

    const bool now = (*phase <= ALERT_FLASH_DUTY);
    if (now && !*lit) vg_sfx_play(cue, 1.0f);   // on the lit edge, once
    *lit = now;
}

void vg_update_alerts(float dt, bool alive) {
    if (!alive) {
        vg.alert_msl_ph = vg.alert_wall_ph = 0.0f;
        vg.alert_msl_lit = vg.alert_wall_lit = false;
        return;
    }
    const bool msl = vg.threat && vg.threat_range <= MSL_ALERT_RANGE;
    alert_step(&vg.alert_msl_ph, &vg.alert_msl_lit, msl,
               msl ? (1.0f - vg.threat_range / MSL_ALERT_RANGE) : 0.0f,
               dt, SFX_MSL_ALERT);

    const bool wall = (vg.wall_clear <= ARENA_ALERT_RANGE);
    alert_step(&vg.alert_wall_ph, &vg.alert_wall_lit, wall,
               wall ? (1.0f - vg.wall_clear / ARENA_ALERT_RANGE) : 0.0f,
               dt, SFX_WALL_ALERT);
}

// The cockpit's own timers: what the panel is doing about things that already
// happened. None of it is simulation -- the hull is not healing here, a light is
// going out.
void vg_hud_decay(float dt) {
    // Faster than the damage vignette. A flash is the arrival of light, not a
    // state the cockpit sits in.
    if (vg.blast_flash > 0) {
        vg.blast_flash -= dt * 4.2f;
        if (vg.blast_flash < 0) vg.blast_flash = 0;
    }
    if (vg.hit_flash     > 0) vg.hit_flash     -= dt;
    if (vg.hud_boot      > 0) vg.hud_boot      -= dt;
    if (vg.damage_glitch > 0) vg.damage_glitch -= dt;
}
