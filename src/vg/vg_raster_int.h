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
enum : uint8_t { PRIM_LINE = 0, PRIM_POINT, PRIM_FILL, PRIM_GLYPH, PRIM_TRI,
                 PRIM_SKY };

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
    // PRIM_LINE only: draw antialiased. Rides in what was padding, so per-line
    // quality selection costs no memory and no extra cache traffic.
    uint8_t  aa;
};

// --- owned by vg_raster.cpp ---
bool        vg_prim_init(void);
const Prim* vg_prim_list(void);
int         vg_prim_live(void);

// --- owned by vg_band.cpp ---
bool        vg_band_init(void);
