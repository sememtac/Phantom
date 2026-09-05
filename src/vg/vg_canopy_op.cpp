#include "vg_canopy_op.h"
#include "vg_canopy_draw.h"
#include "vg_config.h"
#include <Arduino.h>

// The opaque canopy. See vg_canopy_op.h for what this is an experiment in.

bool vg_canopy_op_on = true;

static const VgCanOp* s_cur    = nullptr;
static float          s_reveal = 1.0f;

void vg_canopy_op_use(const VgCanOp* c) { s_cur = c; }
const VgCanOp* vg_canopy_op_current(void) { return s_cur; }

void vg_canopy_op_reveal(float k) {
    s_reveal = (k < 0.0f) ? 0.0f : (k > 1.0f ? 1.0f : k);
}

// THE OUTLINE'S COLOUR, in NATIVE order.
//
// cfg_palette stores every colour pre-swapped, because the panel takes RGB565
// big-endian and the raster copies entries out untouched. A blend has to unpack
// channels, so it wants the natural order and swaps at both ends -- the same pair
// of swaps the antialiased line path already pays.
//
// One constant for every pixel of every outline: the drawing says WHERE the lit
// edge is and this says what lit means. It is the HUD's own amber, so the edge
// belongs to the instrument layer rather than to the paintwork.
static inline uint16_t outline_native(void) {
    const uint16_t c = COL_HUD;
    return (uint16_t)((c >> 8) | (c << 8));
}

// Saturating add, per channel, on a pixel already known to be inside the band.
// The same arithmetic blend_px does in vg_band.cpp; it is static there and this
// is fifteen lines rather than a header shared for one caller.
static inline void add_px(uint16_t* p, uint16_t d_native) {
    const uint16_t s = (uint16_t)((*p >> 8) | (*p << 8));

    uint32_t r = (s >> 11) & 31u, g = (s >> 5) & 63u, b = s & 31u;
    r += (d_native >> 11) & 31u; if (r > 31u) r = 31u;
    g += (d_native >>  5) & 63u; if (g > 63u) g = 63u;
    b +=  d_native        & 31u; if (b > 31u) b = 31u;

    const uint16_t o = (uint16_t)((r << 11) | (g << 5) | b);
    *p = (uint16_t)((o >> 8) | (o << 8));
}

// ONE POINT ONTO THE SPHERE, along the column. warp_y in vg_canopy_draw.cpp is the
// same three lines against the same state; it is static there and inlining it here
// is cheaper than a call a span. If one is ever changed the other has to be, and the
// motion they share comes out of vg_canopy_motion so that only THIS can drift.
static inline int sphere_x(int x, float zbase, float zk) {
    const float d = (float)(x - SCR_CY);
    return (int)((float)SCR_CY + d * (zbase + zk * d * d) + 0.5f);
}

void IRAM_ATTR vg_canopy_op_rows(uint16_t* band, int by0, int r0, int r1) {
    const VgCanOp* c = s_cur;
    if (!c) return;

    // Hoisted out of the loops, for the reason vg_bezel_rows gives: the palette
    // stays in cache all frame, and reloading a base pointer through two levels
    // of struct on every pixel is work the compiler cannot always see through.
    const uint16_t* pal  = c->pal;
    const uint8_t*  data = c->data;
    const uint16_t  lit  = outline_native();

    // HOW MANY ZONES HAVE ARRIVED. Compared against a span's zone, so the gate is
    // one integer test a RUN rather than anything per pixel -- which is the whole
    // reason the baker refuses to let a run straddle two zones.
    const int shown = (s_reveal >= 1.0f)
                    ? (int)c->zones
                    : (int)(s_reveal * (float)c->zones + 0.5f);

    for (int r = r0; r < r1; r++) {
        const int y = by0 + r;
        if (y < 0 || y >= SCR_H) continue;

        // THE COCKPIT MOVES BY BEING READ SOMEWHERE ELSE. The spring, the throttle
        // bend and the roll shear all arrive as one row index and one displacement
        // along it -- see VgCanMotion. A rigid frame answers false and the whole of
        // the rest of this loop is the straight blit it was before.
        VgCanMotion mo;
        const bool  mv = vg_canopy_motion(y, &mo);
        const int   sy = mv ? mo.row : y;

        const uint16_t s0 = c->row[sy], s1 = c->row[sy + 1];
        uint16_t* dst_row = &band[r * SCR_W];

        for (uint16_t si = s0; si < s1; si++) {
            const VgCanOpSpan* sp = &c->span[si];
            if ((int)sp->zone >= shown) continue;

            const int len = (int)sp->len;
            int d0 = (int)sp->x0, d1 = (int)sp->x0 + len;
            int c0 = d0, c1 = d1;
            bool ext = false;

            if (mv) {
                d0 = sphere_x(d0, mo.zbase, mo.zk) + mo.xofs;
                d1 = sphere_x(d1, mo.zbase, mo.zk) + mo.xofs;
                if (d1 <= d0) continue;
                c0 = d0; c1 = d1;
                // CLAMPED TO THE EDGE, NOT DROPPED, the same call canopy_rows_t
                // makes on the other axis: a run that reached a border keeps
                // reaching it, so the frame reads as continuing past the screen
                // rather than sliding away from it and leaving sky.
                if (sp->x0 == 0)         { c0 = 0;     ext = true; }
                if ((int)sp->x0 + len >= SCR_W) { c1 = SCR_W; ext = true; }
            }

            if (c0 < 0)     c0 = 0;
            if (c1 > SCR_W) c1 = SCR_W;
            if (c1 <= c0) continue;

            uint16_t* dst = &dst_row[c0];
            int       n   = c1 - c0;

            if (sp->kind == VG_CANOP_ADD) {
                // The lit edge. A pixel at a time: there is no pairing to be had
                // here the way there is for a store, because each one is a read,
                // an unpack, three clamps and a write.
                while (n-- > 0) add_px(dst++, lit);
                continue;
            }

            const uint8_t* src = &data[sp->off];

            // STRETCHED, which the sphere does and a translation does not. The run
            // holds a palette index a pixel, so a run that lands longer or shorter
            // than it was drawn has to be RESAMPLED -- nearest neighbour, on a 16.16
            // step, clamped at both ends so the border extension above repeats the
            // edge pixel instead of reading off the run.
            if (ext || (d1 - d0) != len) {
                const int32_t step = (int32_t)(((int64_t)len << 16) / (d1 - d0));
                const int32_t top  = (int32_t)(len - 1) << 16;
                int32_t acc = (int32_t)((int64_t)(c0 - d0) * step);
                while (n-- > 0) {
                    const int32_t a = (acc < 0) ? 0 : (acc > top ? top : acc);
                    *dst++ = pal[src[a >> 16]];
                    acc += step;
                }
                continue;
            }

            // METAL, WHERE IT LANDED WHOLE. Two pixels to a store, exactly as the
            // chassis blit does -- the panel is 16-bit and the bus is 32, so a pair
            // is one word. A span starts at an arbitrary x, so an odd first pixel
            // goes out alone to reach alignment: an unaligned 32-bit store on Xtensa
            // is a fault rather than a slowdown, so this is correctness not tuning.
            src += (c0 - d0);
            if ((((uintptr_t)dst) & 2u) && n > 0) {
                *dst++ = pal[*src++];
                n--;
            }
            uint32_t* w = (uint32_t*)dst;
            while (n >= 2) {
                const uint16_t a = pal[src[0]];
                const uint16_t b = pal[src[1]];
                *w++ = (uint32_t)a | ((uint32_t)b << 16);
                src += 2;
                n   -= 2;
            }
            if (n > 0) *(uint16_t*)w = pal[*src];
        }
    }
}
