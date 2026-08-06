#include "vg_canopy_set.h"

// ===========================================================================
// THE COCKPITS, AND THE ONLY PLACE THE DRAWINGS ARE INCLUDED
//
// That last part is not a style preference. A generated header defines its tables
// as `static`, so every translation unit that includes one gets its OWN COPY in
// flash -- and the first version of this table lived in a header, which put a
// second 36 KB copy of the reference drawing in the binary. Harmless at one
// drawing and multiplied by every hull the artist adds, which is precisely when it
// would have been noticed as "canopies are expensive" rather than as a mistake.
//
// So the data has one owner. The rasteriser never includes a drawing; it is handed
// one through vg_canopy_use and holds a pointer.
//
// THIS TABLE IS THE WHOLE INTERFACE FOR THE ARTIST PASS. Adding a cockpit is
// baking a drawing and editing one row; nothing else in the firmware is touched.
//
//   1. draw it -- the authoring rules are in tools/README.md
//   2. python tools/canopy_bake.py design/Chariot.png src/vg/vg_canopy_chariot.h \
//          --name CANOPY_CHARIOT
//   3. #include it below
//   4. point that hull's row at it
//
// EVERY HULL FLIES THE SAME DRAWING TODAY, which is a statement of where the work
// is rather than a placeholder to tidy. The reference cockpit is the one the whole
// feature was tuned against, so it is the right thing to fall back to: a cockpit
// that is not this hull's is something an artist can see and judge, where an empty
// frame would only look broken.
//
// WORTH KNOWING BEFORE JUDGING A NEW DRAWING. The four hulls already differ in how
// the frame MOVES without differing in what it looks like -- CANOPY_LAG_HULL rides
// ShipSpec::shake, so a CHARIOT's cockpit is visibly looser than a BALLISTA's on
// the same art. Some of what is felt is the airframe, not the drawing.
// ===========================================================================
#include "vg_canopy_data.h"

static const VgCanopy* const SET[SHIP_CLASSES] = {
    /* AEGIS    */ &CANOPY,
    /* LANCE    */ &CANOPY,
    /* CHARIOT  */ &CANOPY,
    /* BALLISTA */ &CANOPY,
};

// Clamped rather than trusted. ShipClass is a uint8_t restored from a save file,
// and one written by a build with more hulls in it would index off the end.
const VgCanopy* vg_canopy_for(ShipClass c) {
    return SET[(c < SHIP_CLASSES) ? c : SHIP_AEGIS];
}

const VgCanopy* vg_canopy_default(void) { return &CANOPY; }
