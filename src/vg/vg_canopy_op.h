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

// ONE RUN OF THE REGION MAP, which is a different question from the span table.
//
// A span says WHAT IS DRAWN. This says WHICH REGION OF THE VIEW a pixel is in, and it
// covers every pixel of the panel -- the panes included, which the span table stores
// nothing for. Both things that act on a region act on the whole of it: the arrival
// holds a region black and dissolves the world out of it, and a round through the
// canopy takes a region to white and then to static. Neither is possible against a
// list that only knows where the metal is.
//
// Separate rather than a flag on a span because the two are read at different moments.
// The span table MOVES with the frame -- the spring, the bend, the shear -- and this
// does not: the gate is drawn rigid, so it lands where the view is rather than where
// the cockpit has swung to. The delta canopy splits them for the same reason.
struct VgCanOpZone {
    uint16_t x0;
    uint16_t len;
    uint8_t  zone;
};

struct VgCanOp {
    const uint16_t*    pal;     // RGB565, pre-swapped, one entry per index
    const uint8_t*     data;    // one palette index per stored opaque pixel
    const VgCanOpSpan* span;
    const uint16_t*    row;     // first span of each panel row, SCR_H + 1 entries

    const VgCanOpZone* zone;    // the region map
    const uint16_t*    zrow;    // first run of each panel row, SCR_H + 1 entries

    uint16_t spans;
    uint16_t zruns;
    uint32_t pixels;
    uint8_t  zones;
    uint8_t  centre;            // the region being looked THROUGH; never fails
};

// IS THE EXPERIMENT ON. Default true, because a branch called opaque-canopy that
// has to be argued into using it would answer a different question.
//
// BOTH BUILDS CAN TURN IT OFF NOW. The desktop takes --delta-canopy and the board
// takes 'o' and 'O' over the link -- see the block beside 'q' in vg_capture.cpp,
// which is the same argument about comparing two flights from memory. The board
// needed it for a reason that is worth recording: two BOARDS carrying two builds
// is not an A/B. A recorded session navigates the menus by tap COORDINATE, so a
// build whose menus have moved replays a different session entirely -- measured,
// on the same .phr: one board flew and the other never reached the cockpit, and
// the canopy counter read a clean zero without anything being wrong.
extern bool vg_canopy_op_on;

// WHICH DRAWING, or none. A null one costs a test and draws nothing, which is
// what every ship without an opaque bake gets.
void vg_canopy_op_use(const VgCanOp* c);
const VgCanOp* vg_canopy_op_current(void);

// THE ARRIVAL AND THE DAMAGE ARE NOT THIS FILE'S.
//
// There was a vg_canopy_op_reveal here -- a scalar 0..1 that let zones through in
// order -- and it was never driven by anything, which was the honest state of it: a
// cockpit arriving is not a fraction, it is a sequence with a flash, a dissolve and a
// cooling edge, and it is already written once in vg_canopy_draw.cpp. So is a hit.
//
// This draws under both of them instead. vg_canopy_zone_live says whether a region's
// metal exists yet, vg_canopy_zone_glow says how hot its edge is running, and
// vg_canopy_gate_run paints whatever the region is doing over the top -- all out of
// the one clock, so the two cockpits arrive and break identically. See the block at
// VgCanMotion in vg_canopy_draw.h; this is the same argument a second time.

// Paint this core's rows of one band. Called from the band raster, in the same
// place and under the same primitive as the delta canopy.
void vg_canopy_op_rows(uint16_t* band, int by0, int r0, int r1);
