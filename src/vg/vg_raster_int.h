#pragma once
#include <stdint.h>
#include "vg_config.h"

// Internal contract between the two halves of the rasteriser. Not part of the
// public API in vg_raster.h.
//
//   vg_raster.cpp  SUBMIT side -- clip, warp, rotate, append primitives
//   vg_band.cpp    RASTER side -- draw a band from that list, blit it
//
// Splitting along this seam matters because the two halves have completely
// different performance characteristics: submit runs once per frame and costs
// frame time directly, while the band raster runs 15 times and hides under DMA.

// PRIM_SKY is the rear-view patch's backdrop. It has to be a primitive rather
// than part of the band clear: the clear happens before anything is drawn, so a
// sky put there would be painted over by the forward scene, which has no reason
// to avoid the patch. As a primitive it lands in submission order -- after the
// world, before the patch's own geometry -- and being opaque it is also what
// hides the forward scene inside the patch.
// PRIM_CANOPY is the baked cockpit frame, and it is a primitive for exactly the same
// reason PRIM_SKY is: WHERE it happens in the order matters. The frame goes over the
// world and under the instruments, and a table applied outside the primitive list
// could only be before everything or after it.
// PRIM_BEZEL is the console chassis behind a menu, and it is here for the third
// time for the same reason: ORDER. It goes over the idle scene and under the
// menu. Unlike the canopy it is opaque -- it replaces the pixel rather than
// lighting it, because a machine in a room does not let the stars through.
enum : uint8_t { PRIM_LINE = 0, PRIM_POINT, PRIM_FILL, PRIM_GLYPH, PRIM_TRI,
                 PRIM_SKY, PRIM_CANOPY, PRIM_BEZEL };

// ymin/ymax are precomputed at submit time so the per-band overlap test is two
// compares with no type dispatch. x2/y2 are only used by PRIM_TRI; carrying them
// costs 4 bytes on every primitive but keeps ONE flat list, which is what makes
// submission order -- and therefore painter ordering -- trivially correct.
struct Prim {
    int16_t  x0, y0, x1, y1;
    int16_t  x2, y2;
    int16_t  ymin, ymax;
    uint16_t color;
    uint8_t  type;
    // PRIM_LINE only, and a MODE rather than a flag -- see LINE_* below. Rides in what
    // was padding, so per-line quality AND per-line blending cost no memory and no extra
    // cache traffic. Keeping it one byte also keeps Prim at 20, which the join's word
    // copy relies on: sizeof(Prim) must stay a multiple of 4.
    uint8_t  aa;
};
static_assert(sizeof(Prim) % 4 == 0, "Prim must stay word-sized for vg_prim_join");

// How a line puts itself down. OPAQUE and AA state a colour; ADD and SUB state a
// CHANGE to what is already there, which is what lets the canopy read as a lit rib
// instead of a decal -- it holds its relationship to the backdrop over a dark nebula
// and a bright one alike. For ADD and SUB, `color` is the DELTA, not the result.
enum : uint8_t { LINE_OPAQUE = 0, LINE_AA = 1, LINE_ADD = 2, LINE_SUB = 3 };

// --- owned by vg_raster.cpp ---
bool        vg_prim_init(void);
const Prim* vg_prim_list(void);
int         vg_prim_live(void);

// --- owned by vg_band.cpp ---
bool        vg_band_init(void);

// How many band buffers there are, for the init report. See BAND_BUFS.
int vg_band_bufs(void);

// WHERE A BAND IS CUT for the two-core row split. Shared: the canopy, the scanlines and
// the backdrop all take half a band each, and the canopy is in another unit now.
enum { ROW_SPLIT = BAND_H / 2 };
