#include "vg_sim.h"
#include "vg_weapons.h"
#include "vg_sfx.h"
#include "vg_raster.h"
#include "vg_flight.h"
#include "vg_canopy_draw.h"
#include "vg_cockpit.h"

Cockpit vg_cockpit;

// The boot chain to zero. Called from vg_game_init, which is what begin_record restarts
// the game through before a recording -- see the note in vg_cockpit.h.
void vg_cockpit_clear(void) {
    // THE WHOLE STRUCT, not a list of fields, and the list is why.
    //
    // This cleared five boot fields when the boot chain was all that lived here. Then the
    // annunciators arrived, then the missile banner, then the three flashes -- and none of
    // them was added, because a clear written as a list does not complain when the struct
    // grows past it. Three groups went uncleared for three commits.
    //
    // It matters where the clear rule always matters: begin_record restarts the game
    // through vg_game_init before every recording, so a match that ended with a banner up
    // would have opened the next recording showing one. That reads as "the simulation is
    // not deterministic" rather than as a field that stopped being zeroed.
    vg_cockpit = Cockpit{};
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
        vg_cockpit.alerts.msl_ph = vg_cockpit.alerts.wall_ph = 0.0f;
        vg_cockpit.alerts.msl_lit = vg_cockpit.alerts.wall_lit = false;
        return;
    }
    const bool msl = vg_threat.on && vg_threat.range <= MSL_ALERT_RANGE;
    alert_step(&vg_cockpit.alerts.msl_ph, &vg_cockpit.alerts.msl_lit, msl,
               msl ? (1.0f - vg_threat.range / MSL_ALERT_RANGE) : 0.0f,
               dt, SFX_MSL_ALERT);

    const bool wall = (vg_wall.clearance <= ARENA_ALERT_RANGE);
    alert_step(&vg_cockpit.alerts.wall_ph, &vg_cockpit.alerts.wall_lit, wall,
               wall ? (1.0f - vg_wall.clearance / ARENA_ALERT_RANGE) : 0.0f,
               dt, SFX_WALL_ALERT);
}

// The cockpit's own timers: what the panel is doing about things that already
// happened. None of it is simulation -- the hull is not healing here, a light is
// going out.
void vg_hud_decay(float dt) {
    // THE MISSILE BANNER, working through its own backlog. Here because it is exactly what
    // this function is for -- what the panel is doing about something that already
    // happened -- and it spent its life in the radio module for want of anywhere better.
    if (vg_cockpit.banner.t > 0) {
        vg_cockpit.banner.t -= dt;
        if (vg_cockpit.banner.t <= 0) {
            if (vg_cockpit.banner.qn > 0) {
                vg_cockpit.banner.ev = vg_cockpit.banner.queue[0];
                for (int i = 1; i < vg_cockpit.banner.qn; i++) vg_cockpit.banner.queue[i - 1] = vg_cockpit.banner.queue[i];
                vg_cockpit.banner.qn--;
                // Held briefly when more are stacked up, so a salvo reports
                // itself out promptly instead of trailing the fight.
                vg_cockpit.banner.t = vg_cockpit.banner.qn ? MSL_BANNER_FAST : MSL_BANNER;
            } else {
                vg_cockpit.banner.ev = MSL_NONE;
            }
        }
    }

    // Faster than the damage vignette. A flash is the arrival of light, not a
    // state the cockpit sits in.
    if (vg_cockpit.flash.blast > 0) {
        vg_cockpit.flash.blast -= dt * 4.2f;
        if (vg_cockpit.flash.blast < 0) vg_cockpit.flash.blast = 0;
    }
    if (vg_cockpit.flash.hit     > 0) vg_cockpit.flash.hit     -= dt;
    if (vg_cockpit.boot      > 0) vg_cockpit.boot      -= dt;
    if (vg_cockpit.flash.glitch > 0) vg_cockpit.flash.glitch -= dt;
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
