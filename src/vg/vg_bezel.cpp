#include "vg_bezel.h"
#include "vg_raster.h"
#include "vg_raster_int.h"
#include "vg_port.h"
#include "vg_config.h"
#include <Arduino.h>   // IRAM_ATTR

// The selected bezel, or none. Menus that have no chassis leave this null and
// the primitive returns immediately.
static const VgBezel* s_cur = nullptr;

void vg_bezel_use(const VgBezel* b) { s_cur = b; }
const VgBezel* vg_bezel_current(void) { return s_cur; }

// vg_bezel_prim lives in vg_raster.cpp, beside vg_canopy_prim: the primitive
// list's push() is private to that file.

// PAINT, DO NOT BLEND.
//
// One byte in, one palette lookup, one 16-bit store. The palette is 512 bytes and
// stays in cache across the whole frame, so the inner loop is a load, an indexed
// load and a store -- which is why a chassis covering 42% of the panel costs less
// than a cockpit frame covering 10% of it. The canopy has to read the destination
// back, add a signed delta to three channels and clamp each one.
//
// r0 and r1 are the CALLER'S rows and not the band's. Under the row split each
// core owns part of a band, and a store outside its share lands in the other
// core's rows.
void IRAM_ATTR vg_bezel_rows(uint16_t* band, int by0, int r0, int r1) {
    const VgBezel* b = s_cur;
    if (!b) return;

    for (int r = r0; r < r1; r++) {
        const int y = by0 + r;
        if (y < 0 || y >= SCR_H) continue;

        const uint16_t s0 = b->row[y], s1 = b->row[y + 1];
        uint16_t* dst_row = &band[r * SCR_W];

        for (uint16_t si = s0; si < s1; si++) {
            const VgBezelSpan* sp = &b->span[si];
            const uint8_t*  src = &b->data[sp->off];
            uint16_t*       dst = &dst_row[sp->x0];
            for (uint16_t i = 0; i < sp->len; i++)
                *dst++ = b->pal[*src++];
        }
    }
}
