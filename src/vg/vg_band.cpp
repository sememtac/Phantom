#include "vg_raster.h"
#include "vg_crumb.h"
#include "vg_raster_int.h"
#include "vg_font.h"
#include "vg_port.h"
#include "vg_capture.h"
#include "vg_sky.h"
#include "vg_canopy_data.h"
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <string.h>
#include <math.h>

// The raster half. Everything here runs NUM_BANDS times per frame but is hidden
// under the panel DMA, so work moved into this file is close to free -- right up
// until the per-band total exceeds the 0.768 ms DMA window, at which point it
// starts costing frame time directly. That threshold is the whole reason the
// backdrop fill and the scanline pass are written the way they are.

// Two band buffers, alternating: one is drawn into while the other is on the wire.
static uint16_t* s_band[2] = { nullptr, nullptr };

bool vg_band_init(void) {
    // Must be internal and DMA-capable: written pixel-by-pixel, then handed
    // straight to the SPI engine.
    s_band[0] = (uint16_t*)heap_caps_malloc(SCR_W * BAND_H * 2,
                                            MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    s_band[1] = (uint16_t*)heap_caps_malloc(SCR_W * BAND_H * 2,
                                            MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (!s_band[0] || !s_band[1]) {
        Serial.printf("vg_band_init: alloc failed (b0=%p b1=%p)\n", s_band[0], s_band[1]);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Primitives
// ---------------------------------------------------------------------------

// ROUND TO NEAREST, IN ONE INSTRUCTION.
//
// `lrintf` is not an instruction here. It links to newlib's sf_lrint.c -- 138 bytes of
// soft-float bit manipulation, reached by a call -- and the line dispatch used four of them
// per line per band. The FPU has ROUND.S, which converts single to signed integer rounding to
// nearest with ties to even, which is precisely what lrintf does in the default rounding
// mode. So this is the same answer, in one instruction instead of a function call.
//
// Safe because the inputs are clipped screen coordinates: finite, and inside a few hundred.
// ROUND.S on an out-of-range or NaN input saturates rather than raising, where lrintf's
// result would be undefined anyway.
static inline int fast_lrintf(float f) {
    int r;
    asm ("round.s %0, %1, 0" : "=a"(r) : "f"(f));
    return r;
}

// Clip a line to a y-range. x is already inside the screen, so only y needs
// trimming; a parametric clip on y alone is cheaper and exact.
static inline bool clip_band_y(float* ax, float* ay, float* bx, float* by,
                               float ymin, float ymax) {
    float y0 = *ay, y1 = *by;
    if (y0 == y1) return (y0 >= ymin && y0 <= ymax);

    const float dy = y1 - y0;

    // ONLY THE END THAT IS ACTUALLY OUTSIDE NEEDS ITS PARAMETER.
    //
    // Both were computed every time, and each is a float divide -- which the S3 has no single
    // instruction for. This runs once per line per band and a line is 19 pixels, so most of
    // them sit inside their band entirely and needed neither.
    //
    // The conditions are the clamps rewritten, not new logic. For dy > 0, t0 is ta only when
    // ta > 0, which is exactly y0 < ymin; and t1 is tb only when tb < 1, which rearranges to
    // y1 > ymax. For dy < 0 the two swap, so the tests read the other way and each takes the
    // other numerator. Same expressions, evaluated only where they can change the answer, so
    // the result is identical to the bit.
    float t0 = 0.0f, t1 = 1.0f;
    if (dy > 0.0f) {
        if (y0 < ymin) t0 = (ymin - y0) / dy;
        if (y1 > ymax) t1 = (ymax - y0) / dy;
    } else {
        if (y0 > ymax) t0 = (ymax - y0) / dy;
        if (y1 < ymin) t1 = (ymin - y0) / dy;
    }
    if (t0 > t1) return false;
    // Nothing was trimmed, so the endpoints are already the answer. The old form still spent
    // four multiplies and four adds arriving back at them.
    if (t0 == 0.0f && t1 == 1.0f) return true;

    const float x0 = *ax, dx = *bx - x0;
    *ax = x0 + dx * t0;  *ay = y0 + dy * t0;
    *bx = x0 + dx * t1;  *by = y0 + dy * t1;
    return true;
}

// A POINTER AND A STEP COUNT, rather than coordinates and a destination test.
//
// The address was rebuilt for every pixel -- a subtract, a multiply by 480 and an add -- when
// each step moves it by exactly sx or by sy rows. And x and y were tracked solely to ask
// "am I there yet", which Bresenham already knows: it visits max(dx, |dy|) + 1 pixels and no
// other number, so a counter answers it in one compare instead of two.
//
// Same pixels in the same order with the same colour. The endpoints are clamped to the band
// by the caller, so the walk cannot leave it and the pointer cannot leave the buffer.
// How much line there actually IS, per frame. Two adds per LINE, not per pixel.
//
// Without this, `ln` cannot be divided into the per-line setup -- clipping, rounding,
// dispatch -- and the per-pixel walk, and a bench cannot know what line length to use. The
// first line bench averaged 176 pixels a line, which is nothing like a hull edge or a trail.
static uint32_t s_ln_px = 0, s_ln_n = 0;

static inline void band_line(uint16_t* band, int band_y0,
                             int x0, int y0, int x1, int y1, uint16_t color) {
    const int dx =  abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    const int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;

    uint16_t* p = &band[(y0 - band_y0) * SCR_W + x0];
    const int ystep = sy * SCR_W;
    s_ln_px += (uint32_t)((dx > -dy ? dx : -dy) + 1);
    s_ln_n++;

    // AND THE MAJOR AXIS STEPS EVERY TIME, so its test is not a test.
    //
    // Three conditional branches a pixel, on a core with no branch predictor, was most of
    // what was left. For a line at least as wide as it is tall the x step fires on every
    // iteration without exception, so its compare can go -- and the same the other way for a
    // steep line. Both `e2` tests read the error from BEFORE either update, which is why the
    // unconditional half can be applied first without disturbing the second.
    //
    // Two loops instead of one, each shorter. If the "always" is ever not always, the bench
    // says DIFFERENT rather than the frame quietly changing.
    if (dx >= -dy) {
        for (int n = dx; ; ) {
            *p = color;
            if (--n < 0) break;
            const int e2 = err << 1;
            err += dy; p += sx;
            if (e2 <= dx) { err += dx; p += ystep; }
        }
    } else {
        for (int n = -dy; ; ) {
            *p = color;
            if (--n < 0) break;
            const int e2 = err << 1;
            err += dx; p += ystep;
            if (e2 >= dy) { err += dy; p += sx; }
        }
    }
}

#if VG_LINE_AA
// Alpha blend in NATIVE RGB565. R+B and G are blended separately so all three
// channels ride two multiplies, with no field able to borrow into the next.
// `a` is coverage in 1/32 units.
static inline uint16_t blend565(uint16_t s, uint16_t d, uint32_t a) {
    uint32_t ia = 32u - a;
    uint32_t rb = ((((uint32_t)s & 0xF81Fu) * a) + (((uint32_t)d & 0xF81Fu) * ia)) >> 5;
    uint32_t g  = ((((uint32_t)s & 0x07E0u) * a) + (((uint32_t)d & 0x07E0u) * ia)) >> 5;
    return (uint16_t)((rb & 0xF81Fu) | (g & 0x07E0u));
}

static inline void plot_aa(uint16_t* band, int by0, int by1,
                           int x, int y, uint16_t src_native, uint32_t a) {
    if (a == 0) return;
    if (y < by0 || y > by1) return;
    if ((unsigned)x >= (unsigned)SCR_W) return;

    uint16_t* p = &band[(y - by0) * SCR_W + x];
    uint16_t  d = (uint16_t)((*p >> 8) | (*p << 8));    // panel order -> native
    uint16_t  o = blend565(src_native, d, a);
    *p = (uint16_t)((o >> 8) | (o << 8));               // and back
}

// Wu's algorithm. The source colour is converted out of panel byte order ONCE
// per line rather than per pixel, which is most of what makes this affordable.
static void band_line_aa(uint16_t* band, int by0, int by1,
                         int x0, int y0, int x1, int y1, uint16_t color) {
    const uint16_t src = (uint16_t)((color >> 8) | (color << 8));

    // Work along the major axis so the gradient is always <= 1 and each step
    // straddles exactly two pixels.
    const bool steep = abs(y1 - y0) > abs(x1 - x0);
    if (steep)   { int t; t=x0; x0=y0; y0=t; t=x1; x1=y1; y1=t; }
    if (x0 > x1) { int t; t=x0; x0=x1; x1=t; t=y0; y0=y1; y1=t; }

    const int dx = x1 - x0;

    // 16.16 fixed point rather than a float accumulator. The old version paid a
    // float add, a float-to-int conversion and a float multiply per pixel; this
    // is an integer add and two shifts, and the coverage falls straight out of
    // the fraction bits with no rounding step at all.
    const int32_t grad  = (dx == 0) ? 0
                        : (int32_t)(((int32_t)(y1 - y0) << 16) / dx);
    int32_t       inter = (int32_t)y0 << 16;

    for (int x = x0; x <= x1; x++) {
        const int      iy = inter >> 16;
        const uint32_t f  = ((uint32_t)inter >> 11) & 31u;

        if (steep) {
            plot_aa(band, by0, by1, iy,     x, src, 32u - f);
            plot_aa(band, by0, by1, iy + 1, x, src, f);
        } else {
            plot_aa(band, by0, by1, x, iy,     src, 32u - f);
            plot_aa(band, by0, by1, x, iy + 1, src, f);
        }
        inter += grad;
    }
}
#endif // VG_LINE_AA

// ADDITIVE AND SUBTRACTIVE PIXELS, which is what lets the canopy be LIT rather than
// drawn on.
//
// An opaque line states a colour; these state a CHANGE to whatever is behind. The
// difference matters for structure: a frame drawn opaque is a decal that sits at one
// brightness whatever it crosses, while a frame that adds on its lit side and
// subtracts on its shaded side reads as a rib catching light -- and it keeps reading
// that way over a dark nebula and over a bright one, because it is a relationship
// instead of a value.
//
// Saturating per channel in NATIVE order, so the two byte swaps are the same pair the
// antialiased path already pays. Costs about what a blend costs: an order of magnitude
// more than a store, which is affordable for a frame of a few dozen lines and would
// not be for the world.
// The blend itself, on a pixel already known to be inside the band. Split out so the
// span fill below can hoist every bound check out of its inner loop -- which is the
// whole reason a broad member is drawn as a fill and not as a bundle of lines.
static inline void blend_px(uint16_t* p, uint16_t d_native, bool add) {
    const uint16_t s = (uint16_t)((*p >> 8) | (*p << 8));      // panel order -> native

    uint32_t r = (s >> 11) & 31u, g = (s >> 5) & 63u, b = s & 31u;
    const uint32_t dr = (d_native >> 11) & 31u,
                   dg = (d_native >> 5) & 63u,
                   db =  d_native        & 31u;
    if (add) {
        r += dr; if (r > 31u) r = 31u;
        g += dg; if (g > 63u) g = 63u;
        b += db; if (b > 31u) b = 31u;
    } else {
        r = (r > dr) ? r - dr : 0u;
        g = (g > dg) ? g - dg : 0u;
        b = (b > db) ? b - db : 0u;
    }
    const uint16_t o = (uint16_t)((r << 11) | (g << 5) | b);
    *p = (uint16_t)((o >> 8) | (o << 8));                      // and back
}

static inline void plot_delta(uint16_t* band, int by0, int by1,
                              int x, int y, uint16_t d_native, bool add) {
    if (y < by0 || y > by1) return;
    if ((unsigned)x >= (unsigned)SCR_W) return;

    uint16_t* p = &band[(y - by0) * SCR_W + x];
    const uint16_t s = (uint16_t)((*p >> 8) | (*p << 8));      // panel order -> native

    uint32_t r = (s >> 11) & 31u, g = (s >> 5) & 63u, b = s & 31u;
    const uint32_t dr = (d_native >> 11) & 31u,
                   dg = (d_native >> 5) & 63u,
                   db =  d_native        & 31u;
    if (add) {
        r += dr; if (r > 31u) r = 31u;
        g += dg; if (g > 63u) g = 63u;
        b += db; if (b > 31u) b = 31u;
    } else {
        r = (r > dr) ? r - dr : 0u;
        g = (g > dg) ? g - dg : 0u;
        b = (b > db) ? b - db : 0u;
    }
    const uint16_t o = (uint16_t)((r << 11) | (g << 5) | b);
    *p = (uint16_t)((o >> 8) | (o << 8));                      // and back
}

static void band_line_delta(uint16_t* band, int by0, int by1,
                            int x0, int y0, int x1, int y1, uint16_t colour, bool add) {
    // The delta is carried as an ordinary colour and converted once per line, the same
    // trick that makes the antialiased path affordable.
    const uint16_t d = (uint16_t)((colour >> 8) | (colour << 8));

    int dx =  abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    int x = x0, y = y0;
    for (;;) {
        plot_delta(band, by0, by1, x, y, d, add);
        if (x == x1 && y == y1) break;
        const int e2 = err << 1;
        if (e2 >= dy) { err += dy; x += sx; }
        if (e2 <= dx) { err += dx; y += sy; }
    }
}

// THE OPAQUE PATH, and it was carrying a guard that could not fire.
//
// This is what draws trails, hulls and arena structure -- `ln` on the telemetry line, and one
// of the largest items in a course run. It tested every pixel against the band and the screen
// width, three compares on top of an address rebuilt from scratch, and the comment above it
// said it could not assume x was in range.
//
// That stopped being true. The dispatch in draw_band clamps BOTH endpoints into the band and
// into [0, SCR_W) before it picks a mode -- unconditionally, for every line -- and Bresenham
// never leaves its endpoints' bounding box. So the guard was protecting against a caller that
// no longer exists.
//
// Clamped once here rather than not at all, because "the caller does it" is a claim that
// outlives the caller who did. Eight compares a LINE against three a PIXEL, then the walk in
// band_line does the rest.
static inline void band_line_fast(uint16_t* band, int by0, int by1,
                                  int x0, int y0, int x1, int y1, uint16_t color) {
    if (y0 < by0) y0 = by0; else if (y0 > by1) y0 = by1;
    if (y1 < by0) y1 = by0; else if (y1 > by1) y1 = by1;
    if (x0 < 0) x0 = 0; else if (x0 > SCR_W - 1) x0 = SCR_W - 1;
    if (x1 < 0) x1 = 0; else if (x1 > SCR_W - 1) x1 = SCR_W - 1;
    band_line(band, by0, x0, y0, x1, y1, color);
}

// AS IT WAS, for the bench to prove the above against.
static void band_line_fast_ref(uint16_t* band, int by0, int by1,
                               int x0, int y0, int x1, int y1, uint16_t color) {
    int dx =  abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;

    int x = x0, y = y0;
    for (;;) {
        if (y >= by0 && y <= by1 && (unsigned)x < (unsigned)SCR_W)
            band[(y - by0) * SCR_W + x] = color;
        if (x == x1 && y == y1) break;
        int e2 = err << 1;
        if (e2 >= dy) { err += dy; x += sx; }
        if (e2 <= dx) { err += dx; y += sy; }
    }
}

// Scanline-fill a triangle, clipped to the band's rows. Spans are clamped
// horizontally rather than the vertices being clipped, which keeps the edge
// interpolation exact for geometry running off the side of the screen.
// `mode` is LINE_* -- ADD and SUB make the fill a DELTA over what is already there,
// which is how a broad canopy member gets drawn. A member is a fill and not a bundle
// of parallel lines for one reason: the span loop below pays its bound checks once a
// row instead of once a pixel, and at 15,000 blended pixels a frame that difference is
// the whole affordability of the thing.
static void band_tri(uint16_t* band, int by0, int by1,
                     int x0, int y0, int x1, int y1, int x2, int y2,
                     uint16_t color, uint8_t mode) {
    const bool     blend = (mode == LINE_ADD || mode == LINE_SUB);
    const bool     add   = (mode == LINE_ADD);
    const uint16_t dn    = (uint16_t)((color >> 8) | (color << 8));
    int t;
    if (y1 < y0) { t=x0; x0=x1; x1=t; t=y0; y0=y1; y1=t; }
    if (y2 < y0) { t=x0; x0=x2; x2=t; t=y0; y0=y2; y2=t; }
    if (y2 < y1) { t=x1; x1=x2; x2=t; t=y1; y1=y2; y2=t; }
    if (y2 == y0) return;                       // zero height

    int ys = y0 > by0 ? y0 : by0;
    int ye = y2 < by1 ? y2 : by1;
    if (ys > ye) return;

    const float inv02 = 1.0f / (float)(y2 - y0);
    const float inv01 = (y1 != y0) ? 1.0f / (float)(y1 - y0) : 0.0f;
    const float inv12 = (y2 != y1) ? 1.0f / (float)(y2 - y1) : 0.0f;

    for (int y = ys; y <= ye; y++) {
        // Long edge y0->y2, plus whichever short edge this scanline crosses.
        float xa = (float)x0 + (float)(x2 - x0) * ((float)(y - y0) * inv02);
        float xb;
        if (y < y1) xb = (inv01 != 0.0f)
                       ? (float)x0 + (float)(x1 - x0) * ((float)(y - y0) * inv01)
                       : (float)x1;
        else        xb = (inv12 != 0.0f)
                       ? (float)x1 + (float)(x2 - x1) * ((float)(y - y1) * inv12)
                       : (float)x1;

        int xl = (int)(xa < xb ? xa : xb);
        int xr = (int)(xa < xb ? xb : xa);
        if (xl < 0) xl = 0;
        if (xr > SCR_W - 1) xr = SCR_W - 1;
        if (xl > xr) continue;

        uint16_t* row = &band[(y - by0) * SCR_W];
        if (blend) { for (int x = xl; x <= xr; x++) blend_px(&row[x], dn, add); }
        else       { for (int x = xl; x <= xr; x++) row[x] = color; }
    }
}

static inline void band_glyph(uint16_t* band, int by0, int by1, const Prim* p) {
    const int      scale = p->x1;
    const uint8_t* glyph = VG_FONT5X7[p->y1 - VG_FONT_FIRST];

    // The font bitmap is authored in the LOGICAL frame, so each set pixel's
    // offset is mapped into panel space here. Without this the text would stay
    // aligned to the panel while the rest of the world turned.
#if VG_ROTATE == 1
    // THE BAND TEST BELONGS OUTSIDE, and under this rotation it can be.
    //
    // A glyph's panel row is p->y0 - dx, and dx comes from the COLUMN loops alone -- so
    // every pixel of a column lands on one panel row. The test was in the innermost loop
    // being asked 7*scale times per column for an answer that cannot change, and a glyph is
    // walked in full once for every band it touches, so most of those asks were rejections.
    //
    // Hoisted, a column costs one test and one row pointer, and the inner loop is a walk
    // along panel x with a bounds check and a store. The pixels written are the same pixels
    // in the same colour -- only the order changed, and they are plain stores.
    const int x0 = p->x0, y0 = p->y0;
    const uint16_t colour = p->color;
    for (int col = 0; col < 5; col++) {
        const uint8_t bits = glyph[col];
        if (!bits) continue;
        for (int jx = 0; jx < scale; jx++) {
            const int yy = y0 - (col * scale + jx);
            if (yy < by0 || yy > by1) continue;
            uint16_t* prow = &band[(yy - by0) * SCR_W];
            for (int row = 0; row < 7; row++) {
                if (!(bits & (1 << row))) continue;
                const int xb = x0 + row * scale;
                for (int jy = 0; jy < scale; jy++) {
                    const int xx = xb + jy;
                    if ((unsigned)xx >= (unsigned)SCR_W) continue;
                    prow[xx] = colour;
                }
            }
        }
    }
#else
    // The other rotations put the panel row on the dy axis instead, so the hoist would want
    // the loops the other way round. They are not built, so they keep the plain form.
    for (int col = 0; col < 5; col++) {
        uint8_t bits = glyph[col];
        if (!bits) continue;
        for (int row = 0; row < 7; row++) {
            if (!(bits & (1 << row))) continue;
            for (int jy = 0; jy < scale; jy++) {
                for (int jx = 0; jx < scale; jx++) {
                    const int dx = col * scale + jx;
                    const int dy = row * scale + jy;
#if VG_ROTATE == 2
                    const int xx = p->x0 - dx, yy = p->y0 - dy;
#elif VG_ROTATE == 3
                    const int xx = p->x0 - dy, yy = p->y0 + dx;
#else
                    const int xx = p->x0 + dx, yy = p->y0 + dy;
#endif
                    if (yy < by0 || yy > by1) continue;
                    if ((unsigned)xx >= (unsigned)SCR_W) continue;
                    band[(yy - by0) * SCR_W + xx] = p->color;
                }
            }
        }
    }
#endif
}

// ---------------------------------------------------------------------------
// CRT scanlines
// ---------------------------------------------------------------------------

// Halve every channel of TWO packed pixels at once.
//
// The obvious version -- unpack each channel, multiply by a scale, repack, with
// a byte swap either side -- runs about 18 operations per pixel. That was fine
// while the screen was 95% black and nearly every pixel could be skipped, but
// once a backdrop lit every pixel it cost ~4 ms a frame and broke the DMA window.
// Halving is exactly a shift and a mask, and both pixels of a 32-bit word ride
// through together, bringing it to ~5 ops per pixel.
//
// The green channel straddles the byte boundary, so the swap cannot be avoided
// -- but it can at least be done for two pixels in one go.
static inline uint32_t scanline_pair(uint32_t v) {
    uint32_t n = ((v >> 8) & 0x00FF00FFu) | ((v << 8) & 0xFF00FF00u);
    n = (n >> 1) & 0x7BEF7BEFu;
    return ((n >> 8) & 0x00FF00FFu) | ((n << 8) & 0xFF00FF00u);
}

// ---------------------------------------------------------------------------
// Wall proximity tint
// ---------------------------------------------------------------------------

// Redden a pixel with ONE mask and ONE or, and no byte swap.
//
// The first version swapped each pixel to native order, scaled green and blue by
// a shift, added the red, and swapped back. Correct, and it cost 17ms a frame at
// full coverage: 66 fps fell to 35, at the exact moment the player most needs the
// frame rate. Two swaps and eight masks per pixel pair, over 230,400 pixels.
//
// So the work is done in the panel's own byte order instead. The pixels are stored
// byte-swapped, which puts the fields at:
//
//     bits 15..13  green, high 3      bits 7..3   red
//     bits 12..8   blue               bits 2..0   green, low 3
//
// Blue is contiguous there and green's top bit is reachable, so "take blue out and
// cap green" is a mask, and "add red" is an or into bits 7..3. Green cannot be
// SCALED without recombining its two halves, and it does not need to be: for a
// warning tint, removing blue and capping green is what turns the picture red.
//
// The masks take green down in stages, and they have to reach far enough to catch
// AMBER. The HUD is #ffae1e, whose green is 21 of 63 -- binary 010101, with its
// top bit already clear. An earlier ramp only cleared green's top bit, so it did
// nothing at all to amber and the instruments sat there untouched inside a red
// cockpit. Clearing G4 is what actually reddens them, and by the rim green is
// gone entirely.
// Twelve rings, not four. Four was a gradient in the sense that it had steps in
// it, and the steps were the thing you noticed. Each extra ring costs one more
// square root per ROW and two more spans, which is nothing next to the pixels.
#define TINT_RINGS 12

// Per ring, from the faint inner edge out to the rim. The mask CLEARS bits and
// the glow is a red value out of 31, pre-shifted into the swapped red field.
//
// The red ramp starts at zero on purpose: the innermost ring changes nothing at
// all, so the gradient fades out instead of ending on an edge.
// Pre-paired, so the hot loop does not build them. Each entry is the mask and the
// glow for TWO pixels at once. Recomputing these inside the span function cost
// six operations per call, and there are twelve rings on both sides of 480 rows.
static const uint32_t TINT_KEEP[TINT_RINGS] = {
    0x1FFF1FFFu, 0x1EFF1EFFu, 0x1CFF1CFFu, 0x18FF18FFu,
    0x10FF10FFu, 0x00FE00FEu, 0x00FF00FFu, 0x00FE00FEu,
    0x00FE00FEu, 0x00FA00FAu, 0x00F800F8u, 0x00F800F8u,
};
static const uint32_t TINT_GLOW[TINT_RINGS] = {
    0x00000000u, 0x00080008u, 0x00100010u, 0x00180018u,
    0x00200020u, 0x00300030u, 0x00400040u, 0x00500050u,
    0x00600060u, 0x00700070u, 0x00800080u, 0x00900090u,
};
// Shift applied to green's HIGH three bits, which are contiguous at 15..13 in
// the swapped word. This is what gives amber intermediate levels: masks alone
// could only take its green from 21 straight to 0, because 010101 has no bits
// left to remove in between.
static const uint8_t TINT_GSHIFT[TINT_RINGS] = {
    0, 0, 0, 1, 1, 1,
    2, 2, 3, 3, 3, 3,
};

// Tint [x0,x1) of one row. Unrolled four words at a time, which is what the
// scanline pass above learned: at five operations of real work per word, the loop
// itself was most of the cost.
static inline void tint_span(uint16_t* row, int x0, int x1, int ring) {
    if (x0 < 0)     x0 = 0;
    if (x1 > SCR_W) x1 = SCR_W;
    if (x1 <= x0)   return;

    const uint32_t keep = TINT_KEEP[ring];
    const uint32_t glow = TINT_GLOW[ring];
    const int       gs  = TINT_GSHIFT[ring];
    if (glow == 0u && gs == 0 && keep == 0xE000E000u) { /* nothing to do */ }
    // Green's high field is rebuilt rather than kept, so it is cleared in `keep`
    // and re-inserted shifted. GH is the mask for that field, in both pixels.
    const uint32_t GH = 0xE000E000u;

    int x = x0;
    if (x & 1) {
        const uint16_t v = row[x];
        row[x] = (uint16_t)((v & (uint16_t)keep)
                          | (((v & (uint16_t)GH) >> gs) & (uint16_t)GH)
                          | (uint16_t)glow);
        x++;
    }

    uint32_t* p = (uint32_t*)(row + x);
    int n = (x1 - x) >> 1;
    int i = 0;
    for (; i + 4 <= n; i += 4) {
        uint32_t a = p[i], b2 = p[i + 1], c = p[i + 2], d = p[i + 3];
        p[i]     = (a  & keep) | (((a  & GH) >> gs) & GH) | glow;
        p[i + 1] = (b2 & keep) | (((b2 & GH) >> gs) & GH) | glow;
        p[i + 2] = (c  & keep) | (((c  & GH) >> gs) & GH) | glow;
        p[i + 3] = (d  & keep) | (((d  & GH) >> gs) & GH) | glow;
    }
    for (; i < n; i++) {
        const uint32_t v = p[i];
        p[i] = (v & keep) | (((v & GH) >> gs) & GH) | glow;
    }
    x += n * 2;

    for (; x < x1; x++) {
        const uint16_t v = row[x];
        row[x] = (uint16_t)((v & (uint16_t)keep)
                          | (((v & (uint16_t)GH) >> gs) & (uint16_t)GH)
                          | (uint16_t)glow);
    }
}

// A RADIAL gradient that closes inward. The edge of the screen reddens first and
// the red front travels toward the middle, so at the moment of real danger the
// whole picture is red and the strongest part is still the rim.
//
// This replaced a flat tint over the whole frame, which said "danger" but not
// "how close", and before that a subdivided patch of wall, which was a detail on
// a surface the player was not looking at.
//
// No square root per pixel. For a row at dy from centre, the ring of radius R
// crosses it at dx = sqrt(R*R - dy*dy), so one root per ring per ROW gives the
// boundaries and everything between them is a span. Untinted spans cost nothing.
//
// TINT_RINGS is defined once, above the tables. It was defined a second time
// here, at 4, left behind by an edit -- so the tables held twelve entries and the
// loop only ever read the first four, which are the faintest. The gradient was
// running over the whole screen, writing 153,600 pixels a frame, and doing
// almost nothing visible with them. The compiler warned about the redefinition
// and the warning was filtered out of the build output.


// Set from outside, once per frame: 0 for no tint, up to 1 at the wall. The
// rasteriser does not include vg_game.h and must not -- it draws what it is given
// and knows nothing about walls. The render layer owns the game state.
static float s_tint_k = 0.0f;

// --- tint, at the source ----------------------------------------------------
//
// The full-frame tint pass died of the lit sky. Its pixel op is three masks a
// word, but it walked every bright pixel of every row -- and once the sphere
// backdrop lit the whole frame, "every bright pixel" became the whole frame:
// five milliseconds a frame parked against a wall, measured. So the tint is
// applied where the colour is already in hand instead: the sky fill tints its
// 8-pixel chunks as it writes them, and primitives are tinted once each at
// submit. A 60-chunk row costs sixty ops where the pass paid two hundred and
// forty; a primitive costs one.
bool vg_tint_active(void) { return s_tint_k > 0.0f; }

// Ring crossings for one panel row: lim[i] is the half-width where ring i
// begins. Geometry identical to the dead pass.
void vg_tint_row_limits(int sy, int* lim) {
    const float cx = (float)(SCR_W / 2), cy = (float)(SCR_H / 2);
    const float rmax = sqrtf(cx * cx + cy * cy);
    const float rin  = rmax * (1.0f - s_tint_k);
    const float step = (rmax - rin) / (float)TINT_RINGS;
    const float dy   = (float)sy - cy;
    const float dy2  = dy * dy;
    for (int i = 0; i <= TINT_RINGS; i++) {
        const float r  = rin + step * (float)i;
        const float d2 = r * r - dy2;
        lim[i] = (d2 <= 0.0f) ? 0 : (int)sqrtf(d2);
    }
}

uint32_t vg_tint_word(uint32_t v, int ring) {
    const uint32_t GH = 0xE000E000u;
    return (v & TINT_KEEP[ring]) | (((v & GH) >> TINT_GSHIFT[ring]) & GH)
         | TINT_GLOW[ring];
}

// One primitive, tinted by the ring under its centre. A long line crosses
// several rings and gets its centre's -- a visible simplification nobody will
// study during a boundary alarm.
uint16_t vg_tint_prim(uint16_t c, float x, float y) {
    if (s_tint_k <= 0.0f || c == 0) return c;
    const float cx = (float)(SCR_W / 2), cy = (float)(SCR_H / 2);
    const float rmax = sqrtf(cx * cx + cy * cy);
    const float rin  = rmax * (1.0f - s_tint_k);
    const float step = (rmax - rin) / (float)TINT_RINGS;
    const float r    = sqrtf((x - cx) * (x - cx) + (y - cy) * (y - cy));
    int ring = (int)((r - rin) / step);
    if (ring < 0) return c;
    if (ring >= TINT_RINGS) ring = TINT_RINGS - 1;
    const uint32_t v = ((uint32_t)c << 16) | c;
    return (uint16_t)vg_tint_word(v, ring);
}

static void band_wall_tint(uint16_t* band, int by0, float k) __attribute__((unused));
static void band_wall_tint(uint16_t* band, int by0, float k) {
    const float cx = (float)(SCR_W / 2), cy = (float)(SCR_H / 2);
    // The corner. At k=1 the innermost boundary reaches the centre and the whole
    // frame is inside the gradient.
    const float rmax = sqrtf(cx * cx + cy * cy);
    const float rin  = rmax * (1.0f - k);
    const float step = (rmax - rin) / (float)TINT_RINGS;

    for (int row = 0; row < BAND_H; row++) {
        // Only the rows the scanline pass left BRIGHT.
        //
        // This halves the pixel work, which is what pays for reddening amber at
        // all: rebuilding green's high field costs three more operations a word
        // than a mask does, and at full coverage that was 5.8ms and 48 fps.
        //
        // It is not a compromise on the look. band_scanlines has already darkened
        // the other rows, so the red lands exactly where there is brightness to
        // colour, and the result reads as a red vignette with the scanline texture
        // still in it rather than as stripes.
        if (((by0 + row) % SCANLINE_PITCH) == 0) continue;

        const float dy  = (float)(by0 + row) - cy;
        const float dy2 = dy * dy;
        uint16_t*   p   = band + row * SCR_W;

        // Where each ring boundary crosses this row, as a half-width. Rings that
        // do not reach the row at all clamp to the screen edge and their spans
        // come out empty.
        int lim[TINT_RINGS + 1];
        for (int i = 0; i <= TINT_RINGS; i++) {
            const float r  = rin + step * (float)i;
            const float d2 = r * r - dy2;
            lim[i] = (d2 <= 0.0f) ? 0 : (int)sqrtf(d2);
        }

        // Innermost ring is the faintest. Outside the last boundary is the corner
        // region, which stays at the strongest setting.
        for (int i = 0; i < TINT_RINGS; i++) {
            const int a = lim[i], b = lim[i + 1];
            tint_span(p, (int)cx + a, (int)cx + b, i);            // right
            tint_span(p, (int)cx - b, (int)cx - a, i);            // left
        }
        // Beyond the last boundary is the corner region, held at the rim setting.
        tint_span(p, (int)cx + lim[TINT_RINGS], SCR_W, TINT_RINGS - 1);
        tint_span(p, 0, (int)cx - lim[TINT_RINGS], TINT_RINGS - 1);
    }
}



void vg_rast_tint(float k) {
    // Written as "keep it only if it is in range" rather than as two rejections,
    // because a NaN fails BOTH `k < 0` and `k > 1` and would sail through a clamp
    // written the obvious way. Nothing produces a NaN wall distance today; a
    // clamp that cannot actually clamp is still not worth keeping.
    s_tint_k = (k >= 0.0f && k <= 1.0f) ? k : ((k > 0.0f) ? 1.0f : 0.0f);
}

// ---------------------------------------------------------------------------
// The set turning on and off
//
// A picture that cuts straight from the menu to the cockpit is a scene change.
// A picture that collapses to a line and comes back is a BROADCAST, which is
// what the tournament is meant to be -- so the transition carries the fiction
// rather than just covering the seam.
//
// Three independent controls, because the two directions are not mirror images
// and one "progress" number could not express either:
//   open  how much of the height is picture at all, centred; the aperture
//   glow  the bright scan band riding the aperture edge
//   dim   how far what remains is faded toward black
//
// All of it is per-ROW work. Whole rows go black, whole rows go white, and only
// the rows that are still picture pay for anything per-pixel -- which is what
// makes an effect over the whole screen affordable at all.
// ---------------------------------------------------------------------------

static float s_tv_open = 1.0f;   // how much of the height is lit, centred
static float s_tv_wide = 1.0f;   // ...and of the width: the dot before the line
static float s_tv_wash = 0.0f;   // how far the lit part is washed toward white
static float s_tv_dim  = 0.0f;   // ...and how far the rest is faded to black

void vg_rast_tv(float open, float wide, float wash, float dim) {
    // Same "keep it only if it is in range" shape as vg_rast_tint, and for the
    // same reason: a NaN fails every rejection test written the obvious way.
    #define TV_CLAMP(v) (((v) >= 0.0f && (v) <= 1.0f) ? (v) : (((v) > 0.0f) ? 1.0f : 0.0f))
    s_tv_open = TV_CLAMP(open);
    s_tv_wide = TV_CLAMP(wide);
    s_tv_wash = TV_CLAMP(wash);
    s_tv_dim  = TV_CLAMP(dim);
    #undef TV_CLAMP
}

bool vg_rast_tv_active(void) {
    return s_tv_open < 1.0f || s_tv_wide < 1.0f
        || s_tv_wash > 0.0f || s_tv_dim  > 0.0f;
}

// Dim a span by shifting every channel right, which is a halving per step.
//
// MASK FIRST, THEN SHIFT. Shifting the packed word first would drag blue's low
// bit into red's top bit and green's into blue's, so each field is isolated
// before it moves and re-masked after.
//
// WHERE GREEN ACTUALLY IS. VGC() swaps the two bytes of a native RGB565 word, so
// native R[15:11] G[10:5] B[4:0] is stored as:
//
//   bits 15..13  green's LOW three bits      bits 12..8  blue
//   bits  7..3   red                         bits  2..0  green's HIGH three bits
//
// Green's MOST significant bits are at the BOTTOM of the word. Reading 15..13 as
// "green high" because it sits in the high byte is the natural mistake and it is
// the one made here first: the wash raised red, blue and the bottom bit of green,
// which is the definition of magenta, and the tube flashed bright pink.
#define TV_R   0x00F800F8u
#define TV_B   0x1F001F00u
#define TV_G   0x00070007u      // green's top three bits
#define TV_R16 0x00F8u
#define TV_B16 0x1F00u
#define TV_G16 0x0007u

// Washing toward WHITE is the opposite operation and cheaper: raise the top bits
// of every channel. Four steps from untouched to full white, which is plenty for
// something that resolves in half a second.
//
//   L1  R bit 7        B bit 12        G bit 2
//   L2  R bits 7,6     B bits 12,11    G bits 2,1
//   L3  R bits 7,6,5   B bits 12..10   G bits 2,1,0
static const uint16_t TV_WASH[5] = { 0x0000, 0x1084, 0x18C6, 0x1CE7, 0xFFFF };

static inline uint16_t tv_px(uint16_t v, int s, uint16_t wash) {
    if (s >= 4) v = 0;
    else if (s > 0) v = (uint16_t)((((v & TV_R16) >> s) & TV_R16)
                                 | (((v & TV_B16) >> s) & TV_B16)
                                 | (((v & TV_G16) >> s) & TV_G16));
    return (uint16_t)(v | wash);
}

static inline void tv_span(uint16_t* row, int x0, int x1, int s, uint16_t wash) {
    if (x1 <= x0) return;
    if (s <= 0 && wash == 0) return;                       // nothing to do
    if (wash == 0xFFFF) {                                  // solid, skip the maths
        for (int x = x0; x < x1; x++) row[x] = 0xFFFF;
        return;
    }
    if (s >= 4 && wash == 0) { memset(row + x0, 0, (size_t)(x1 - x0) * 2); return; }

    int x = x0;
    if (x & 1) { row[x] = tv_px(row[x], s, wash); x++; }   // reach alignment

    const uint32_t w32 = (uint32_t)wash | ((uint32_t)wash << 16);
    uint32_t* p = (uint32_t*)(row + x);
    const int n = (x1 - x) >> 1;
    for (int i = 0; i < n; i++) {
        uint32_t v = p[i];
        if (s >= 4)      v = 0;
        else if (s > 0)  v = (((v & TV_R) >> s) & TV_R)
                           | (((v & TV_B) >> s) & TV_B)
                           | (((v & TV_G) >> s) & TV_G);
        p[i] = v | w32;
    }
    const int xt = x + n * 2;
    if (xt < x1) row[xt] = tv_px(row[xt], s, wash);        // odd tail
}

// ---------------------------------------------------------------------------
// The set turning on
//
// An old tube does not open like a shutter. The signal arrives, the line of the
// raster lights at the middle of the screen, and it spreads outward through the
// tube until the whole face is carrying picture -- so what the player should see
// is ONE bright bar at the centre that GROWS, not two edges travelling apart.
//
// That is the difference between this and the first version. The first lit the
// two boundaries of an opening aperture, which is a shutter with glowing edges:
// perfectly good-looking, and the wrong machine.
//
// Four controls, because the phases are genuinely independent:
//   wide  the dot opening into a line, before there is any height at all
//   open  the bar growing outward from the centre line
//   wash  how far what is lit is still raw white rather than resolved picture
//   dim   how far the rest of it has gone to black
//
// THE BAR RUNS ACROSS THE BAND, NOT DOWN IT. The band buffer is in panel space
// and the game is drawn through a quarter turn (see rot_pt in vg_raster.cpp), so
// logical (lx,ly) lands at panel (ly, H-1-lx) and a whole buffer row is a logical
// COLUMN. The first version of this effect ran sideways for exactly that reason.
// ---------------------------------------------------------------------------
static void band_tv(uint16_t* band, int by0) {
    (void)by0;

    // A six-level shift would band visibly across a fade this short, so the
    // fractional level is dithered by row -- adjacent rows sit either side of the
    // true level and the eye blends them, at no per-pixel cost.
    static const float ROWD[4] = { 0.125f, 0.625f, 0.375f, 0.875f };
    // Four levels, not five: green has three bits here to red and blue's five, so
    // a fifth step would zero green while the others still carried a tenth of
    // their range -- a fade to black that went through pink on the way.
    const float dimf = s_tv_dim * 4.0f;
    const int   dim0 = (int)dimf;
    const float frac = dimf - (float)dim0;

    const float washf = s_tv_wash * 4.0f;
    const int   wash0 = (int)washf;
    const float wfrac = washf - (float)wash0;

#if VG_ROTATE == 1 || VG_ROTATE == 3
    // Logical y -- the axis the bar grows along -- is the panel's x. A quarter
    // turn either way puts it there, and the bar is symmetric about the centre,
    // so 1 and 3 need no distinguishing; nor do 0 and 2.
    const float half = SCR_W * 0.5f;
    float ap = s_tv_open * half;
    if (s_tv_wash > 0.0f && ap < 1.5f) ap = 1.5f;   // the line itself, always lit
    int lo = (int)(half - ap), hi = (int)(half + ap);
    if (lo < 0)     lo = 0;
    if (hi > SCR_W) hi = SCR_W;
    if (hi < lo)    hi = lo;

    // Logical x is the panel's y: the dot spreading into a full-width line.
    const float hhalf = SCR_H * 0.5f;
    float wp = s_tv_wide * hhalf;
    if (s_tv_wash > 0.0f && wp < 1.5f) wp = 1.5f;
    const int rlo = (int)(hhalf - wp), rhi = (int)(hhalf + wp);

    for (int r = 0; r < BAND_H; r++) {
        uint16_t* row = band + r * SCR_W;
        const int y   = by0 + r;

        if (y < rlo || y >= rhi) {          // outside the line's length: dark
            memset(row, 0, SCR_W * 2);
            continue;
        }
        if (lo > 0)     memset(row,      0, (size_t)lo * 2);
        if (hi < SCR_W) memset(row + hi, 0, (size_t)(SCR_W - hi) * 2);

        const int s = dim0 + (ROWD[y & 3] < frac  ? 1 : 0);
        const int w = wash0 + (ROWD[y & 3] < wfrac ? 1 : 0);
        tv_span(row, lo, hi, s, TV_WASH[w > 4 ? 4 : w]);
    }
#else
    // Untested: this board is VG_ROTATE 1. Kept so the effect survives a port.
    const float half = SCR_H * 0.5f;
    float ap = s_tv_open * half;
    if (s_tv_wash > 0.0f && ap < 1.5f) ap = 1.5f;
    const int lo = (int)(half - ap), hi = (int)(half + ap);

    const float whalf = SCR_W * 0.5f;
    float wp = s_tv_wide * whalf;
    if (s_tv_wash > 0.0f && wp < 1.5f) wp = 1.5f;
    int clo = (int)(whalf - wp), chi = (int)(whalf + wp);
    if (clo < 0) clo = 0;
    if (chi > SCR_W) chi = SCR_W;

    for (int r = 0; r < BAND_H; r++) {
        const int y   = by0 + r;
        uint16_t* row = band + r * SCR_W;
        if (y < lo || y >= hi) { memset(row, 0, SCR_W * 2); continue; }
        if (clo > 0)     memset(row,       0, (size_t)clo * 2);
        if (chi < SCR_W) memset(row + chi, 0, (size_t)(SCR_W - chi) * 2);
        const int s = dim0  + (ROWD[y & 3] < frac  ? 1 : 0);
        const int w = wash0 + (ROWD[y & 3] < wfrac ? 1 : 0);
        tv_span(row, clo, chi, s, TV_WASH[w > 4 ? 4 : w]);
    }
#endif
}

// Rows [r0, r1) of the band. The lit rows are chosen from by0 so the pitch runs
// unbroken across band boundaries; restricting to a row range then advances to
// the first lit row at or after r0 rather than restarting the phase, which is
// what lets two cores each take half a band and land on the same pixels a single
// core would have.
static inline void band_scanlines(uint16_t* band, int by0, int r0, int r1) {
    int first = (SCANLINE_PITCH - (by0 % SCANLINE_PITCH)) % SCANLINE_PITCH;
    if (first < r0)
        first += ((r0 - first + SCANLINE_PITCH - 1) / SCANLINE_PITCH) * SCANLINE_PITCH;

    for (int y = first; y < r1; y += SCANLINE_PITCH) {
        // Rows are 960 bytes and the buffer is 16-byte aligned, so this is
        // always 4-byte aligned. The zero test still pays off wherever the
        // backdrop is dark.
        // Unrolled four ways. Measured at ~3.3ms a frame -- 20% of a 60fps
        // budget -- for 38,400 words of work that is five operations each, so
        // most of it was loop overhead rather than the arithmetic. SCR_W/2 is
        // 240, divisible by 4, so no remainder handling is needed.
        // BRANCHLESS now. The zero test paid while the screen was mostly
        // black; the sphere backdrop lights most of the frame, so nearly every
        // word took the branch AND did the work. scanline_pair(0) is 0, so the
        // unconditional store is safe, and four straight-line RMWs pipeline
        // where four tests stalled.
        uint32_t* p = (uint32_t*)&band[y * SCR_W];
        for (int i = 0; i < SCR_W / 2; i += 4) {
            const uint32_t a = p[i], b = p[i + 1], c = p[i + 2], d = p[i + 3];
            p[i]     = scanline_pair(a);
            p[i + 1] = scanline_pair(b);
            p[i + 2] = scanline_pair(c);
            p[i + 3] = scanline_pair(d);
        }
    }
}

// ---------------------------------------------------------------------------

// Raster cost, split three ways. Guessing which of these dominates has now been
// wrong twice: the primitive count alone cannot tell a long antialiased span
// from a triangle fill covering a third of the screen, and neither shows up
// against a constant backdrop cost. So measure all three.
static uint32_t s_sky_us = 0, s_prim_us = 0, s_scan_us = 0;

// The prim stage broken down by TYPE, in CPU cycles read off CCOUNT -- a
// micros() pair per primitive would cost more than some of the primitives.
// This exists because "prim 9.3ms" names a stage, not a culprit: whether that
// is antialiased ring segments or asteroid fills decides which knife to reach
// for, and guessing has burned this project before. Cheap enough to leave in:
// one pair of ~3-cycle counter reads per primitive per band it overlaps.
static uint32_t s_cyc_aa = 0, s_cyc_ln = 0, s_cyc_tri = 0, s_cyc_oth = 0;
// The canopy on its OWN counter, because `oth` is a bucket -- glyphs, fills and points
// live there too, and they grow with how busy the fight is. Four rounds of canopy
// optimisation were read off `oth` and two of those readings were really the HUD's text
// getting longer. A number that moves for reasons other than the thing being measured
// is not a measurement.
static uint32_t s_cyc_can = 0;

// AND `oth` SPLIT INTO ITS THREE, for the same reason the canopy got its own counter.
//
// `oth` is a bucket -- points, glyphs and rectangle fills -- and it is the largest item in
// `prim` during a course run. Optimising against a bucket is how four rounds of canopy work
// came out unattributable: any of the three can move for a reason that has nothing to do
// with the change being measured. Points scale with speed, glyphs with how much the HUD has
// to say, fills with the instruments drawn.
static uint32_t s_cyc_pt = 0, s_cyc_gl = 0, s_cyc_fl = 0;
static uint32_t s_tint_us = 0;
uint32_t vg_rast_aa_us(void)   { return s_cyc_aa  / 240u; }
uint32_t vg_rast_ln_us(void)   { return s_cyc_ln  / 240u; }
uint32_t vg_rast_tri_us(void)  { return s_cyc_tri / 240u; }
uint32_t vg_rast_oth_us(void)  { return s_cyc_oth / 240u; }
uint32_t vg_rast_can_us(void)  { return s_cyc_can / 240u; }
uint32_t vg_rast_pt_us(void)   { return s_cyc_pt  / 240u; }
uint32_t vg_rast_gl_us(void)   { return s_cyc_gl  / 240u; }
uint32_t vg_rast_fl_us(void)   { return s_cyc_fl  / 240u; }
uint32_t vg_rast_ln_px(void)   { return s_ln_px; }
uint32_t vg_rast_ln_n(void)    { return s_ln_n; }
uint32_t vg_rast_tint_us(void) { return s_tint_us; }

uint32_t vg_rast_sky_us(void)  { return s_sky_us; }
uint32_t vg_rast_prim_us(void) { return s_prim_us; }
uint32_t vg_rast_scan_us(void) { return s_scan_us; }

// ===========================================================================
// THE ROW SPLIT: half a band on each core
//
// Bands cannot be parallelised against each other -- the active list is built
// incrementally across them and RAMWRC forces the transfers out in order -- but
// within one band the rows are independent, and core 0 spends ~15 ms of every
// frame doing nothing.
//
// ONLY THE TWO PASSES THAT HAVE NO CROSS-ROW STATE go through here: the backdrop
// fill and the scanline overlay. Both are per-pixel functions of the row index,
// so splitting them is bit-identical and the replay proves it.
//
// The primitives are NOT split, and that is a correctness decision rather than a
// scheduling one. A line is a Bresenham or Wu walk whose phase is set by where
// it was clipped, so clipping one at a mid-band row and rasterising the halves
// separately does not reproduce the pixels a full-band walk produces -- it would
// add fifteen more of the +-1px boundary jogs the band edges already make. That
// is a change to the look, which is the author's call and not a free win.
//
// The split point must be EVEN: the backdrop walks rows in pairs and a pair that
// straddled the boundary would be filled by one core and copied by the other.
enum { ROW_SPLIT = BAND_H / 2 };
static_assert(ROW_SPLIT % 2 == 0, "row split must not straddle a backdrop pair");

// RS_PREP is not a row split at all -- it is a range of BANDS, and it reuses this
// machinery because the handshake and the helper task are exactly what it needs. r0 and r1
// carry the band range instead of a row range.
enum { RS_SKY = 0, RS_SCAN = 1, RS_CANOPY = 2, RS_PREP = 3 };

// Forward: the canopy pass is a third row-splittable job, and it is the largest of
// them. Declared here because the helper task below dispatches to it.
static void canopy_rows(uint16_t* band, int by0, int r0, int r1);

static SemaphoreHandle_t s_rs_go   = nullptr;
static SemaphoreHandle_t s_rs_done = nullptr;
static struct {
    uint16_t* band;
    int       by0, r0, r1;
    uint8_t   op;
} s_rs;

static void rowsplit_task(void*) {
    for (;;) {
        xSemaphoreTake(s_rs_go, portMAX_DELAY);
        if      (s_rs.op == RS_SKY)    vg_sky_fill_rows(s_rs.band, s_rs.by0, s_rs.r0, s_rs.r1);
        else if (s_rs.op == RS_CANOPY) canopy_rows(s_rs.band, s_rs.by0, s_rs.r0, s_rs.r1);
        else if (s_rs.op == RS_PREP)   vg_sky_prep_bands(s_rs.r0, s_rs.r1);
        else                           band_scanlines(s_rs.band, s_rs.by0, s_rs.r0, s_rs.r1);
        xSemaphoreGive(s_rs_done);
    }
}

// Hands the bottom half to core 0 and says whether it took it. A false return
// means the caller does the whole band itself, so a failed task creation costs
// frame rate and nothing else.
//
// Priority 3, above the audio task at 2: a rendezvous that waited behind an
// audio chunk would cost more than the split saves. Audio has a 65 ms ring
// against thirty bursts of ~90 us, so it never notices.
static bool rowsplit_start(uint8_t op, uint16_t* band, int by0, int r0, int r1) {
    if (!s_rs_go) {
        s_rs_go   = xSemaphoreCreateBinary();
        s_rs_done = xSemaphoreCreateBinary();
        if (!s_rs_go || !s_rs_done) return false;
        if (xTaskCreatePinnedToCore(rowsplit_task, "rowsplit", 4096, nullptr,
                                    3, nullptr, 0) != pdPASS) {
            s_rs_go = nullptr;      // and never try again
            return false;
        }
    }
    s_rs.op = op; s_rs.band = band; s_rs.by0 = by0; s_rs.r0 = r0; s_rs.r1 = r1;
    // The give is the release fence for everything above, including the band
    // prep the helper is about to read.
    xSemaphoreGive(s_rs_go);
    return true;
}

static inline void rowsplit_wait(void) { xSemaphoreTake(s_rs_done, portMAX_DELAY); }

// ===========================================================================
// THE BAKED CANOPY
//
// The author draws the frame on a grey field and this applies it as a CHANGE to the
// finished picture: brighter than the background adds light, darker takes it away. So
// the frame is lit BY the scene rather than painted over it, and it holds that
// relationship over a dark nebula and a bright one alike -- which is exactly what an
// opaque line cannot do, and what the sky gamma made obvious.
//
// COLUMNS, because the panel is turned a quarter turn: panel_x is the picture's y and
// panel_y is 479 minus its x, so one band is 32 columns of the drawing and a column
// layout keeps each band's data contiguous. LEFT HALF ONLY -- the drawing is symmetric
// to within four levels of antialiasing noise, so column x serves 479-x as well and
// nothing needs flipping inside a run, since mirroring in x leaves the y-runs alone.
//
// Empty pixels are not stored: a frame covers ~7% of the screen, so the table is runs
// of used pixels and skips the rest. 9.5 KB in FLASH, where there are megabytes free,
// rather than the 21 KB of internal SRAM that is actually scarce.
//
// The grey levels come through a 256-entry table built once, so the per-pixel work is
// a load and a saturating add rather than any arithmetic on the hue.
static uint16_t s_can_lut[256];
static bool     s_can_ready = false;

static void canopy_lut(void) {
    for (int g = 0; g < 256; g++) {
        const float f = (g > CANOPY_BG)
                      ? (float)(g - CANOPY_BG) / (float)(255 - CANOPY_BG)
                      : (float)(CANOPY_BG - g) / (float)CANOPY_BG;
        // NATIVE order, swapped once here. Palette colours are stored in panel order
        // and blend_px works in native, so leaving this unswapped would put the delta's
        // red into the blue channel -- a bug that would have looked like a design
        // decision rather than a mistake.
        const uint16_t c = vg_dim(COL_HUD, f);
        s_can_lut[g] = (uint16_t)((c >> 8) | (c << 8));
    }
    s_can_ready = true;
}

// A RUN OF ONE DELTA, which is where the cost of this actually lives.
//
// Two masked adds instead of three channel extracts. R+B share a word because nothing
// can carry between them once G is masked out, and each field gets a spare bit above it
// to catch the overflow: bit 16 above R, bit 5 above B, bit 11 above G. If the guard bit
// is set the field saturated, so fill it -- no compares against 31 and 63, no
// per-channel shifting back and forth.
//
// Subtraction is the same trick run backwards: set the guard bits first, and a field
// that borrows through its guard has underflowed and clamps to zero.
//
// The delta is hoisted out of the loop, which is the other half of the win and the
// reason the table stores runs of one level rather than a level per pixel.
// A GUARD BIT FILLS ITS OWN FIELD, which is what makes this branchless without costing
// more than the branches did.
//
// For a field of width w with its guard bit immediately above it, `m - (m >> w)` turns a
// set guard into a solid field of ones and a clear guard into nothing. Red sits at bits
// 11..15 under a guard at 16, so 0x10000 - 0x800 is 0xF800: exactly the red field. Blue
// at 0..4 under bit 5 gives 0x20 - 1 = 0x1F. Both guards at once still work, because the
// shifted fields do not overlap. Green is width 6 under bit 11, so it shifts by 6.
//
// The point is not the two cycles of branch. It is that the branchy form needed seven
// mask constants -- 0x1F, 0xF800, 0x7E0 and their complements -- and sixteen registers
// could not hold them alongside two interleaved pixels. The compiler was reloading them
// from the stack and fetching ~0xF800 out of the literal pool EVERY iteration. Four
// constants fit; seven did not.
//
// The input is not masked to 16 bits: every path below masks with 0xF81F or 0x07E0
// anyway, so the high bytes the swap leaves behind cannot reach the result.
static inline uint32_t px_add(uint32_t s, uint32_t drb, uint32_t dg) {
    const uint32_t v = (s >> 8) | (s << 8);
    uint32_t rb = (v & 0xF81Fu) + drb;
    uint32_t g  = (v & 0x07E0u) + dg;
    const uint32_t mrb = rb & 0x10020u;         // a set guard means that field overflowed
    const uint32_t mg  = g  & 0x00800u;
    rb |= mrb - (mrb >> 5);                     // ...so fill it to the ceiling
    g  |= mg  - (mg  >> 6);
    const uint32_t o = (rb & 0xF81Fu) | (g & 0x07E0u);
    return ((o >> 8) | (o << 8)) & 0xFFFFu;
}

// Subtraction is the same trick run backwards: set the guards first, and a field that
// borrowed through its guard has underflowed. Here a STILL-SET guard is the good case, so
// the same expression becomes a keep-mask and clears the guard on its way out -- which is
// why this one needs no final masking at all.
static inline uint32_t px_sub(uint32_t s, uint32_t drb, uint32_t dg) {
    const uint32_t v = (s >> 8) | (s << 8);
    uint32_t rb = ((v & 0xF81Fu) | 0x10020u) - drb;
    uint32_t g  = ((v & 0x07E0u) | 0x00800u) - dg;
    const uint32_t mrb = rb & 0x10020u;          // a guard that survived did not borrow
    const uint32_t mg  = g  & 0x00800u;
    rb &= mrb - (mrb >> 5);                      // ...and one that did not, zeroes it
    g  &= mg  - (mg  >> 6);
    const uint32_t o = rb | g;
    return ((o >> 8) | (o << 8)) & 0xFFFFu;
}

// TWO PIXELS PER ACCESS, because this loop is bound by memory and not by arithmetic.
// Taking eight cycles of maths out of it earlier changed nothing at all -- the work was
// already hidden behind the load and the store -- so the only thing left to remove is
// the number of accesses. One 32-bit load and one 32-bit store per pair halves them.
//
// The pixel maths stays per-pixel on purpose. Packing both into one 32-bit add would
// need a spare bit above red to catch its carry, and red sits at the top of its half,
// so the carry lands in the neighbouring pixel's blue. That is a real trap and not
// worth the two cycles it would save.
//
// Xtensa will not do an unaligned 32-bit load, so a span starting on an odd pixel does
// that one singly first.
#define SPAN_BODY(FN)                                                        \
    const uint32_t drb = (uint32_t)d & 0xF81Fu, dg = (uint32_t)d & 0x07E0u;  \
    int i = 0;                                                               \
    if (n > 0 && (((uintptr_t)q & 3u) != 0u)) {                              \
        q[0] = (uint16_t)FN(q[0], drb, dg);                                  \
        i = 1;                                                               \
    }                                                                        \
    for (; i + 1 < n; i += 2) {                                              \
        uint32_t* w = (uint32_t*)(void*)(q + i);                             \
        const uint32_t v = *w;                                               \
        *w = FN(v & 0xFFFFu, drb, dg) | (FN(v >> 16, drb, dg) << 16);         \
    }                                                                        \
    for (; i < n; i++) q[i] = (uint16_t)FN(q[i], drb, dg);

static inline void span_add(uint16_t* q, int n, uint16_t d) { SPAN_BODY(px_add) }
static inline void span_sub(uint16_t* q, int n, uint16_t d) { SPAN_BODY(px_sub) }

// A PIXEL AT A TIME, for the antialiased edge of a shape.
//
// The flat span above is the cheap path and carries most of the area, but it earns that
// by hoisting the delta, which only works where the level is constant. At the edge of a
// member every pixel is a different level, and stored as flat spans those were 78% of
// all the blocks while carrying 18% of the pixels -- four bytes of header to deliver one
// byte of level, and a header decode is worth several pixels.
//
// So a stretch of them is one block with a level per pixel. The delta comes back inside
// the loop, but the pairing survives: two pixels in one 32-bit access still works when
// their deltas differ, because the maths is per-pixel either way.
#define SPAN_LIT_BODY(FN)                                                    \
    int i = 0;                                                               \
    if (n > 0 && (((uintptr_t)q & 3u) != 0u)) {                              \
        const uint32_t d = s_can_lut[lv[0]];                                 \
        q[0] = (uint16_t)FN(q[0], d & 0xF81Fu, d & 0x07E0u);                 \
        i = 1;                                                               \
    }                                                                        \
    for (; i + 1 < n; i += 2) {                                              \
        const uint32_t d0 = s_can_lut[lv[i]], d1 = s_can_lut[lv[i + 1]];     \
        uint32_t* w = (uint32_t*)(void*)(q + i);                             \
        const uint32_t v = *w;                                               \
        *w = FN(v & 0xFFFFu, d0 & 0xF81Fu, d0 & 0x07E0u)                     \
           | (FN(v >> 16, d1 & 0xF81Fu, d1 & 0x07E0u) << 16);                \
    }                                                                        \
    for (; i < n; i++) {                                                     \
        const uint32_t d = s_can_lut[lv[i]];                                 \
        q[i] = (uint16_t)FN(q[i], d & 0xF81Fu, d & 0x07E0u);                 \
    }

// NOT INLINED, and that is the whole point of it.
//
// Inlined, all four span variants and the block walk are live in one function at once,
// and Xtensa has sixteen visible registers. The compiler ran out: the disassembled pair
// loop was 79 instructions for two pixels, and among them were three stack reloads, two
// rematerialised constants and a literal-pool fetch -- every iteration. The arithmetic
// was never the cost; the spilling was.
//
// Out of line, this loop gets the register file to itself. A call costs a handful of
// cycles once per block, and a block averages thirty-odd pixels.
static __attribute__((noinline)) void span_lit_add(uint16_t* q, int n, const uint8_t* lv) { SPAN_LIT_BODY(px_add) }
static __attribute__((noinline)) void span_lit_sub(uint16_t* q, int n, const uint8_t* lv) { SPAN_LIT_BODY(px_sub) }

// Rows [r0, r1) of the band, so the pass can be halved across the cores. Each panel
// row is an independent column of the drawing -- nothing carries between them -- which
// is what makes this splittable at all, and it is by far the biggest of the three
// row-split jobs: measured at ~4 ms against the backdrop's 2.5 and the scanlines' 1.8.
// ===========================================================================
// THE WARP
//
// The frame flexes with the throttle, and it costs almost nothing, because the table is
// already a list of RUNS and a run can be moved by moving its endpoints. Two tables:
//
//   s_wy[y]   where the drawing's y goes. A stretch about the centre, so the members spread
//             as the throttle opens. Applied to a block's START and END and the length taken
//             as the difference, which is what makes it gap-free -- one run's end is the next
//             run's start, so mapping both through the same table keeps them touching.
//   s_wc[col] a shift for the whole column. Larger toward the edges, so the stretch bows
//             rather than sliding, which is the difference between a frame flexing and a
//             frame being dragged.
//
// Two table reads, a subtract and an add per block: 2,492 blocks, about 60 us. Nothing here
// touches the per-PIXEL loop, which is where the 37 cycles live.
//
// The maps are rebuilt only when the QUANTISED amount changes. The HUD's warp has to be
// quantised deliberately -- a fractional shift moves every instrument line by part of a pixel
// and reads as shimmer -- and this one quantises itself, because a table of pixel offsets has
// nowhere to put a fraction.
static int16_t s_wy[SCR_W + 1];
static int16_t s_wc[SCR_H];
static int     s_wq = -1;                 // the quantised amount the maps were built for
static bool    s_warp_on = false;

void vg_canopy_warp(float k) {
    if (k < 0.0f) k = 0.0f;
    if (k > 1.0f) k = 1.0f;
    const int q = (int)(k * CANOPY_WARP_STEPS + 0.5f);
    s_warp_on = (q != 0);
    if (q == s_wq) return;
    s_wq = q;

    const float a  = (float)q / (float)CANOPY_WARP_STEPS;
    const float cy = (float)(SCR_W - 1) * 0.5f;
    const float s  = 1.0f + CANOPY_WARP_STRETCH * a;
    for (int y = 0; y <= SCR_W; y++) {
        int v = (int)(cy + ((float)y - cy) * s + 0.5f);
        if (v < 0) v = 0; else if (v > SCR_W) v = SCR_W;
        s_wy[y] = (int16_t)v;
    }
    // The bow, per column, measured from the middle of the frame outward.
    const float cx = (float)(SCR_H - 1) * 0.5f;
    for (int c = 0; c < SCR_H; c++) {
        const float t = ((float)c - cx) / cx;          // -1 .. 1
        s_wc[c] = (int16_t)(int)(CANOPY_WARP_BOW * a * t * t * (t < 0.0f ? -1.0f : 1.0f));
    }
}

// WARP OR NO WARP, settled at compile time.
//
// Asked per block, the flag cost 83 us of a 4042 us pass for a question whose answer is the
// same for the whole frame -- the same shape of waste the sky fill's tint flag was. Two
// instantiations, and the rigid one carries no warp code at all.
template <bool WARP>
static void canopy_rows_t(uint16_t* band, int by0, int r0, int r1) {
    if (!s_can_ready) canopy_lut();

    for (int py = by0 + r0; py < by0 + r1; py++) {
        const int lx = SCR_H - 1 - py;                 // this panel row IS a column
        // Mirrored only when the drawing is symmetric, which the baker decides and
        // records. An asymmetric frame stores every column and is read straight
        // through -- a cockpit is allowed to be lopsided.
        const int c  = (CANOPY_MIRROR && lx >= CANOPY_COLS) ? (SCR_W - 1 - lx) : lx;
        if (c < 0 || c >= CANOPY_COLS) continue;

        const uint8_t* p = &CANOPY_DATA[CANOPY_OFS[c]];
        const uint8_t* e = &CANOPY_DATA[CANOPY_OFS[c + 1]];
        uint16_t* row = &band[(py - by0) * SCR_W];

        // Three bytes of header, then either one level for the whole block or a level
        // per pixel. Nine bits each of start and length, so the odd bit of both rides
        // in the flag byte. The side is in the header too: a block never crosses the
        // background, so nothing here tests light against shade per pixel.
        const int wofs = WARP ? s_wc[c] : 0;

        while (p + 3 <= e) {
            const uint8_t h   = p[0];
            const int     y0  = ((h & 1) << 8) | p[1];   // the picture's y is panel x
            const int     len = ((h & 2) << 7) | p[2];
            p += 3;

            int at = y0, n = len, skip = 0;
            if (WARP) {
                // Both ends through the same map, and the length is the difference -- so the
                // run still ends exactly where the next one starts.
                at = s_wy[y0] + wofs;
                n  = s_wy[y0 + len] + wofs - at;
                // THE TABLE'S OWN BOUNDS NO LONGER APPLY. Baked, every block was inside the
                // row by construction and the baker verifies it; moved at runtime, that
                // guarantee is gone and a block can hang off either edge. Trimming the FRONT
                // also has to advance the level array, or a literal block would paint its
                // remaining pixels with the colours of the ones that were clipped.
                if (at < 0)            { skip = -at; n -= skip; at = 0; }
                if (at + n > SCR_W)    { n = SCR_W - at; }
                if (n <= 0) { if (h & 0x80) p += len; else p++; continue; }
            }

            if (h & 0x80) {                              // a level per pixel
                if (h & 0x40) span_lit_sub(&row[at], n, p + skip);
                else          span_lit_add(&row[at], n, p + skip);
                p += len;
            } else {                                     // one level, delta hoisted
                const uint16_t d = s_can_lut[*p++];
                if (h & 0x40) span_sub(&row[at], n, d);
                else          span_add(&row[at], n, d);
            }
        }
    }
}

static void canopy_rows(uint16_t* band, int by0, int r0, int r1) {
    if (s_warp_on) canopy_rows_t<true>(band, by0, r0, r1);
    else           canopy_rows_t<false>(band, by0, r0, r1);
}

// A FAN OF LINES, both ways, over every slope.
//
// Endpoints are generated inside one band and across the full width, from a fixed sequence,
// so the set covers shallow, steep and diagonal alike and is the same set every run. Both
// banks are compared: a faster walk that puts a pixel in a different place is not faster.
void vg_line_bench(VgLineCost* out) {
    if (!s_band[0] || !s_band[1]) { *out = VgLineCost{}; return; }

    struct Seg { int16_t x0, y0, x1, y1; };
    static Seg seg[256];
    static int nseg = 0;
    if (!nseg) {
        uint32_t r = 12345u;
        const int H = BAND_H - 1;
        for (int i = 0; i < 256; i++) {
            r = r * 1664525u + 1013904223u;
            seg[i].x0 = (int16_t)((r >> 16) % SCR_W);
            r = r * 1664525u + 1013904223u;
            seg[i].y0 = (int16_t)((r >> 16) % (H + 1));
            r = r * 1664525u + 1013904223u;
            seg[i].x1 = (int16_t)((r >> 16) % SCR_W);
            r = r * 1664525u + 1013904223u;
            seg[i].y1 = (int16_t)((r >> 16) % (H + 1));
        }
        nseg = 256;
    }

    uint32_t ca = 0, cb = 0;
    long px = 0;
    memset(s_band[0], 0, SCR_W * BAND_H * 2);
    memset(s_band[1], 0, SCR_W * BAND_H * 2);

    for (int rep = 0; rep < 4; rep++) {
        const uint32_t t0 = esp_cpu_get_cycle_count();
        for (int i = 0; i < nseg; i++)
            band_line_fast_ref(s_band[0], 0, BAND_H - 1,
                               seg[i].x0, seg[i].y0, seg[i].x1, seg[i].y1, 0x17CD);
        const uint32_t t1 = esp_cpu_get_cycle_count();
        for (int i = 0; i < nseg; i++)
            band_line_fast(s_band[1], 0, BAND_H - 1,
                           seg[i].x0, seg[i].y0, seg[i].x1, seg[i].y1, 0x17CD);
        const uint32_t t2 = esp_cpu_get_cycle_count();
        ca += t1 - t0;
        cb += t2 - t1;
    }
    for (int i = 0; i < nseg; i++) {
        const int a = abs(seg[i].x1 - seg[i].x0), b = abs(seg[i].y1 - seg[i].y0);
        px += (a > b ? a : b) + 1;
    }
    out->ref_us = ca / 240u;
    out->now_us = cb / 240u;
    out->lines  = nseg * 4;
    out->px     = px * 4;
    out->same   = memcmp(s_band[0], s_band[1], SCR_W * BAND_H * 2) == 0;
}

// THE GLYPH NEST, AS IT WAS, kept only so the bench has something to be faster than.
//
// `gl` on the telemetry line cannot answer this on its own: it is a per-frame total and the
// amount of text on screen changes frame to frame, so two captures describe two different
// workloads. The same bucket problem the canopy had, one level down. This is the plain nest,
// benched against the hoisted one over the same fixed text, in the same build.
static void band_glyph_ref(uint16_t* band, int by0, int by1, const Prim* p) {
    const int      scale = p->x1;
    const uint8_t* glyph = VG_FONT5X7[p->y1 - VG_FONT_FIRST];
    for (int col = 0; col < 5; col++) {
        uint8_t bits = glyph[col];
        if (!bits) continue;
        for (int row = 0; row < 7; row++) {
            if (!(bits & (1 << row))) continue;
            for (int jy = 0; jy < scale; jy++) {
                for (int jx = 0; jx < scale; jx++) {
                    const int dx = col * scale + jx;
                    const int dy = row * scale + jy;
                    const int xx = p->x0 + dy, yy = p->y0 - dx;
                    if (yy < by0 || yy > by1) continue;
                    if ((unsigned)xx >= (unsigned)SCR_W) continue;
                    band[(yy - by0) * SCR_W + xx] = p->color;
                }
            }
        }
    }
}

// A PAGE OF TEXT, benched both ways, over every band it touches.
//
// The workload is fixed and deliberately spread: three scales, the whole printable range,
// and glyph rows placed so plenty of them straddle a band boundary -- which is the case the
// hoist is meant to help, because a glyph is re-walked in full for every band it touches and
// most of those walks are rejections.
//
// Both banks are checksummed and compared. The claim is that the pixels are identical and
// only the order changed, and an assertion beats a claim.
void vg_glyph_bench(VgGlyphCost* out) {
    if (!s_band[0] || !s_band[1]) { *out = VgGlyphCost{}; return; }

    Prim g[96];
    int n = 0;
    for (int scale = 1; scale <= 3 && n < 96; scale++) {
        for (int c = 0; c < 32 && n < 96; c++) {
            Prim& q = g[n++];
            q.type  = PRIM_GLYPH;
            q.x1    = (int16_t)scale;
            q.y1    = (int16_t)(VG_FONT_FIRST + (c % (VG_FONT_LAST - VG_FONT_FIRST + 1)));
            q.x0    = (int16_t)(11 + (c * 13) % 400);
            q.y0    = (int16_t)(30 + (c * 29 + scale * 7) % 420);
            q.color = 0x17CD;
            q.aa    = 0;
            // The row range the active list would bucket it by -- see vg_text.
            q.ymin  = (int16_t)(q.y0 - (5 * scale - 1));
            q.ymax  = q.y0;
        }
    }

    // ONLY THE BANDS A GLYPH ACTUALLY TOUCHES, because that is all the renderer visits.
    //
    // The active list buckets each primitive by its row range, and a glyph spans 5*scale
    // panel rows -- fifteen at the largest scale, against a band height of 32. So it lands
    // in one band or two, never fifteen. An earlier version of this bench called every band
    // for every glyph and reported the hoist as 4x; almost all of that was rejection work
    // the renderer never does. A bench measuring a workload the program does not have is
    // worse than no bench, because it is believed.
    uint32_t ca = 0, cb = 0;
    for (int b = 0; b < NUM_BANDS; b++) {
        const int by0 = b * BAND_H, by1 = by0 + BAND_H - 1;
        memset(s_band[0], 0, SCR_W * BAND_H * 2);
        memset(s_band[1], 0, SCR_W * BAND_H * 2);

        const uint32_t t0 = esp_cpu_get_cycle_count();
        for (int i = 0; i < n; i++)
            if (g[i].ymax >= by0 && g[i].ymin <= by1)
                band_glyph_ref(s_band[0], by0, by1, &g[i]);
        const uint32_t t1 = esp_cpu_get_cycle_count();
        for (int i = 0; i < n; i++)
            if (g[i].ymax >= by0 && g[i].ymin <= by1)
                band_glyph(s_band[1], by0, by1, &g[i]);
        const uint32_t t2 = esp_cpu_get_cycle_count();
        ca += t1 - t0;
        cb += t2 - t1;

        if (memcmp(s_band[0], s_band[1], SCR_W * BAND_H * 2) != 0) out->same = false;
    }
    out->ref_us = ca / 240u;
    out->now_us = cb / 240u;
    out->glyphs = n;
}

// WHAT THE BACKDROP COSTS, on the same terms as the canopy.
//
// `sky` on the telemetry line is the second largest item in the frame and it has never been
// looked at. It also brackets three different things -- the per-band chart prep, the fill
// itself, and the rendezvous -- so the line cannot say which of them is expensive.
//
// This runs prep and a whole-band fill on one core, band by band, over a real band buffer.
// The buffers are idle outside a flush and there is not 30 KB of heap free to allocate
// another, so it borrows one.
//
// THE CHECKSUM IS THE POINT, as much as the timing. The backdrop must come out bit-identical
// after any change to this loop -- a replay renders frame for frame, and the fill's own
// comments record two places where an optimisation was chosen specifically because it could
// not alter a rounding. Fold the whole band in and compare before and after.
//
// Which means the bench has to be reproducible across two FLASHES, not just two calls, and
// that takes more pinning than it first appears. The view drifts, so it is pinned; and the
// attract loop regenerates the backdrop, so the texture is regenerated here from a fixed kind
// and seed. Both were found by watching the checksum move while nothing else did.
//
// It therefore has a visible side effect: the sky on screen changes to this one. That is a
// fair price for a number that means something, and it only happens when asked.
void vg_sky_bench(VgSkyCost* out) {
    uint32_t pc = 0, fc = 0, sum = 2166136261u;
    if (!s_band[0]) { *out = VgSkyCost{}; return; }

    vg_sky_generate(SKY_NEBULA, 0x5EED1234u);
    if (!vg_sky_ready()) { *out = VgSkyCost{}; return; }
    vg_sky_bench_pin(true);
    vg_sky_prep_begin();
    for (int b = 0; b < NUM_BANDS; b++) {
        const int by0 = b * BAND_H;

        // Prep on ONE core here, deliberately. The frame splits it across both, so the
        // frame pays about half of what this reports -- the same relationship the fill has.
        // What the bench is for is the cost of the work, not the schedule.
        const uint32_t t0 = esp_cpu_get_cycle_count();
        vg_sky_prep_bands(b, b + 1);
        const uint32_t t1 = esp_cpu_get_cycle_count();
        vg_sky_fill_rows(s_band[0], by0, 0, BAND_H);
        const uint32_t t2 = esp_cpu_get_cycle_count();
        pc += t1 - t0;
        fc += t2 - t1;

        // Outside the timing: what the band came out as, folded in a word at a time.
        const uint32_t* w = (const uint32_t*)(const void*)s_band[0];
        for (int i = 0; i < SCR_W * BAND_H / 2; i++) sum = (sum ^ w[i]) * 16777619u;
    }
    vg_sky_bench_pin(false);
    out->prep_us = pc / 240u;
    out->fill_us = fc / 240u;
    out->sum     = sum;
}

// WHAT THE DRAWING COSTS, on the device and without flying.
//
// The canopy only draws inside a match, so reading its cost off the frame counter needs
// someone at the controls -- and that counter reports one band's worse half plus the
// rendezvous, which is not a number the baker can predict from a table. This runs the
// whole pass on one core over a scratch row and reports it straight.
//
// Row by row, because a real band buffer is 30 KB and there is not that much heap free
// at runtime. Each panel row is an independent column of the drawing, so a row at a time
// is the same work in the same order; internal SRAM is uniform, so a reused row costs
// what a spread-out band costs.
//
// The scratch is refilled between rows, outside the timing. Blending into the same row
// 480 times would drive every channel to the rail and leave every saturation branch
// taken, which is not what the pass meets in a frame.
void vg_canopy_bench(VgCanopyCost* out) {
    static uint16_t scratch[SCR_W];
    if (!s_can_ready) canopy_lut();

    // BOTH STATES, because the warp is the thing most likely to be changed next and its cost
    // is the question. The game's own setting is saved and put back, so running the bench does
    // not leave the frame flexing at whatever the bench used last.
    const int   save_q  = s_wq;
    const bool  save_on = s_warp_on;

    uint32_t cyc = 0, cal = 0, wcyc = 0;
    vg_canopy_warp(0.0f);
    for (int py = 0; py < SCR_H; py++) {
        for (int x = 0; x < SCR_W; x++) scratch[x] = 0x1084;   // mid grey, both ways to go
        const uint32_t t0 = esp_cpu_get_cycle_count();
        canopy_rows(scratch, py, 0, 1);
        cyc += esp_cpu_get_cycle_count() - t0;

        // The same two reads around nothing, so the harness pays for itself.
        const uint32_t t1 = esp_cpu_get_cycle_count();
        asm volatile("" ::: "memory");
        cal += esp_cpu_get_cycle_count() - t1;
    }
    vg_canopy_warp(1.0f);
    for (int py = 0; py < SCR_H; py++) {
        for (int x = 0; x < SCR_W; x++) scratch[x] = 0x1084;
        const uint32_t t0 = esp_cpu_get_cycle_count();
        canopy_rows(scratch, py, 0, 1);
        wcyc += esp_cpu_get_cycle_count() - t0;
    }
    s_wq = save_q; s_warp_on = save_on;   // leave the game's own flex as it was

    out->us      = (cyc > cal ? cyc - cal : 0) / 240u;
    out->warp_us = (wcyc > cal ? wcyc - cal : 0) / 240u;
    out->blocks  = CANOPY_BLOCKS;
    out->flat_px = CANOPY_FLAT_PX;
    out->lit_px  = CANOPY_LIT_PX;
}

static void draw_band(int band_index, uint16_t* band) {
    const int by0 = band_index * BAND_H;
    const int by1 = by0 + BAND_H - 1;

    const uint32_t t_sky = micros();

    // The backdrop fill REPLACES the clear rather than adding to it, so its net
    // cost is only what it exceeds a memset by.
    if (vg_sky_ready()) {
        // The chart is already built, for every band, by vg_sky_prep_all before the band
        // loop began -- see the note there. This used to prep the band right here, on this
        // core, with the helper idle and nothing overlapping it.
        const bool split = rowsplit_start(RS_SKY, band, by0, ROW_SPLIT, BAND_H);
        vg_sky_fill_rows(band, by0, 0, split ? ROW_SPLIT : BAND_H);
        if (split) rowsplit_wait();
    } else {
        memset(band, 0, SCR_W * BAND_H * 2);
    }

    const uint32_t t_prim = micros();
    s_sky_us += t_prim - t_sky;

    const Prim* prims = vg_prim_list();
    const int   n     = vg_prim_live();

    // THE ACTIVE LIST, built once per frame. Every band used to test every
    // primitive against its row range: fifteen bands times seven hundred
    // primitives is ten thousand rejections a frame, nearly a millisecond of
    // pure bookkeeping in the heaviest scenes. Classic scanline instead: each
    // primitive is bucketed by its first band, bands are drawn in order, and a
    // compacting active array carries primitives forward until their last row
    // passes. Each primitive is now touched once to insert and once per band it
    // actually spans.
    // Two active buffers, because the update is a MERGE. Survivors and the
    // band's new admissions are each sorted by primitive index -- submission
    // order, which is draw order, which is the painter's algorithm -- and
    // simply appending the new ones after the old would draw a late-submitted
    // primitive under an early one wherever their first bands differ. A trail
    // would poke through the comms badge. Merging keeps the order exact.
    static uint16_t s_active[2][MAX_PRIMS];
    static int      s_active_n = 0;
    static int      s_flip     = 0;
    static uint16_t s_bucket[MAX_PRIMS];
    static uint16_t s_bhead[NUM_BANDS + 1];

    if (band_index == 0) {
        // Counting sort of primitive indices by first band: counts, prefix,
        // scatter. Stable and O(n).
        uint16_t cnt[NUM_BANDS + 1] = { 0 };
        for (int i = 0; i < n; i++) {
            int fb = prims[i].ymin / BAND_H;
            if (fb < 0) fb = 0;
            if (fb >= NUM_BANDS) fb = NUM_BANDS - 1;
            cnt[fb]++;
        }
        int acc = 0;
        for (int k2 = 0; k2 < NUM_BANDS; k2++) { s_bhead[k2] = (uint16_t)acc; acc += cnt[k2]; cnt[k2] = s_bhead[k2]; }
        s_bhead[NUM_BANDS] = (uint16_t)acc;
        for (int i = 0; i < n; i++) {
            int fb = prims[i].ymin / BAND_H;
            if (fb < 0) fb = 0;
            if (fb >= NUM_BANDS) fb = NUM_BANDS - 1;
            s_bucket[cnt[fb]++] = (uint16_t)i;
        }
        s_active_n = 0;
    }

    // Retire what ended above this band and merge in what starts here, both
    // streams ascending by index.
    {
        const uint16_t* src = s_active[s_flip];
        uint16_t*       dst = s_active[s_flip ^ 1];
        int r = 0, q = (int)s_bhead[band_index], dn = 0;
        const int qe = (int)s_bhead[band_index + 1];
        while (r < s_active_n || q < qe) {
            if (r < s_active_n && prims[src[r]].ymax < by0) { r++; continue; }
            if (q >= qe)                     dst[dn++] = src[r++];
            else if (r >= s_active_n)        dst[dn++] = s_bucket[q++];
            else if (src[r] < s_bucket[q])   dst[dn++] = src[r++];
            else                             dst[dn++] = s_bucket[q++];
        }
        s_flip ^= 1;
        s_active_n = dn;
    }

    const uint16_t* act = s_active[s_flip];
    for (int ai = 0; ai < s_active_n; ai++) {
        const Prim* p = &prims[act[ai]];
        if (p->ymax < by0 || p->ymin > by1) continue;

        const uint32_t c0 = esp_cpu_get_cycle_count();
        switch (p->type) {
        case PRIM_SKY:
            vg_sky_fill_patch(band, by0);
            break;

        case PRIM_CANOPY: {
            // HALF ON EACH CORE, and this is where it pays most: during the primitive
            // phase core 0 has nothing to do at all -- the backdrop is finished and the
            // scanlines have not started -- so the whole pass is being done by one core
            // while the other waits. The rendezvous is the same one the backdrop and
            // the scanlines already use.
            // Split where this band's pixels actually balance, not at its midpoint. A
            // band costs the SLOWER half, so an even-looking split of uneven work
            // returns almost nothing -- measured, the midpoint gave 1.2 of the 1.9 ms
            // it should have. The baker computes the point; it is fifteen bytes.
            const int at = CANOPY_SPLIT[band_index];
            const bool split = rowsplit_start(RS_CANOPY, band, by0, at, BAND_H);
            canopy_rows(band, by0, 0, split ? at : BAND_H);
            if (split) rowsplit_wait();
            break;
        }

        case PRIM_POINT:
            band[(p->y0 - by0) * SCR_W + p->x0] = p->color;
            break;

        case PRIM_LINE: {
            float ax = p->x0, ay = p->y0, bx = p->x1, by = p->y1;
            if (!clip_band_y(&ax, &ay, &bx, &by, (float)by0, (float)by1)) break;

            int ix0 = fast_lrintf(ax), iy0 = fast_lrintf(ay);
            int ix1 = fast_lrintf(bx), iy1 = fast_lrintf(by);
            // Guard against float rounding pushing an endpoint one row out.
            if (iy0 < by0) iy0 = by0; else if (iy0 > by1) iy0 = by1;
            if (iy1 < by0) iy1 = by0; else if (iy1 > by1) iy1 = by1;
            if (ix0 < 0) ix0 = 0; else if (ix0 > SCR_W - 1) ix0 = SCR_W - 1;
            if (ix1 < 0) ix1 = 0; else if (ix1 > SCR_W - 1) ix1 = SCR_W - 1;
            // `aa` is a MODE, not a flag -- see LINE_* in vg_raster_int.h. It rides in
            // what was padding, so the extra modes cost no memory and, more to the
            // point, do not grow Prim past the 20 bytes the join's word copy assumes.
            if (p->aa == LINE_ADD || p->aa == LINE_SUB) {
                band_line_delta(band, by0, by1, ix0, iy0, ix1, iy1,
                                p->color, p->aa == LINE_ADD);
            }
#if VG_LINE_AA
            else if (p->aa == LINE_AA) band_line_aa(band, by0, by1, ix0, iy0, ix1, iy1, p->color);
            else                       band_line_fast(band, by0, by1, ix0, iy0, ix1, iy1, p->color);
#else
            else band_line(band, by0, ix0, iy0, ix1, iy1, p->color);
#endif
            break;
        }

        case PRIM_TRI:
            band_tri(band, by0, by1, p->x0, p->y0, p->x1, p->y1, p->x2, p->y2,
                     p->color, p->aa);
            break;

        case PRIM_FILL: {
            int ry0 = p->y0 > by0 ? p->y0 : by0;
            int rye = p->y0 + p->y1 - 1;
            int ry1 = rye < by1 ? rye : by1;
            for (int y = ry0; y <= ry1; y++) {
                uint16_t* row = &band[(y - by0) * SCR_W + p->x0];
                for (int x = 0; x < p->x1; x++) row[x] = p->color;
            }
            break;
        }

        case PRIM_GLYPH:
            band_glyph(band, by0, by1, p);
            break;
        }
        const uint32_t dc = esp_cpu_get_cycle_count() - c0;
        if      (p->type == PRIM_LINE && p->aa) s_cyc_aa  += dc;
        else if (p->type == PRIM_LINE)          s_cyc_ln  += dc;
        else if (p->type == PRIM_TRI)           s_cyc_tri += dc;
        else if (p->type == PRIM_CANOPY)        s_cyc_can += dc;
        else if (p->type == PRIM_POINT)         s_cyc_pt  += dc;
        else if (p->type == PRIM_GLYPH)         s_cyc_gl  += dc;
        else if (p->type == PRIM_FILL)          s_cyc_fl  += dc;
        else                                    s_cyc_oth += dc;
    }

    s_prim_us += micros() - t_prim;
}

static uint32_t s_raster_us = 0;
static uint32_t s_wait_us   = 0;
static uint32_t s_push_us   = 0;
static int      s_over_n    = 0;
static uint32_t s_over_us   = 0;
static uint32_t s_band_us[NUM_BANDS];
static uint32_t s_join_us   = 0;
static uint32_t s_res_us    = 0;

// One band's time on the wire, which is the window everything above has to fit
// inside: 480 x 32 pixels x 16 bits over four QSPI data lines at 80 MHz is
// 61440 clocks, 768 us. A band that comes in under this costs nothing at all --
// its CPU hid under the previous band's transfer. The amount by which a band
// goes OVER is frame time, and it is the only part of the raster that splitting
// the work across cores can win back.
//
// Derived rather than written down so it follows BAND_H. The 80 is LCD_CLOCK_HZ
// in MHz and the 4 is the QSPI data lines; both live in vg_port_co5300.cpp,
// which is why this is an assumption stated here and not an include.
#define BAND_DMA_US ((SCR_W * BAND_H * 16) / 4 / 80)

uint32_t vg_rast_raster_us(void) { return s_raster_us; }
uint32_t vg_rast_wait_us(void)   { return s_wait_us; }
uint32_t vg_rast_push_us(void)   { return s_push_us; }
int      vg_rast_over_bands(void){ return s_over_n; }
uint32_t vg_rast_over_us(void)   { return s_over_us; }
const uint32_t* vg_rast_band_us(void) { return s_band_us; }
uint32_t vg_rast_join_us(void)   { return s_join_us; }
uint32_t vg_rast_res_us(void)    { return s_res_us; }

void vg_rast_flush(void) {
    // CLOSE THE LIST HERE, in the consumer, not at the end of submit.
    //
    // It was at the end of vg_render_frame, which has an early return for menu
    // states -- so on every menu and attract frame the join never ran, the count
    // stayed zero and nothing but the backdrop drew. Putting it at the top of the
    // flush makes it unreachable-by-omission: whatever submit did or skipped, the
    // thing that reads the list closes it first.
    const uint32_t f0 = micros();
    vg_prim_join();
    s_join_us = micros() - f0;

    // Drain the LAST band of the previous frame before touching its buffer
    // again. Waiting here rather than at the end of the loop means that final
    // transfer overlaps this frame's input and simulation, which are already
    // done by the time we get here.
    const uint32_t w0 = micros();
    vg_crumb(CRUMB_FWAIT, 0);
    vg_panel_wait();
    s_wait_us = micros() - w0;

    uint32_t raster = 0;
    s_push_us = s_over_us = 0;
    s_over_n  = 0;
    s_sky_us = s_prim_us = s_scan_us = 0;
    s_cyc_aa = s_cyc_ln = s_cyc_tri = s_cyc_oth = s_cyc_can = 0;
    s_cyc_pt = s_cyc_gl = s_cyc_fl = 0;
    s_ln_px = s_ln_n = 0;
    s_tint_us = 0;

    const float tint_k = s_tint_k;

    // THE WHOLE FRAME'S CHART, BOTH CORES, BEFORE ANY BAND IS DRAWN.
    //
    // This used to be fifteen separate calls, one at the top of each band, on the drawing
    // core with the helper idle and nothing overlapping it -- 660 us of trig billed to the
    // frame in full. Every chart sample costs two divides and a square root to normalise a
    // ray plus an arctangent and an arcsine on top, and the ESP32-S3 has no single
    // instruction for a divide or a root.
    //
    // The bands are independent: each re-chains its lift from the frame accumulator rather
    // than from the band before it, so there is no order to preserve and the set can simply
    // be cut in half. Bit for bit the same charts, because each band's own samples are
    // still built in the same sequence.
    if (vg_sky_ready()) {
        const uint32_t p0 = micros();
        vg_sky_prep_begin();          // the frame's shared bank trig, once, before the fork
        const bool split = rowsplit_start(RS_PREP, nullptr, 0, NUM_BANDS / 2, NUM_BANDS);
        vg_sky_prep_bands(0, split ? NUM_BANDS / 2 : NUM_BANDS);
        if (split) rowsplit_wait();
        // Charged to `sky`, which is where a reader looks for the cost of the backdrop --
        // AND to `raster`, because it is CPU spent drawing and the telemetry's invariant is
        // that rast is sky plus prim plus scan. Adding it to one and not the other put it in
        // `res` as well, which read as 455 us of new preemption that did not exist.
        const uint32_t pd = micros() - p0;
        s_sky_us += pd;
        raster   += pd;
    }

    for (int b = 0; b < NUM_BANDS; b++) {
        uint16_t* buf = s_band[b & 1];

        // Timed WITHOUT the push: vg_panel_push_band blocks on the previous
        // transfer, so folding it in here would charge us for the DMA we are
        // trying to hide under. What this counter measures is the CPU work that
        // has to fit inside the transfer window, which is the number that
        // actually decides the frame rate.
        const uint32_t r0 = micros();
        vg_crumb(CRUMB_FDRAW, (uint8_t)b);
        draw_band(b, buf);
        // Over the finished band, so the backdrop and the instruments drawn on
        // top of it are striped alike. Baking it into the backdrop texture
        // instead silently skipped every vector element.
        const uint32_t t_scan = micros();
        vg_crumb(CRUMB_FSCAN, (uint8_t)b);
        {
            const bool split = rowsplit_start(RS_SCAN, buf, b * BAND_H,
                                              ROW_SPLIT, BAND_H);
            band_scanlines(buf, b * BAND_H, 0, split ? ROW_SPLIT : BAND_H);
            if (split) rowsplit_wait();
        }
        // After the scanlines, so the tint colours those too. A red warning that
        // left the scanlines amber would read as an overlay rather than as the
        // whole picture going red.
        // Tint happens at the source now -- in the sky fill and at submit --
        // so there is nothing to do here. tint_k keeps the variable alive for
        // the band_tv gate below. (void)band_wall_tint quiets the compiler; the
        // function stays as the reference for the ring geometry.
        (void)tint_k;
        // Last of all. The set turning off takes the whole picture with it --
        // scanlines, tint, instruments and all -- because it is the display
        // going away rather than another layer drawn on top of it.
        if (vg_rast_tv_active()) band_tv(buf, b * BAND_H);
        s_scan_us += micros() - t_scan;
        const uint32_t dr = micros() - r0;
        raster += dr;
        s_band_us[b] = dr;
        if (dr > BAND_DMA_US) { s_over_n++; s_over_us += dr - BAND_DMA_US; }

        // Captured AFTER the scanline pass and BEFORE the blit, so the recording
        // is exactly the bytes the panel receives -- effects included, and with
        // no chance of catching a buffer mid-redraw. Reads the band without
        // touching it, so the transfer that follows is unaffected.
        if (b == 0) vg_capture_frame_begin();
        vg_capture_band(b * BAND_H, BAND_H, buf);

        // Queues and returns: the next iteration rasterises into the other
        // buffer while this one is on the wire.
        //
        // TIMED, because "queues and returns" is only true when the previous
        // transfer has finished. The push drains it first, so the time spent in
        // here is the CPU standing still with nothing left to do -- the frame
        // being panel-bound rather than compute-bound. It was the one piece of
        // the flush nobody had a number for, and blit minus wait minus rast
        // minus push is now zero by construction.
        vg_crumb(CRUMB_FPUSH, (uint8_t)b);
        const uint32_t p0 = micros();
        vg_panel_push_band(b * BAND_H, BAND_H, buf);
        s_push_us += micros() - p0;
    }

    vg_capture_frame_end();
    s_raster_us = raster;

    // What is left of the flush once the four brackets are subtracted: the
    // micros() calls themselves, the crumbs, the capture hooks' early returns --
    // and any time this thread was PREEMPTED between brackets, which is the only
    // item here that can be large. Named because it measured 416-510 us in
    // combat against 100-130 in attract, and an unattributed number that moves
    // with load is a lead, not rounding.
    const uint32_t all = micros() - f0;
    const uint32_t acc = s_wait_us + raster + s_push_us + s_join_us;
    s_res_us = (all > acc) ? all - acc : 0;
}
