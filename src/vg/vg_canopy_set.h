#pragma once
#include "vg_canopy.h"
#include "vg_ship.h"

// Which cockpit a hull flies. Defined in vg_canopy_set.cpp, which is the ONLY
// translation unit that includes the generated drawings -- see the note there.
//
// Clamped, never null: a hull with no drawing of its own gets the reference one.
const VgCanopy* vg_canopy_for(ShipClass c);

// The one every hull falls back to, for whoever needs a canopy before a ship has
// been chosen.
const VgCanopy* vg_canopy_default(void);
