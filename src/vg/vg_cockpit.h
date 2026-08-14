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
