#include "vg_canopy_set.h"
#include "vg_canopy_op.h"

// ===========================================================================
// WHICH COCKPIT A HULL FLIES, and the table is GENERATED
//
// A drawing is named after the hull that flies it -- design/canopy/chariot.png is
// the CHARIOT's cockpit -- and tools/canopy_set.py bakes the directory and writes
// the rows. So adding a canopy is dropping a PNG in and running one command. No C++
// is edited, and a drawing cannot be wired to the wrong hull, because the file name
// IS the wiring.
//
// It used to be a hand-edited table beside a hand-chosen C identifier and a
// hand-chosen output path: three things to keep consistent, nothing checking that
// they agreed, and the one step in the pipeline that was not "drop a file in".
//
// A HULL WITH NO DRAWING FLIES WITH NO COCKPIT. There is no default texture and no
// substitute, which is the author's call and the right one -- the reference drawing
// belongs to the CHARIOT, and a shared placeholder would put a cockpit that is not
// this hull's in front of the player and call it finished. nullptr is a supported
// selection: see vg_canopy_op_use, and the note in vg_canopy_intro_begin about the
// boot chain still having to run when there is no sequence to play.
//
// WORTH KNOWING BEFORE JUDGING A NEW DRAWING. The four hulls already differ in how
// the frame MOVES without differing in what it looks like -- CANOPY_LAG_HULL rides
// ShipSpec::shake, so a CHARIOT's cockpit is visibly looser than a BALLISTA's on the
// same art. Some of what is felt is the airframe, not the drawing.
//
// THIS FILE IS THE ONLY PLACE THE DRAWINGS ARE INCLUDED. A generated header defines
// its tables as `static`, so every translation unit that includes one gets its own
// copy in flash -- a table in a header once put a second 36 KB copy of a drawing in
// the binary, and that would multiply by every hull the artist adds.
// ===========================================================================
#include "generated/canopy_set.h"

// THE DRAWINGS: design/canopy/<hull>.png bakes to a row here. Opaque -- colour and
// coverage, metal that replaces the pixel -- see vg_canopy_op.h. It was a light delta
// until 2026-09-05, a second table sat beside this one for the other kind, and the
// frame's bend and the two-core split were built from the delta's columns even on a
// hull that flew the bake. They are built from the bake's own spans now, the delta
// table is gone, and this is the only set there is.
static const VgCanOp* const OP_SET[SHIP_CLASSES] = {
    VG_CANOPY_OP_SET_ROWS
};
static_assert(sizeof(OP_SET) / sizeof(OP_SET[0]) == SHIP_CLASSES,
              "the generated opaque set has a different number of hulls than ShipClass "
              "-- re-run tools/canopy_set.py");

// Clamped rather than trusted. ShipClass is a uint8_t restored from a save file, and
// one written by a build with more hulls in it would index off the end.
//
// MAY RETURN NULL, and callers must not assume otherwise. That is the point: a hull
// with no drawing has no canopy rather than borrowing another hull's.
const VgCanOp* vg_canopy_op_for(ShipClass c) {
    return OP_SET[(c < SHIP_CLASSES) ? c : SHIP_AEGIS];
}
