#include "vg_canopy_op.h"
#include "vg_canopy_draw.h"
#include "vg_raster.h"
#include "vg_config.h"
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <string.h>

// The opaque canopy. See vg_canopy_op.h for what this is an experiment in.

bool vg_canopy_op_on = true;

static const VgCanOp* s_cur = nullptr;

// EXPERIMENT: THE BAKE, RESIDENT, instead of streamed from flash.
//
// The tables are `static const`, which puts 116 KB of them in flash behind a data
// cache a fraction of that size, and the pass reads all of it every frame: the pixel
// bytes once each and the span table once. The delta bake is 21 KB, and a copy of it
// to SRAM measured nothing, which says it was already living in the cache. This one
// cannot, so every frame fetches it again -- and a line from QIO flash is a serial
// transaction that no amount of loop tuning can hide. The desktop cannot see this:
// its L2 holds the whole bake.
//
// Internal SRAM has 3 KB free, so the copy goes to PSRAM: the same cache in front,
// but octal at double data rate behind it, several times the flash's fill rate. An
// instrument, switched from the serial link ('r' / 'h' in vg_capture.cpp) so one
// flash can measure both. If it moves the needle, the real change is a smaller table
// or a residency decided at build time; this is not that change. The copy is made
// once and kept: there is one bake, and the game never lets go of it.
static const VgCanOp* s_src      = nullptr;    // what was asked for
static bool           s_resident = false;
static VgCanOp        s_copy;
static const VgCanOp* s_copy_of  = nullptr;

static const VgCanOp* resident(const VgCanOp* c) {
    if (!c || !s_resident) return c;
    if (s_copy_of == c) return &s_copy;
    const size_t npal = 256 * sizeof(uint16_t);
    const size_t nspn = (size_t)c->spans * sizeof(VgCanOpSpan);
    const size_t nrow = (size_t)(SCR_H + 1) * sizeof(uint16_t);
    uint16_t*    pal  = (uint16_t*)   heap_caps_malloc(npal,      MALLOC_CAP_SPIRAM);
    uint8_t*     data = (uint8_t*)    heap_caps_malloc(c->pixels, MALLOC_CAP_SPIRAM);
    VgCanOpSpan* span = (VgCanOpSpan*)heap_caps_malloc(nspn,      MALLOC_CAP_SPIRAM);
    uint16_t*    row  = (uint16_t*)   heap_caps_malloc(nrow,      MALLOC_CAP_SPIRAM);
    if (!pal || !data || !span || !row) {
        free(pal); free(data); free(span); free(row);
        return c;                                   // no room: the flash tables still work
    }
    memcpy(pal,  c->pal,  npal);
    memcpy(data, c->data, c->pixels);
    memcpy(span, c->span, nspn);
    memcpy(row,  c->row,  nrow);
    s_copy = *c;
    s_copy.pal = pal; s_copy.data = data; s_copy.span = span; s_copy.row = row;
    s_copy_of = c;
    return &s_copy;
}

void vg_canopy_op_resident(bool on) {
    s_resident = on;
    vg_canopy_op_use(s_src);
}

// THE PAINT, RUN HOT. One table a region, and the shape of this is copied from
// canopy_ilut rather than invented: the delta cockpit's members come up white and cool
// to their authored colour, and that cooling IS the arrival -- without it a region just
// switches on. It was the one part of the sequence the opaque bake could not do, because
// its members are the artist's own paint and there was no table between the paint and
// the panel.
//
// Now there is. Metal in a glowing region is read through this instead of through the
// palette, so the pixel costs exactly what it always did -- a lookup and a store. What it
// buys is that the paint RESOLVES OUT OF the region's white flash instead of appearing
// beside it, which is the thing worth having.
//
// 8 KB, and IN PSRAM, which is not where the delta path keeps its s_ilut and is not
// a matter of taste. Internal RAM is where the other core's stack has to come from,
// the rowsplit helper asks for 4 KB of it on the first band ever drawn, and this
// table as a static 8 KB was the difference between a helper that starts and one
// that does not -- see rowsplit_start in vg_band.cpp. Read through the cache like
// everything else in PSRAM; 512 bytes a region stays resident once touched. Rebuilt
// only when a region's quantised glow moves -- CANOPY_INTRO_QSTEP steps over the
// whole cooling -- so a match pays a couple of dozen 256-entry loops in total and
// nothing at all once the cockpit is up.
static uint16_t (*s_hot)[256] = nullptr;     // [VG_CANOPY_MAX_ZONES][256]
static int16_t  s_hot_q[VG_CANOPY_MAX_ZONES];

void vg_canopy_op_use(const VgCanOp* c) {
    s_src = c;
    s_cur = resident(c);
    if (c && !s_hot)
        s_hot = (uint16_t (*)[256])heap_caps_malloc(
                    (size_t)VG_CANOPY_MAX_ZONES * 256 * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    // A DIFFERENT DRAWING IS A DIFFERENT PALETTE, so every table built against the last
    // one is wrong. -1 is a level no glow reaches, so this is "never built".
    for (int z = 0; z < VG_CANOPY_MAX_ZONES; z++) s_hot_q[z] = -1;
}
const VgCanOp* vg_canopy_op_current(void) { return s_cur; }

// t is 0..255 toward white. The arithmetic is canopy_ilut's, against the art's palette
// instead of against a delta table: unpack in NATIVE order, walk each channel its own
// fraction of the distance to full, repack and swap back to panel order.
static void hot_lut(const VgCanOp* c, int z, uint32_t t) {
    for (int g = 0; g < 256; g++) {
        const uint32_t v = c->pal[g];                     // panel order
        const uint32_t n = (v >> 8) | ((v << 8) & 0xFF00u);
        const uint32_t r = (n >> 11) & 31u, gg = (n >> 5) & 63u, b = n & 31u;
        const uint32_t R = r  + (((31u - r)  * t) >> 8);
        const uint32_t G = gg + (((63u - gg) * t) >> 8);
        const uint32_t B = b  + (((31u - b)  * t) >> 8);
        const uint32_t o = (R << 11) | (G << 5) | B;
        s_hot[z][g] = (uint16_t)(((o >> 8) | (o << 8)) & 0xFFFFu);
    }
    s_hot_q[z] = (int16_t)t;
}

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
    bool             live[VG_CANOPY_MAX_ZONES];
    uint16_t         edge[VG_CANOPY_MAX_ZONES];
    uint8_t          heat[VG_CANOPY_MAX_ZONES];
    uint8_t          hotth[VG_CANOPY_MAX_ZONES];   // cells still drawn as light
    uint8_t          revth[VG_CANOPY_MAX_ZONES];   // cells the gate is still holding
    uint16_t         held[VG_CANOPY_MAX_ZONES];    // ...and what one of those comes to
    const uint16_t*  zpal[VG_CANOPY_MAX_ZONES];
    const int nz = (int)c->zones;
    for (int z = 0; z < nz && z < VG_CANOPY_MAX_ZONES; z++) {
        live[z] = vg_canopy_zone_live(z);
        const uint32_t g = vg_canopy_zone_glow(z);
        heat[z] = (uint8_t)g;
        edge[z] = outline_native((float)g * (1.0f / 255.0f));
        // COOL, AND THEN THE PALETTE ITSELF. A region that is not running hot reads the
        // art directly, which is every region for all but the first three seconds of a
        // match -- so the whole of this costs one compare a region a band.
        if (!g || !s_hot) { zpal[z] = pal; continue; }   // no table is a cool region
        if (s_hot_q[z] != (int16_t)g) hot_lut(c, z, g);
        zpal[z] = s_hot[z];

        // THE PIXEL UNDER A HOT ONE IS OFTEN ALREADY KNOWN, and that is worth more
        // than any arithmetic. The measured claim above SPAN_BODY in vg_canopy_draw.cpp
        // is that a blended pixel costs its READ-MODIFY-WRITE and not its maths --
        // eight cycles taken out of the blend there changed nothing at all -- and that
        // the only lever left is to touch fewer pixels.
        //
        // The gate runs first, over this same row, and where it is still HOLDING a
        // dither cell it has just written one flat colour there. So a hot pixel over a
        // held cell adds a known value to a known value: the answer is a constant for
        // the whole region and it can be STORED, with no read at all. Only the cells
        // the world has come through need the real blend.
        //
        // The two thresholds come out of the same 4x4 cell, so the test is exact
        // rather than approximate: rev 0 makes every cell held and rev 255 makes none,
        // which is precisely when the gate stops touching the region.
        //
        // Worth most exactly where the spike is. A region flashes at full heat with
        // its reveal still at zero -- every pixel light, every cell held -- so the
        // frame that used to be 45,591 read-modify-writes is now that many stores.
        hotth[z] = (uint8_t)(((255u - g) * 17u) >> 8);
        const uint32_t rv = vg_canopy_zone_reveal(z);
        revth[z] = (uint8_t)((rv * 17u) >> 8);
        // Through add_px itself, so the stored value cannot drift from the blended one.
        uint16_t k = vg_canopy_zone_fill(z);
        add_px(&k, edge[z]);
        held[z] = k;
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

        // ONE PROJECTION PER ENDPOINT, NOT TWO PER SPAN. The runs on a row are in
        // order and the outline traces the metal, so most runs begin where the last
        // one ended; the sphere is a function of x alone on a row, so that endpoint
        // has already been projected. Keyed by the drawing's x, and reset per row.
        // 9,724 float projections a frame become about half that, bit for bit.
        //
        // AND NONE AT ALL WHEN THE SPHERE IS FLAT. With the bend quantised to zero the
        // maps hold zoom 1 and curvature 0 exactly, and sphere_x(x) is then
        // (int)((float)x + 0.5f) == x for every column on the panel -- so a lag-only
        // frame is a translation and its runs keep their length, which is the blit.
        int  share_x = -1, share_d = 0;
        const bool flat = mv && mo.zk == 0.0f && mo.zbase == 1.0f;

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
                if (flat) {
                    d0 += mo.xofs;
                    d1 += mo.xofs;
                } else {
                    const int x1 = d1;                          // the drawing's x, still
                    d0 = (d0 == share_x) ? share_d
                                         : sphere_x(d0, mo.zbase, mo.zk) + mo.xofs;
                    d1 = sphere_x(x1, mo.zbase, mo.zk) + mo.xofs;
                    share_x = x1;
                    share_d = d1;
                }
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

            const uint8_t*  src  = &data[sp->off];
            const uint16_t* spal = zpal[sp->zone];
            const uint32_t  hz   = heat[sp->zone];

            // METAL IS LIGHT WHILE IT IS HOT, and that is where the colour went.
            //
            // The light-delta cockpit's arrival is not one effect, it is a
            // RELATIONSHIP: its members are additive, and behind them the region is
            // dissolving out of white into the world. So the frame clips to white over
            // the flash, burns amber over the first cells of world that come through,
            // and settles as the background stops moving -- a wide colour excursion that
            // nothing in the drawing contains. Painted metal replaces the pixel, so it
            // gets none of that: it went white and then it was simply there.
            //
            // So while a region is running hot its metal is ADDED rather than stored,
            // in the same lit amber the outline takes, and pixels cross to paint one
            // dither cell at a time as it cools. A dither and not a fade, for the reason
            // BAYER4 gives where it is declared: it is a store or an add either way, no
            // blend, and it is the language the reveal already speaks.
            //
            // Costs a branch a pixel over a region's metal for the two or three seconds
            // it is arriving, and nothing whatever after that -- heat is zero for every
            // region for the rest of the match.
            if (hz) {
                const uint8_t* bay = vg_canopy_bayer_row(y);
                // 0 while white-hot, so every cell is still light; 16 once cool, which
                // is past every cell, so every cell has become paint.
                const uint32_t th   = hotth[sp->zone];
                // ...and the same cell against the gate's own reveal. A cell at or above
                // this one is still holding the fill, so its blended value is the
                // constant worked out in the prologue and needs no read. Only usable
                // when the gate actually ran this row: nothing else puts that colour
                // there. 16 is past every cell, which turns the whole test off.
                const uint32_t thr  = gate ? (uint32_t)revth[sp->zone] : 16u;
                const uint16_t k    = held[sp->zone];
                const uint16_t litz = edge[sp->zone];
                // 32-bit throughout -- see the note at the stretched path below.
                const int32_t  step = (int32_t)(len << 16) / (d1 - d0);
                const int32_t  top  = (int32_t)(len - 1) << 16;
                int32_t acc = (int32_t)((uint32_t)(c0 - d0) * (uint32_t)step);
                for (int x = c0; x < c1; x++) {
                    const uint32_t bc = bay[x & 3];
                    if (bc >= th) {
                        if (bc >= thr) *dst++ = k;          // known: no read
                        else           add_px(dst++, litz); // the world is behind it
                    } else {
                        const int32_t a = (acc < 0) ? 0 : (acc > top ? top : acc);
                        *dst++ = spal[src[a >> 16]];
                    }
                    acc += step;
                }
                continue;
            }

            // STRETCHED, which the sphere does and a translation does not. The run
            // holds a palette index a pixel, so a run that lands longer or shorter
            // than it was drawn has to be RESAMPLED -- nearest neighbour, on a 16.16
            // step, clamped at both ends so the border extension repeats the edge
            // pixel instead of reading off the run.
            //
            // AND THIS IS THE FLIGHT PATH, not an edge case, which is what makes it
            // worth more than the straight loop it started as. The bend is driven by
            // (1 - throttle), so the tube is at its most bent when the ship is slow
            // and the sphere is stretching almost every span almost all the time. The
            // first version cost the cockpit 55 us to 102 on the desktop bench, and
            // all of it went on two things this now avoids.
            //
            // THE CLAMP IS A RANGE, NOT A TEST. The accumulator only leaves the run at
            // the ENDS: below zero solely through the left border extension, and above
            // the last index for the handful of trailing pixels a stretch invents. So
            // the count that stays inside is computed once and the inner loop carries
            // no compares at all; the tail is a constant, because every clamped index
            // is the same index.
            //
            // AND THE STORES PAIR, exactly as they do in the unstretched blit below.
            // The panel is 16-bit and the bus is 32, so two pixels are one word -- the
            // resample was giving that back a pixel at a time for no reason but the
            // shape of the loop.
            if (ext || (d1 - d0) != len) {
                // 32-BIT THROUGHOUT, and every value is the same one the 64-bit form gave.
                // A 64-bit divide on this core is a library call of a few hundred cycles,
                // and there were two of them and a 64-bit multiply on every stretched
                // run -- about 4,800 a frame at full bend. By range they never needed
                // it: len is at most the panel's width, so len << 16 is under 2^25 and
                // divides in 32; top - acc is inside [0, top]; and the product was
                // always truncated to 32 bits on assignment, which is what the unsigned
                // multiply gives, wrap for wrap, without the undefined signed overflow.
                const int32_t step = (int32_t)(len << 16) / (d1 - d0);
                const int32_t top  = (int32_t)(len - 1) << 16;
                int32_t acc = (int32_t)((uint32_t)(c0 - d0) * (uint32_t)step);

                // An odd first pixel, to reach alignment. Clamped, because it is one
                // pixel and the range arithmetic below is not worth doing twice: an
                // unaligned 32-bit store on Xtensa is a fault, not a slowdown.
                if ((((uintptr_t)dst) & 2u) && n > 0) {
                    const int32_t a = (acc < 0) ? 0 : (acc > top ? top : acc);
                    *dst++ = spal[src[a >> 16]];
                    acc += step;
                    n--;
                }

                // HOW MANY STAY INSIDE THE RUN. Zero if the accumulator starts below
                // it, which only the left border extension can do -- that case keeps
                // the old shape and pays the compares, and it is one span in a frame.
                int mid = 0;
                if (acc >= 0 && acc <= top) {
                    const int32_t room = top - acc;
                    const int32_t k    = room / step + 1;
                    mid = (k < n) ? k : n;
                }

                uint32_t* w = (uint32_t*)dst;
                int       m = mid;
                while (m >= 2) {
                    const uint16_t a0 = spal[src[acc >> 16]]; acc += step;
                    const uint16_t a1 = spal[src[acc >> 16]]; acc += step;
                    *w++ = (uint32_t)a0 | ((uint32_t)a1 << 16);
                    m -= 2;
                }
                dst = (uint16_t*)w;
                if (m) { *dst++ = spal[src[acc >> 16]]; acc += step; m--; }
                n -= mid;

                // THE TAIL IS ONE COLOUR. Past the end of the run every index clamps
                // to the last one, so the pixels a stretch invents beyond it are a
                // fill rather than a resample.
                if (n > 0) {
                    if (acc < 0) {
                        // The rare shape: still short of the run. Clamped, as before.
                        while (n-- > 0) {
                            const int32_t a = (acc < 0) ? 0 : (acc > top ? top : acc);
                            *dst++ = spal[src[a >> 16]];
                            acc += step;
                        }
                    } else {
                        const uint16_t v = spal[src[len - 1]];
                        if ((((uintptr_t)dst) & 2u) && n > 0) { *dst++ = v; n--; }
                        uint32_t* t = (uint32_t*)dst;
                        const uint32_t vv = (uint32_t)v | ((uint32_t)v << 16);
                        while (n >= 2) { *t++ = vv; n -= 2; }
                        dst = (uint16_t*)t;
                        if (n > 0) *dst++ = v;
                    }
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
                *dst++ = spal[*src++];
                n--;
            }
            uint32_t* w = (uint32_t*)dst;
            while (n >= 2) {
                const uint16_t a = spal[src[0]];
                const uint16_t b = spal[src[1]];
                *w++ = (uint32_t)a | ((uint32_t)b << 16);
                src += 2;
                n   -= 2;
            }
            if (n > 0) *(uint16_t*)w = spal[*src];
        }
    }
}
