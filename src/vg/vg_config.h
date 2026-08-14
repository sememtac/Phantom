#pragma once

// Umbrella header. Tuning lives in the cfg_* files, split by concern so a change
// to, say, missile balance does not mean scrolling past the palette and the
// touch zones to find it.
//
//   cfg_display.h  screen, orientation, banding, projection, scanlines, HUD warp
//   cfg_palette.h  every colour, and the byte-swap convention behind them
//   cfg_flight.h   speed, steering, agility, hull integrity, damage
//   cfg_combat.h   missiles, player weapons, enemy behaviour
//   cfg_world.h    arena, asteroids, stars, motes, spawning and culling
//   cfg_events.h   episodic match events: the anomaly, the surge
//   cfg_hud.h      HUD layout, radar geometry, touch zones, gestures
//   cfg_econ.h     credit purses, repair pricing, the bank ceiling

#include "cfg_display.h"
#include "cfg_palette.h"
#include "cfg_flight.h"
#include "cfg_combat.h"
#include "cfg_world.h"
#include "cfg_events.h"
#include "cfg_hud.h"
#include "cfg_econ.h"
