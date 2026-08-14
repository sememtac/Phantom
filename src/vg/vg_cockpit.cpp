#include "vg_sim.h"
#include "vg_weapons.h"
#include "vg_sfx.h"
#include "vg_raster.h"

Cockpit vg_cockpit;

// The boot chain to zero. Called from vg_game_init, which is what begin_record restarts
// the game through before a recording -- see the note in vg_cockpit.h.
void vg_cockpit_clear(void) {
    vg_cockpit.boot        = 0.0f;
    vg_cockpit.cued        = false;
    vg_cockpit.radio_t     = 0.0f;
    vg_cockpit.regions_lit = 0;
    vg_cockpit.ready       = false;
}

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
        vg.alerts.msl_ph = vg.alerts.wall_ph = 0.0f;
        vg.alerts.msl_lit = vg.alerts.wall_lit = false;
        return;
    }
    const bool msl = vg_threat.on && vg_threat.range <= MSL_ALERT_RANGE;
    alert_step(&vg.alerts.msl_ph, &vg.alerts.msl_lit, msl,
               msl ? (1.0f - vg_threat.range / MSL_ALERT_RANGE) : 0.0f,
               dt, SFX_MSL_ALERT);

    const bool wall = (vg.wall_clear <= ARENA_ALERT_RANGE);
    alert_step(&vg.alerts.wall_ph, &vg.alerts.wall_lit, wall,
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
    if (vg_cockpit.boot      > 0) vg_cockpit.boot      -= dt;
    if (vg.damage_glitch > 0) vg.damage_glitch -= dt;
    // The cockpit arriving, which belongs here for the same reason the rest does: it is what the
    // panel is doing, not what the ship is doing. It carries the ramp that follows the sequence
    // as well, so it has to be called past the end of it -- which is why the return is ignored.
    vg_canopy_intro_update(dt);

    // A BEEP PER REGION, one for each that has latched since the last frame.
    //
    // A while loop rather than a test, so two regions coming up in one frame produce two beeps
    // rather than one. At the current rate they are 0.24 s apart and it cannot happen -- but the
    // rate is a dial now, and a dial that silently eats a beep when it is turned up is worse than
    // a loop that costs nothing.
    //
    // Pitched up per region, so the four read as a rising figure rather than four identical ticks.
    // The count is what rises, not the region index, which keeps it right if a drawing ever has a
    // different number of them.
    {
        const int lit = vg_canopy_intro_lit();
        while ((int)vg_cockpit.regions_lit < lit) {
            vg_sfx_play(SFX_PANEL_ON, 1.0f + 0.10f * (float)vg_cockpit.regions_lit);
            vg_cockpit.regions_lit++;
        }
    }

    // THE BOOT CHAIN. Three things used to start together and read as one muddle; each now waits
    // for the one before it.
    //
    // The instruments are cued off the COCKPIT rather than off a timer, so the two cannot drift
    // apart when the pacing is retuned. SFX_READY moves here with them: it is the panel finishing,
    // and it was previously playing at the moment the match began, a second and a half before
    // anything was lit.
    //
    // vg_canopy_intro_cued is a latch and not a progress test, because THIS FUNCTION RUNS IN THE
    // TITLE SCREEN -- it is reached from vg_world_step, which the attract loop and the cutscene
    // both drive. A comparison against a progress value that reads 1.0 when nothing is running is
    // satisfied by default, and the cockpit's power-on sound played over the menu.
    if (!vg_cockpit.cued && vg_canopy_intro_cued()) {
        vg_cockpit.cued = true;
        vg_cockpit.boot = HUD_BOOT_TIME;
        vg_cockpit.radio_t  = BOOT_RADIO_WAIT;
        vg_sfx_play(SFX_READY, 1.0f);
    }
    // ...and the radio opens BOOT_RADIO_WAIT after that sound -- from the cue, not from the end of
    // the flicker. The author's specification is one second after the panel's power-on cue, and
    // measuring it from the sound is what makes that true no matter how long HUD_BOOT_TIME is or
    // where in the cockpit sequence the cue falls.
    if (vg_cockpit.cued && !vg_cockpit.ready) {
        vg_cockpit.radio_t -= dt;
        if (vg_cockpit.radio_t <= 0.0f) { vg_cockpit.radio_t = 0.0f; vg_cockpit.ready = true; }
    }
}
