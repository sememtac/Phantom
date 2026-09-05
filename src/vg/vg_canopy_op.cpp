#include "vg_canopy_op.h"
#include "vg_canopy_draw.h"
#include "vg_raster.h"
#include "vg_config.h"
#include <Arduino.h>

// The opaque canopy. See vg_canopy_op.h for what this is an experiment in.

bool vg_canopy_op_on = true;

static const VgCanOp* s_cur = nullptr;

void vg_canopy_op_use(const VgCanOp* c) { s_cur = c; }
const VgCanOp* vg_canopy_op_current(void) { return s_cur; }

// THE OUTLINE'S COLOUR, in NATIVE order.
//
// cfg_palette stores every colour pre-swapped, because the panel takes RGB565
// big-endian and the raster copies entries out untouched. A blend has to unpack
// channels, so it wants the natural order and swaps at both ends -- the same pair
// of swaps the antialiased line path already pays.
//
// FAINT BY DEFAULT, and that is the point of it. At a full additive COL_HUD the
// outline read as a drawn line rather than as a lit edge, which loses the one thing
// this half of the cockpit is for: the metal is paint and the outline is a
// RELATIONSHIP with whatever is behind it. Held down to CANOPY_OP_EDGE, it reads as
// glass catching the panel.
//
// AND IT IS NOW THE WHOLE OF THE WALL WARNING. The delta cockpit turns its members
// red -- one colour table, nothing per pixel -- and there are no members here to
// turn: they are the artist's own paint. So the outline carries the signal instead,
// and it carries it BOTH WAYS: vg_canopy_alarm_colour pulls the hue from amber to
// COL_DANGER and to white on a strobe, and the same level ramps the brightness from
// CANOPY_OP_EDGE up to full. Starting quiet is what makes getting loud mean
// something.
//
// Recomputed a BAND rather than cached against the alarm's own quantiser: fifteen
// mixes a frame against a per-pixel pass is not a number worth keeping state for.
static inline uint16_t outline_native(float extra) {
    float          al   = 0.0f;
    const uint16_t base = vg_canopy_alarm_colour(&al);
    if (extra > al) al = extra;
    const uint16_t c    = vg_dim(base, CANOPY_OP_EDGE + (1.0f - CANOPY_OP_EDGE) * al);
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

    // WHAT EACH REGION IS DOING, resolved once a band instead of once a span.
    //
    // Two questions, and the baker's refusal to let a run straddle two regions is what
    // makes both of them one array lookup: does this region's cockpit exist yet, and how
    // hot is its lit edge running. The heat is the arrival's -- the delta cockpit spends
    // it on a colour table per region, and a painted cockpit has no table to spend it on,
    // so it goes where the light already is.
    bool     live[VG_CANOPY_MAX_ZONES];
    uint16_t edge[VG_CANOPY_MAX_ZONES];
    const int nz = (int)c->zones;
    for (int z = 0; z < nz && z < VG_CANOPY_MAX_ZONES; z++) {
        live[z] = vg_canopy_zone_live(z);
        edge[z] = outline_native((float)vg_canopy_zone_glow(z) * (1.0f / 255.0f));
    }

    // IS ANYTHING HAPPENING TO A REGION AT ALL. False for almost every frame of a
    // match, and when it is false the walk below is skipped whole -- the region map
    // is 2,001 runs and there is no reason to read it to discover that the cockpit is
    // simply present.
    const bool gate = vg_canopy_gate_on();

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

        // THE VIEW FIRST, THEN THE COCKPIT ON TOP OF IT, which is the order the delta
        // path draws in and for the same reason: everything behind the canopy is already
        // in the band and the instruments come after, so this is the one point where
        // blacking a region hides the world without touching the panel.
        //
        // RIGID. It reads y, not sy, and ignores mo entirely -- a region of the VIEW does
        // not swing with the frame. The delta path makes the same call and says so.
        //
        // No column mask here. The delta path keeps one so a single faulty panel does not
        // cost a full-screen walk to find; this map is 4.2 runs a row against that one's
        // whole zone table, and the gate is off except during an arrival or a hit.
        if (gate) {
            const uint16_t g0 = c->zrow[y], g1 = c->zrow[y + 1];
            for (uint16_t gi = g0; gi < g1; gi++) {
                const VgCanOpZone* zr = &c->zone[gi];
                vg_canopy_gate_run(dst_row, y, y, (int)zr->x0, (int)zr->len,
                                   (int)zr->zone);
            }
        }

        for (uint16_t si = s0; si < s1; si++) {
            const VgCanOpSpan* sp = &c->span[si];
            // A REGION THAT HAS NOT ARRIVED HAS NO COCKPIT IN IT. One test a run, which
            // is the whole reason a run is not allowed to straddle two regions.
            if ((int)sp->zone >= nz || !live[sp->zone]) continue;

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
                const uint16_t lit = edge[sp->zone];
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
