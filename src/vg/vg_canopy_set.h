#pragma once
#include "vg_ship.h"

// Which cockpit a hull flies. Defined in vg_canopy_set.cpp, which is the ONLY
// translation unit that includes the generated drawings -- see the note there.
//
// MAY RETURN NULL, and a caller must handle that. A hull with no drawing has no cockpit
// rather than borrowing another hull's -- there is no reference drawing to fall back to.
struct VgCanOp;
const VgCanOp* vg_canopy_op_for(ShipClass c);
