#pragma once
#include "vg_canopy.h"
#include "vg_ship.h"

// Which cockpit a hull flies. Defined in vg_canopy_set.cpp, which is the ONLY
// translation unit that includes the generated drawings -- see the note there.
//
// MAY RETURN NULL, and a caller must handle that. A hull with no drawing has no cockpit
// rather than borrowing another hull's -- there is no reference drawing to fall back to.
//
// This said "clamped, never null: a hull with no drawing of its own gets the reference
// one" until now, which described the set BEFORE the default was removed. The
// implementation has said MAY RETURN NULL the whole time, so the two files disagreed and
// the header was the one a caller reads first.
const VgCanopy* vg_canopy_for(ShipClass c);

// ...AND THE OPAQUE BAKE, for the hulls that have one. Null for the rest, which
// is every hull but the CHARIOT while this is an experiment -- see vg_canopy_op.h.
// Selected beside the delta one and preferred by the band pass when it is not
// null, so a hull with both drawn is a hull that flies the new one.
struct VgCanOp;
const VgCanOp* vg_canopy_op_for(ShipClass c);

// The one every hull falls back to, for whoever needs a canopy before a ship has
// been chosen.
const VgCanopy* vg_canopy_default(void);
