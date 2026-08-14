#pragma once
#include "vg_game.h"

// ===========================================================================
// THE COCKPIT'S OWN CLOCK
//
// What the PANEL is doing about things that have already happened -- not what the
// ship is doing, which is vg_flight.h, and not what is drawn, which is vg_draw.h.
// The distinction is the reason both of these take a dt and neither draws anything:
// a phase has to be advanced whether or not the panel is on screen, and a sound
// triggered from drawing code does not happen when the panel is not drawn.
//
// The fourth of the headers item 2 said were missing, and it is here now because it
// finally has something to hold. It is also where the cockpit boot group belongs --
// hud_boot, hud_cued, radio_t, regions_lit, ready -- which item 2 left on VgGame for
// having "no dominant owner". It has one; the state has simply not moved yet.
// ===========================================================================

// The caution annunciators. Here rather than in the HUD because the phase has to
// be advanced by dt and a draw has no dt -- and because a sound triggered from
// drawing code does not happen when the panel is not drawn.
void vg_update_alerts(float dt, bool alive);

// The panel's own timers: hit flash, boot sweep, damage glitch, blast flash.
//
// Named for what it decays, not for the file it is in -- see the note on the
// vg_hud_ prefix in vg_raster.h. It belongs to the cockpit and it is under the
// cockpit banner above: it also drives the canopy intro and the per-region beeps,
// which is panel work rather than anything vg_hud.cpp draws.
void vg_hud_decay(float dt);

// THE BOOT CHAIN'S STATE, which item 2 could not place and this header now can.
//
// It was left on VgGame for having "no dominant owner", and by the test used at the time
// it did not have one: vg_render.cpp reads `boot` more often than vg_cockpit.cpp does.
// That test was counting the wrong thing. **Drawing code reads everything -- that is what
// drawing is.** vg_cockpit.cpp is the only file that ASSIGNS the driving values; everyone
// else either resets the chain or draws from it. The owner is who writes.
//
// The prefix is dropped on the way in -- the old vg.hud_boot is vg_cockpit.boot -- which is
// the same trim vg.shake_x -> vg_shake.x and vg.course -> vg_course.gate took.
struct Cockpit {
    float   boot;         // >0 while the instruments are coming up
    // THE SEQUENCE'S HANDSHAKE. The canopy comes up first and only when it is nearly done
    // does `cued` fire and start `boot`; `ready` follows once the radio wait has run out,
    // and BOTH the broadcast and the opponent wait on that.
    bool    cued;         // the cockpit sequence has called for the instruments
    float   radio_t;      // >0 while the radio is still held shut after SFX_READY
    uint8_t regions_lit;  // how many cockpit regions have already been beeped for
    bool    ready;        // the radio may open

    // THE CAUTION ANNUNCIATORS, decided in the update and not in the draw.
    //
    // They used to be fmodf(state_t, period) inside the HUD, and a modulo of an
    // ever-growing clock by a CHANGING period does not merely change rate -- the phase
    // jumps every time the period moves. The faster the range changed the more it
    // scrambled, so the blink rate tracked the SHIP'S SPEED instead of the distance, and
    // flying away from a wall beeped exactly as fast as flying into one. A phase advanced
    // by dt cannot do that.
    //
    // Kept nested rather than flattened into the five above: they are a different clock
    // with a different owner-within-the-owner, and vg_cockpit.alerts.wall_lit says which.
    struct {
        float msl_ph,  wall_ph;
        bool  msl_lit, wall_lit;
    } alerts;
};

extern Cockpit vg_cockpit;

// Everything to zero.
//
// NEEDED, and for the reason this rule has produced every time since vg_course: these five
// relied on the memset in vg_game_init and nothing else zeroes them at boot. vg_match_start
// assigns them, but that runs at a match and not before one -- and begin_record restarts the
// game through vg_game_init before every recording, so getting this wrong reads as "the
// simulation is not deterministic" rather than as five fields that stopped being zeroed.
void vg_cockpit_clear(void);

