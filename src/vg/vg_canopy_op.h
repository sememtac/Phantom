#pragma once
#include <stdint.h>

// ===========================================================================
// AN EXPERIMENT: THE CANOPY AS METAL, NOT AS LIGHT
//
// A canopy has always been a signed light DELTA applied over the finished
// picture -- see vg_canopy.h. The frame catches whatever is behind it, so a rib
// over a nebula glows with the nebula, and that is the whole reason a canopy is
// a delta rather than a decal: it is a RELATIONSHIP with the scene instead of a
// value painted on top of one.
//
// This asks the opposite question. Most of a cockpit is opaque metal, and metal
// does not catch light from the stars behind it -- it occludes them. So:
//
//   OPAQUE   the frame. A palette index a pixel, stored, replacing what was
//            there. Cheap: a store where a blend is a read, an add, a clamp and
//            a store.
//   ADDITIVE a thin outline tracing each pane, still a delta, still lit by
//            whatever it crosses. This is the part that has to stay a
//            relationship, because a lit edge that does not respond to the
//            scene reads as a sticker.
//   NOTHING  the panes themselves. Not stored at all, exactly as a bezel does
//            not store its screen aperture.
//
// WHAT IT COSTS AND WHAT IT BUYS is the point of the experiment. The CHARIOT's
// delta canopy paints 16,494 pixels a half-frame and that is most of its budget.
// This bake paints 45,591 opaque and 5,659 additive -- four times the area, but
// the expensive kind of pixel drops by a factor of six.
//
// THE DRAWING SAYS WHICH IS WHICH, by colour, which is the console chassis's
// rule rather than the canopy's: magenta is a pane, cyan is the outline, and
// anything else is metal. The ARRIVAL SEQUENCE moved to the alpha channel,
// because a full-colour drawing has no green channel to spare.
//
// If this does not work out -- on looks, on frame time, or on the art pipeline --
// the tag pre-opaque-canopy is the commit before any of it.
// ===========================================================================

enum : uint8_t {
    VG_CANOP_OPAQUE = 0,
    VG_CANOP_ADD    = 1,
};

// One run of pixels in a panel row. A run never straddles a change of KIND or of
// ZONE, so neither is ever tested per pixel.
struct VgCanOpSpan {
    uint16_t x0;
    uint16_t len;
    uint32_t off;       // into data. Meaningless for an additive run: it has none
    uint8_t  zone;      // arrival order
    uint8_t  kind;
};

struct VgCanOp {
    const uint16_t*    pal;     // RGB565, pre-swapped, one entry per index
    const uint8_t*     data;    // one palette index per stored opaque pixel
    const VgCanOpSpan* span;
    const uint16_t*    row;     // first span of each panel row, SCR_H + 1 entries

    uint16_t spans;
    uint32_t pixels;
    uint8_t  zones;
};

// IS THE EXPERIMENT ON. Default true, because a branch called opaque-canopy that
// has to be argued into using it would answer a different question. The desktop
// turns it off with --delta-canopy so the two can be compared in one sitting;
// nothing on the board can, which is deliberate for now -- flying it is the test.
extern bool vg_canopy_op_on;

// WHICH DRAWING, or none. A null one costs a test and draws nothing, which is
// what every ship without an opaque bake gets.
void vg_canopy_op_use(const VgCanOp* c);
const VgCanOp* vg_canopy_op_current(void);

// HOW MUCH OF IT HAS ARRIVED, 0..1. Zones are revealed in order, so the cockpit
// assembles rather than appearing -- the same thing the delta canopy does with
// its own zones, driven by the same boot chain.
void vg_canopy_op_reveal(float k);

// Paint this core's rows of one band. Called from the band raster, in the same
// place and under the same primitive as the delta canopy.
void vg_canopy_op_rows(uint16_t* band, int by0, int r0, int r1);
