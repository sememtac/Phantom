#include "vg_raster.h"
#include "vg_crumb.h"
#include "vg_raster_int.h"
#include "vg_font.h"
#include "vg_port.h"
#include "vg_capture.h"
#include "vg_sky.h"
#include "vg_canopy.h"
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <string.h>
#include <math.h>

// The raster half. Everything here runs NUM_BANDS times per frame but is hidden
// under the panel DMA, so work moved into this file is close to free -- right up
// until the per-band total exceeds the 0.768 ms DMA window, at which point it
// starts costing frame time directly. That threshold is the whole reason the
// backdrop fill and the scanline pass are written the way they are.

// THREE BAND BUFFERS, and the third one is what lets the wire stay fed.
//
// Two was one drawn into while one was on the wire, and it forced the driver to drain
// before every band -- so the wire went dark for a task wakeup fifteen times a frame.
// Keeping a band QUEUED behind the one in flight closes those gaps, and that needs
// three: two are outstanding and the rasteriser needs a third to work in.
//
// 30 KB of internal SRAM, which is the scarce memory on this part. Bought against 590
// us of measured wire idle, on every frame, whatever the scene is doing.

#define BAND_BUFS 3
static uint16_t* s_band[BAND_BUFS] = { nullptr, nullptr, nullptr };

int vg_band_bufs(void) { return BAND_BUFS; }

bool vg_band_init(void) {
    // Must be internal and DMA-capable: written pixel-by-pixel, then handed
    // straight to the SPI engine.

    for (int i = 0; i < BAND_BUFS; i++) {
        s_band[i] = (uint16_t*)heap_caps_malloc(SCR_W * BAND_H * 2,
                                               MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
        if (!s_band[i]) {
            Serial.printf("vg_band_init: alloc failed (buffer %d of %d)\n", i, BAND_BUFS);
            return false;
        }
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

// ALWAYS_INLINE, AND THE BENCH AT THE BOTTOM OF THIS FILE IS WHY.
//
// This had exactly one caller and GCC inlined it into the flush path for free. Giving the
// blend bench a second call site was enough to flip that heuristic: the function came out
// of line, and vg_rast_flush lost 198 bytes to a call the frame never used to make.
//
// A copy for the bench to call instead does NOT fix it -- -fipa-icf folds a byte-identical
// copy straight back into this one and outlines the merged result, which is how that was
// found. So the property is pinned here rather than left to a heuristic that a change
// somewhere else can move. It is what the compiler was already doing.
//
// Checked by symbol diff against HEAD, not by eye: vg_rast_flush back to 0x1715, and no
// standalone band_line_delta.
static __attribute__((always_inline)) inline
void band_line_delta(uint16_t* band, int by0, int by1,
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

// Per ring, from the faint inner edge out to the rim. The mask CLEARS bits and
// the glow is a red value out of 31, pre-shifted into the swapped red field.
//
// The red ramp starts at zero on purpose: the innermost ring changes nothing at
// all, so the gradient fades out instead of ending on an edge.
// Pre-paired, so the hot loop does not build them. Each entry is the mask and the
// glow for TWO pixels at once. Recomputing these inside the span function cost
// six operations per call, and there are twelve rings on both sides of 480 rows.
// Shift applied to green's HIGH three bits, which are contiguous at 15..13 in
// the swapped word. This is what gives amber intermediate levels: masks alone
// could only take its green from 21 straight to 0, because 010101 has no bits
// left to remove in between.

// Tint [x0,x1) of one row. Unrolled four words at a time, which is what the
// scanline pass above learned: at five operations of real work per word, the loop
// itself was most of the cost.

// THE WALL WARNING WAS A FULL-SCREEN RED RING AND IT IS GONE. What it cost, measured, is
// the argument against ever putting one back:
//
//   the backdrop fill      1700 us clear, 5251 us with the ring on
//   at the boundary        53 fps against 59-60 with the cockpit doing the job instead
//   per primitive          every line, point, rect, triangle and glyph of every frame
//                          asked whether the player was near a wall, wall or no wall
//
// Two rounds of optimisation went into it and both worked: the ring radii were hoisted to
// once a frame from once a row, and the thirteen square roots a row became a 241 x 13 table
// in PSRAM, which took 5251 us to 4188. It was still the most expensive thing in the frame,
// because the cost was never the arithmetic -- it was writing to 153,600 pixels that were
// already going to be written once.
//
// A cockpit member is already being drawn. Colouring it differently costs nothing per pixel,
// which no amount of work on a second full-screen pass can match. See vg_canopy_alarm.
//
// It existed because it had to: it predates canopies, and with no cockpit to light up,
// tinting the whole view was the only way to say "wall" at all.

// HOW RED THE COCKPIT IS, 0 clear and 1 hard against the wall. The only wall warning there
// is now: the ring that used to sit beside this is deleted, above.
static float s_alarm  = 0.0f;


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

// Ring crossings for one panel row: lim[i] is the half-width where ring i
// begins. Geometry identical to the dead pass.


// One primitive, tinted by the ring under its centre. A long line crosses
// several rings and gets its centre's -- a visible simplification nobody will
// study during a boundary alarm.






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
    // KEEP IT ONLY IF IT IS IN RANGE, rather than rejecting what looks wrong. A NaN
    // fails every test written the obvious way -- (v < 0) and (v > 1) are both false for
    // it -- so a NaN would sail through a rejection and land in the state. The wall
    // warning's own setter used the same shape before it was deleted.
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
// A DEAD COUNTER FOR AS LONG AS THE TINT HAS BEEN DRAWN AT THE SOURCE.
//
// s_tint_us was declared, reset and read, and nothing ever wrote to it, so `tnt` reported
// 0 for a warning that costs over a millisecond. It cost real time to trust: it was quoted
// three times as evidence the boundary effect was cheap, and then as evidence it was
// expensive, and the number behind both was never measured at all.
//
// THE COST IS NOT SEPARABLE ANY MORE, which is why this returns a LEVEL and not a time.
// The tint moved into the sky fill and into submit, and then out of both --
// so its work is interleaved with the fill it colours, and bracketing it would mean a
// second pass to measure the first. What it costs is visible directly in `sky`, measured:
// 1792 us with the warning off and 3028 with it on, up to 3445 hard against the wall.
//
// So `tnt` reports HOW HARD THE WARNING IS RUNNING, 0 to 100. That is the number that was
// actually missing: without it there is no way to tell which telemetry windows had the
// effect active, so an average over a flight buries it -- which is exactly how a 1240 us
// cost and six frames a second were measured as "no change".
// HOW HARD THE WALL WARNING IS RUNNING, 0 to 100. Not microseconds, and it has been three
// different wrong things.
//
// It was a dead counter for as long as the tint was drawn at the source: s_tint_us was
// declared, reset and read and nothing ever wrote to it, so `tnt` reported 0 for an effect
// costing over a millisecond. Then it was pointed at s_tint_k -- in the same commit that
// switched the ring off by handing vg_rast_tint a zero -- so it faithfully reported that
// zero while the cockpit was visibly red on the panel. Two flights each time.
//
// The level is what was actually needed. Without it there is no way to tell which telemetry
// windows had the warning up, so an average over a flight buries the effect entirely -- which
// is how 1240 us and six frames a second were once measured as "no change".
uint32_t vg_rast_tint_us(void) {
    return (uint32_t)(s_alarm * 100.0f + 0.5f);
}

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

// WHICH DRAWING IS BEING FLOWN.
//
// It used to be the CANOPY_* macros, read straight out of the generated header, and that
// shape allowed exactly one cockpit to exist -- a macro cannot be selected at runtime. Every
// hull can have its own now. Nothing is owned or copied: a canopy lives in flash and this
// points at it.
//
// NULL UNTIL SELECTED, and this file no longer includes a drawing at all.
//
// It cannot: a generated header defines its tables as `static`, so including one here and
// again wherever the per-hull table lives would put two copies of every drawing in flash.
// vg_canopy_set.cpp owns them; this holds a pointer it is handed.
//
// Which means every entry point has to tolerate not having one yet. vg_game_init selects at
// boot so it never happens in practice, but "never happens in practice" is not something to
// dereference a pointer on -- and a missing cockpit should be a missing cockpit, not a crash.
static const VgCanopy* s_can = nullptr;

// IS THE ARRIVAL SEQUENCE RUNNING. Declared HERE, beside the drawing, and not down with the
// rest of the intro's state where it used to live -- because the two are coupled and the
// coupling is a safety property: canopy_intro_step reads s_can->zones under this flag alone,
// so a true flag and a null drawing is a fault. vg_canopy_use is the only place that can
// break that pair, and it is a few lines below.
static bool s_intro_on = false;

// LOOKING AFT, so the cockpit must not be drawn.
//
// The canopy is the front of the ship. Draw it over a view out of the back and the picture
// says two things at once: the frame claims you are looking forward while everything inside
// it is behind you. From a playtest, and it is a continuity fault rather than a cosmetic one.
//
// It suppresses the FRAME ONLY, not the intro's world gate. Those share one primitive, and
// the gate is what holds the world black region by region while the cockpit comes online --
// so switching the whole primitive off in rear view would show the entire world at full
// brightness in the middle of the sequence. That is the same trap that lifting this
// primitive out of vg_draw_hud was fixing.
static bool s_can_rear = false;

void vg_canopy_rear(bool on) { s_can_rear = on; }

const VgCanopy* vg_canopy_current(void) { return s_can; }

// THE WALL WARNING, ON THE COCKPIT INSTEAD OF ON THE VIEW.
//
// The ring tint is a per-pixel pass over its own area and costs about 1100 us at the wall,
// measured. This costs NOTHING per pixel: the cockpit's members are already written every
// frame, and their colour comes from a 256-entry table, so turning the frame red is a
// different table rather than more work.
//
// Quantised to sixteen steps so a steady approach rebuilds the table a few times instead
// of sixty a second. The table is 256 entries of trivial arithmetic, so even that is
// nearly free -- the quantising is about not thrashing s_can_ready.
// s_alarm is declared further up, where the ring's own level used to sit beside it.
static int   s_alarm_q = 0;
static bool  s_alarm_white = false;

// TWO THINGS, because they are two different signals and the first attempt conflated them.
//
// `k` is how red the frame is, from the clearance. `white` is a STROBE: the frame goes to
// white for a fraction of a second and comes back. The author's words, after flying the
// first version and not seeing it: "the flashing i have in mind is not the canopy flashing
// its opacity, rather, it's the color flashing from split second of white to the current
// tint".
//
// That is why the first attempt was invisible. It swung the red LEVEL between a floor and
// full, and vg_mix(COL_HUD, COL_DANGER, k) cannot reach white at either end -- so the whole
// effect was a slight change of saturation on an already-red frame, which is precisely the
// "which shade means I am dead" problem it was meant to solve.
//
// The strobe is a bool and the level is quantised, so the table rebuilds on either edge of
// a flash and on a step of the ramp. At the top rate that is about eighteen rebuilds a
// second of 256 trivial entries, which is nothing beside a per-pixel pass.
void vg_canopy_alarm(float k, bool white) {
    if (k < 0.0f) k = 0.0f; else if (k > 1.0f) k = 1.0f;
    const int q = (int)(k * 16.0f + 0.5f);
    if (q == s_alarm_q && white == s_alarm_white) return;
    s_alarm_q     = q;
    s_alarm       = (float)q * (1.0f / 16.0f);
    s_alarm_white = white;
    s_can_ready   = false;        // the colour table is stale; canopy_lut rebuilds it
}

static void canopy_lut(void) {
    const int bg = (int)s_can->bg;
    // What the frame is made of right now: its own amber, pulled toward the danger red as
    // the wall closes. One mix for the whole table rather than per pixel.
    // The frame's own amber, pulled toward the danger red by the clearance, and then all
    // the way to white for the length of a strobe. One mix for the whole table rather than
    // anything per pixel.
    uint16_t base = (s_alarm > 0.0f) ? vg_mix(COL_HUD, COL_DANGER, s_alarm) : COL_HUD;
    if (s_alarm_white) base = CANOPY_ALARM_WHITE;
    for (int g = 0; g < 256; g++) {
        const float f = (g > bg)
                      ? (float)(g - bg) / (float)(255 - bg)
                      : (float)(bg - g) / (float)bg;
        // NATIVE order, swapped once here. Palette colours are stored in panel order
        // and blend_px works in native, so leaving this unswapped would put the delta's
        // red into the blue channel -- a bug that would have looked like a design
        // decision rather than a mistake.
        const uint16_t c = vg_dim(base, f);
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
// TWO MORE ATTEMPTS HAVE SINCE BEEN MEASURED AND REJECTED, both over the same recorded
// session with tools/replay_cost.py, whose noise floor on this counter is one microsecond:
//
//   The byte swaps lifted out of px_add and done once per PAIR instead of once per pixel.
//   Seven operations of about twenty-two are swapping, so this looked like the obvious
//   win. It cost 32 us, +1.1%. The compiler was already folding the swap into the field
//   masks better than the hand-written version could.
//
//   The whole flight table copied from flash into internal SRAM, on the theory that 11 KB
//   read through the cache every frame was the missing time. It saved 3 us, -0.1%, for
//   11 KB of the scarcest memory on the part.
//
// So the 36 cycles a flat pixel costs are neither the arithmetic nor the table: they are
// the read-modify-write of the band itself. That is the floor, and the only way under it
// is to touch fewer pixels. AREA is the lever, and it belongs to the artist, not to this
// file -- which is what the estimate in tools/canopy_bake.py has always said. It now has
// three measurements behind it instead of an assertion.
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

// ALWAYS INLINE, and not as a matter of taste.
//
// These were inlined by choice of the compiler, and taking them OUT of line was measured at
// 175 us of loss when it was tried on purpose -- a flat block averages twenty pixels, which is
// not enough to pay for a call. Then the warp grew canopy_rows_t past whatever size threshold
// GCC weighs, and it stopped inlining them on its own: the rigid instantiation came out at 234
// bytes calling span_sub, and the pass went 4065 -> 4190 us.
//
// So the decision is stated rather than left to a heuristic that cannot know what it costs
// here. A measurement that has already been taken should not have to be taken again because an
// unrelated function got longer.
static inline __attribute__((always_inline))
void span_add(uint16_t* q, int n, uint16_t d) { SPAN_BODY(px_add) }
static inline __attribute__((always_inline))
void span_sub(uint16_t* q, int n, uint16_t d) { SPAN_BODY(px_sub) }

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
        const uint32_t d = lut[lv[0]];                                       \
        q[0] = (uint16_t)FN(q[0], d & 0xF81Fu, d & 0x07E0u);                 \
        i = 1;                                                               \
    }                                                                        \
    for (; i + 1 < n; i += 2) {                                              \
        const uint32_t d0 = lut[lv[i]], d1 = lut[lv[i + 1]];                 \
        uint32_t* w = (uint32_t*)(void*)(q + i);                             \
        const uint32_t v = *w;                                               \
        *w = FN(v & 0xFFFFu, d0 & 0xF81Fu, d0 & 0x07E0u)                     \
           | (FN(v >> 16, d1 & 0xF81Fu, d1 & 0x07E0u) << 16);                \
    }                                                                        \
    for (; i < n; i++) {                                                     \
        const uint32_t d = lut[lv[i]];                                       \
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
// A STRETCHED literal block, resampled rather than over-read.
//
// A literal block carries one level byte per DRAWING pixel. Magnified, its panel length is
// longer than its data, and the plain span above would read past the end of its own levels
// into the next block's -- which are frequently much brighter. That is the bright pixels
// bleeding through the frame at full zoom: not a blend fault, an array read one block too far.
//
// Fixed point, 16.16: `step` is drawing pixels per panel pixel and `i0` is where the clipped
// front starts. Only the graded edges take this path and only while the warp is on, so it
// stays a plain per-pixel walk -- pairing it would save little and this is 4,792 pixels.
#define SPAN_LIT_RS_BODY(FN)                                                     uint32_t idx = i0;                                                           for (int i = 0; i < n; i++) {                                                    const uint32_t d = lut[lv[idx >> 16]];                                       q[i] = (uint16_t)FN(q[i], d & 0xF81Fu, d & 0x07E0u);                         idx += step;                                                             }

// THE TABLE COMES IN AS AN ARGUMENT, because the intro needs a different one per zone.
//
// It was a file static, read straight from the macro bodies. The intro flashes a region white
// by giving that region its own colour table, so the table has to vary per BLOCK -- and these
// are shared with the flight path, which must not pay for it. As an argument it does not: the
// call already passes three registers and the loop reads through a register base rather than a
// literal address, which on Xtensa is no worse. Measured on the canopy bench before and after.
static __attribute__((noinline))
void span_lit_add_rs(uint16_t* q, int n, const uint8_t* lv, const uint16_t* lut, uint32_t i0, uint32_t step)
{ SPAN_LIT_RS_BODY(px_add) }
static __attribute__((noinline))
void span_lit_sub_rs(uint16_t* q, int n, const uint8_t* lv, const uint16_t* lut, uint32_t i0, uint32_t step)
{ SPAN_LIT_RS_BODY(px_sub) }

static __attribute__((noinline)) void span_lit_add(uint16_t* q, int n, const uint8_t* lv, const uint16_t* lut) { SPAN_LIT_BODY(px_add) }
static __attribute__((noinline)) void span_lit_sub(uint16_t* q, int n, const uint8_t* lv, const uint16_t* lut) { SPAN_LIT_BODY(px_sub) }

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
// THE SPHERE, the same one the instruments are drawn on.
//
// A radial warp is NOT separable: k = 1 + K*scale*r2 scales both axes by an amount that
// depends on both coordinates, so a y-map plus a column offset -- which is what this was --
// can only ever produce a shear and a zoom. It read as the frame growing and bending rather
// than bulging.
//
// Done properly in two halves, because the renderer iterates panel ROWS and can only lay runs
// along panel x within one:
//
//   the y axis is EXACT. dx is fixed for the whole column, so dx*dx hoists and each block
//   endpoint costs a subtract, two multiplies, two adds and a round -- about seven operations.
//   Both endpoints go through it and the length is the difference, so runs still touch.
//
//   the x axis is an inverse. For each output row, which drawing column lands here -- built by
//   walking the forward map, which is monotone for any |K| the panel uses. Evaluated at the
//   screen's own centre line, so a point's sideways displacement is taken as constant down its
//   column. That is the one approximation, and it is the same one the rear-view patch makes.
//
// Rebuilt only when the quantised amount changes, so the 480 forward evaluations and the
// inversion are paid a handful of times across a throttle sweep, not per frame.
static float   s_w_zk   = 0.0f;    // zoom * K * scale / R2
static float   s_w_zoom = 1.0f;
static float   s_w_zbase[SCR_H];   // zoom + zoom*K*scale*dx^2/R2, per SOURCE column
static int16_t s_wy[SCR_W + 1];
static int16_t s_wc[SCR_H];
// WHICH DRAWING COLUMN A PANEL ROW SAMPLES, which is the other half of coming closer.
//
// Stretching the drawing's y alone made the frame taller, not nearer. A magnification needs
// both axes, and the other axis IS the column index -- so it is a lookup per panel row, 480 a
// frame, and free. Inverse-mapped because this samples rather than places: a frame magnified
// by s puts drawing column d at panel position cx + (d-cx)*s, so the panel row at p wants
// cx + (p-cx)/s.
//
// Nearest neighbour, so at a magnification of a tenth roughly every tenth column is read
// twice. On a cockpit frame that reads as the members thickening slightly, which is what
// something approaching does anyway. -1 means this row is off the drawing entirely.
static int16_t s_wcol[SCR_H];

// WHERE A WARPED BAND BALANCES, which the baked table cannot know.
//
// VgCanopy::split is computed by the baker from where the drawing's work falls, and it is right
// only while each panel row reads its own column. Warping breaks that: the inverse column map
// duplicates some columns and skips others, so the work slides along the band and the baked
// balance point stops being the middle of it. A band costs whichever half is slower, so the
// error is paid in full and it grows with ZOOM -- which is the setting the look depends on.
//
// Rebuilt with the maps, from the same per-column costs the baker used. s_colcost is walked
// out of the table once, on the first warp.
static uint8_t  s_wsplit[NUM_BANDS];
static uint16_t s_colcost[SCR_H];
static bool     s_colcost_ready = false;

static void canopy_colcost(void) {
    for (int c = 0; c < SCR_H; c++) {
        uint32_t cost = 0;
        if (c < (int)s_can->cols) {
            const uint8_t* p = &s_can->data[s_can->ofs[c]];
            const uint8_t* e = &s_can->data[s_can->ofs[c + 1]];
            while (p + 3 <= e) {
                const uint8_t h = p[0];
                const int len = ((h & 2) << 7) | p[2];
                p += 3 + ((h & 0x80) ? len : 1);
                cost += (uint32_t)len + 1;      // a header is worth about one pixel
            }
        }
        s_colcost[c] = (uint16_t)(cost > 0xFFFFu ? 0xFFFFu : cost);
    }
    s_colcost_ready = true;
}
static int     s_wq = -1;                 // the quantised amount the maps were built for
static bool    s_warp_on = false;

// SELECTING A DRAWING, and it is down here rather than beside s_can because of what it has to
// invalidate: three caches, two of which are the warp's and are declared just above.
//
// All three are derived from the drawing and none of them can survive a change of it. The
// colour table is built from `bg`, which is per drawing -- two cockpits with different
// background levels turn the same stored grey into different amounts of light, so a stale
// table draws the new one at the wrong brightness. The per-column costs and the warp maps are
// built from where the drawing's work falls, which is the whole point of them.
// NULL IS A HULL WITH NO COCKPIT, and that is a supported state rather than a mistake.
//
// This used to ignore null and keep whatever was selected last, on the reasoning that a wrong
// cockpit is something an artist can see where an empty frame just looks broken. That was
// written when a default drawing was assumed. There is no default: a canopy is authored per
// hull, the reference drawing belongs to the CHARIOT, and the other three fly without one
// until somebody draws them.
//
// Everything downstream already tolerates it -- canopy_rows, the warp, the bench and the
// PRIM_CANOPY case all return early on a null drawing -- so what was missing was only the
// ability to SAY none.
void vg_canopy_use(const VgCanopy* c) {
    if (c == s_can) return;
    s_can           = c;
    s_can_ready     = false;
    s_colcost_ready = false;
    s_wq            = -1;
    // A SEQUENCE CANNOT BE RUNNING AGAINST A DRAWING THAT IS GONE.
    //
    // canopy_intro_step reads s_can->zones and is guarded only by s_intro_on, so leaving the
    // flag set while selecting nullptr arms exactly the fault that vg_canopy_intro_reset just
    // had to be fixed for. No caller does that today, and no caller should have to know not to.
    if (!c) s_intro_on = false;
}

// THE FRAME LAGS THE SHIP, which is what makes it read as being inside something.
//
// A pixel offset from the DIFFERENCE between the turn now and a smoothed copy of it, not from
// the turn itself. Displacement then appears while the turn is starting and stopping and
// settles back to nothing through a steady one -- inertia rather than a permanent lean, which
// would just look like the drawing is off centre.
//
// Whole pixels, so it quantises itself like everything else here, and it moves the COLUMN the
// row samples: under the quarter turn a bank is movement along logical x, and logical x is the
// drawing's column. One index add per row.
// ALL THREE AXES, and each is a different shape of offset.
//
// Under the quarter turn the drawing's x is a panel COLUMN and its y is a panel row's x, so:
//
//   yaw   moves the frame along logical x  -> the column each row samples. One index add.
//   pitch moves it along logical y         -> every block's start. One add, per column.
//   roll  is a rotation, which no single offset can be -- but a small rotation about the
//         centre is a SHEAR, and a shear is a per-column offset that grows with the column.
//         So it is the same add as pitch with a term in c. One multiply per column, none per
//         block and none per pixel.
//
// Each rides the difference between the command and a smoothed copy of it, so the frame swings
// as a movement starts and stops and sits still through a held one. A canopy that stayed
// displaced through a sustained roll would be a canopy coming loose from the ship.
// A MASS ON A SPRING, not a difference.
//
// It used to be (command - smoothed) * gain. That eases on the way IN and cannot ease on the
// way out: lifting a finger drops the command to zero in one frame, the smoothed copy has not
// moved, so the difference -- and the frame with it -- jumps to its extreme instantly. Reported
// as the canopy snapping back and the ship losing its weight, which is exactly what a
// first-order difference does. The offset was a function of the instantaneous command, so any
// step in the command was a step in the picture.
//
// Now the displacement is STATE. The command's rate of change drives it, a spring pulls it
// home and damping settles it, so nothing the player does moves the frame in one frame -- only
// its acceleration changes. Releasing the stick gives an equal and opposite kick and the frame
// coasts back at the spring's own pace, overshooting slightly, which is what weight looks like.
//
// SPRING sets how long the return takes -- 0.030 is a period of about 36 frames, six tenths of
// a second. DAMP sets how much it overshoots: this pair is a damping ratio near 0.64, so it
// comes back with one soft rebound rather than ringing or arriving dead.
static float s_lag_q[3]  = { 0.0f, 0.0f, 0.0f };   // last command, per axis
static float s_lag_v[3]  = { 0.0f, 0.0f, 0.0f };   // velocity
static float s_lag_x[3]  = { 0.0f, 0.0f, 0.0f };   // displacement
static int   s_lag_px = 0;         // columns, from yaw
static int   s_lag_py = 0;         // pixels along the drawing's y, from pitch
static float s_lag_sh = 0.0f;      // pixels of y per column, from roll

void vg_canopy_lag(float yaw, float pitch, float roll, float scale) {
    const float cmd[3] = { yaw, pitch, roll };
    float out[3];
    for (int i = 0; i < 3; i++) {
        const float a = cmd[i] - s_lag_q[i];       // the ship's angular acceleration, near enough
        s_lag_q[i] = cmd[i];
        s_lag_v[i] += a * CANOPY_LAG_DRIVE
                    - s_lag_x[i] * CANOPY_LAG_SPRING
                    - s_lag_v[i] * CANOPY_LAG_DAMP;
        s_lag_x[i] += s_lag_v[i];
        out[i] = s_lag_x[i];
    }

    // The CLAMP scales with the airframe too. Scaling only the swing would have every hull
    // reach the same limit and arrive there at a different speed, which flattens exactly the
    // difference this is for -- a CHARIOT would clip where a BALLISTA never gets close.
    const float lim  = CANOPY_LAG_MAX * scale;
    const float gain = CANOPY_LAG_PX * scale;

    float dx = out[0] * gain;
    float dy = out[1] * gain;
    float dr = out[2] * gain * CANOPY_LAG_ROLL;
    if (dx >  lim) dx =  lim; else if (dx < -lim) dx = -lim;
    if (dy >  lim) dy =  lim; else if (dy < -lim) dy = -lim;
    if (dr >  lim) dr =  lim; else if (dr < -lim) dr = -lim;

    s_lag_px = (int)(dx + (dx < 0.0f ? -0.5f : 0.5f));
    s_lag_py = (int)(dy + (dy < 0.0f ? -0.5f : 0.5f));
    // Pixels of y at the frame's edge, spread linearly across the columns.
    s_lag_sh = dr / (float)(SCR_H / 2);
    if (s_lag_px || s_lag_py || s_lag_sh != 0.0f) s_warp_on = true;
}

// The sequence's own clock. Declared here because vg_canopy_warp is where the flex it
// suppresses is set, and defined below with the rest of the intro.
static float s_settle_t = -1.0f;          // < 0 means not settling

void vg_canopy_warp(float k) {
    if (!s_can) return;                    // the maps are built from the drawing
    if (k < 0.0f) k = 0.0f;
    if (k > 1.0f) k = 1.0f;
    const int q = (int)(k * CANOPY_WARP_STEPS + 0.5f);
    s_warp_on = (q != 0) || s_lag_px || s_lag_py || (s_lag_sh != 0.0f);
    if (q == s_wq) return;
    s_wq = q;

    const float a    = (float)q / (float)CANOPY_WARP_STEPS;
    const float zoom = 1.0f + CANOPY_WARP_ZOOM * a;
    const float R2   = (float)SCR_CX * SCR_CX + (float)SCR_CY * SCR_CY;
    const float krn  = HUD_WARP_K * CANOPY_WARP_SPHERE * a / R2;
    s_w_zoom = zoom;
    s_w_zk   = zoom * krn;

    // The forward map along x at the centre line, then inverted. Monotone, so one walk.
    static int16_t fwd[SCR_H];
    for (int x = 0; x < SCR_H; x++) {
        const float dx = (float)x - (float)SCR_CX;
        fwd[x] = (int16_t)(int)((float)SCR_CX + dx * zoom * (1.0f + krn * dx * dx) + 0.5f);
        // ...and what a block endpoint in this column will need, less its dy terms.
        s_w_zbase[x] = zoom + s_w_zk * dx * dx;
    }
    {
        int x = 0;
        for (int xp = 0; xp < SCR_H; xp++) {
            while (x < SCR_H - 1 && fwd[x] < xp) x++;
            int best = x;
            if (x > 0 && (xp - fwd[x - 1]) < (fwd[x] - xp)) best = x - 1;
            s_wcol[xp] = (int16_t)best;
        }
    }
    // And where each band now balances, from the columns the rows actually read.
    if (!s_colcost_ready) canopy_colcost();
    for (int b = 0; b < NUM_BANDS; b++) {
        uint32_t rowc[BAND_H], total = 0;
        for (int r = 0; r < BAND_H; r++) {
            const int lx = SCR_H - 1 - (b * BAND_H + r);
            const int sc = (lx >= 0 && lx < SCR_H) ? s_wcol[lx] : -1;
            rowc[r] = (sc >= 0) ? s_colcost[sc] : 0u;
            total  += rowc[r];
        }
        int at = ROW_SPLIT;
        if (total) {
            uint32_t run = 0, best = 0xFFFFFFFFu;
            for (int r = 1; r < BAND_H; r++) {
                run += rowc[r - 1];
                const uint32_t gap = (run > total - run) ? run - (total - run)
                                                        : (total - run) - run;
                if (gap < best) { best = gap; at = r; }
            }
        }
        s_wsplit[b] = (uint8_t)at;
    }

    // The bow, on top of the sphere, for an author who wants more bend than the panel has.
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
// One endpoint onto the sphere. `zbase` carries the column's dx term; only dy is left.
static inline int warp_y(int y, float zbase) {
    const float dy = (float)y - (float)SCR_CY;
    return (int)((float)SCR_CY + dy * (zbase + s_w_zk * dy * dy) + 0.5f);
}

// THE TWO EDGES, on their own, so the block walk does not carry them.
//
// Only the FIRST and LAST block of a column can touch a border, so this finds those two and
// extends them -- and by living out here it takes its state, its level lookups and its extra
// branches out of canopy_rows_t. That matters more than the walk it repeats: carrying them
// inline grew the rigid instantiation to 1185 bytes with fifty stack accesses, and cost 67 us
// on a path where none of this code even runs.
//
// The extensions never overlap the blocks they come from -- [0, at) and [end, SCR_W) sit
// outside them -- so it does not matter that this runs before the walk rather than around it.
//
// Extended only where the drawing reached the border. Background beyond a member is not
// something to stretch.
static __attribute__((noinline))
void canopy_edges(uint16_t* row, const uint8_t* p, const uint8_t* e, int wofs, float zbase) {
    if (p + 3 > e) return;

    // The near edge, from the first block.
    {
        const uint8_t h  = p[0];
        const int     y0 = ((h & 1) << 8) | p[1];
        if (y0 == 0) {
            int at = warp_y(0, zbase) + wofs;
            if (at > SCR_W) at = SCR_W;
            if (at > 0) {
                const uint16_t d = s_can_lut[p[3]];
                if (h & 0x40) span_sub(&row[0], at, d);
                else          span_add(&row[0], at, d);
            }
        }
    }

    // The far edge, from the last -- which has to be walked to.
    const uint8_t* last = nullptr;
    while (p + 3 <= e) {
        last = p;
        const uint8_t h = p[0];
        const int len = ((h & 2) << 7) | p[2];
        p += 3 + ((h & 0x80) ? len : 1);
    }
    if (!last) return;
    const uint8_t h   = last[0];
    const int     y0  = ((h & 1) << 8) | last[1];
    const int     len = ((h & 2) << 7) | last[2];
    if (y0 + len < SCR_W) return;                 // it stopped short of the border
    int end = warp_y(y0 + len, zbase) + wofs;
    if (end < 0) end = 0;
    if (end >= SCR_W) return;
    const uint16_t d = s_can_lut[(h & 0x80) ? last[3 + len - 1] : last[3]];
    if (h & 0x40) span_sub(&row[end], SCR_W - end, d);
    else          span_add(&row[end], SCR_W - end, d);
}

// ===========================================================================
// THE CANOPY COMING ONLINE
//
// The match opens black with the instruments already lit, and the view arrives a region at a
// time. Per region, keyed off the ZONE the artist painted into the green channel:
//
//   the whole region flashes WHITE -- every pixel of it, not just the frame's members,
//   the world then dissolves in out of that white,
//   and the frame's blocks in that zone begin drawing at their authored level.
//
// THE GREEN SHAPE IS THE FLASH. That is the author's own word for it -- "the shapes act as
// mask" -- and the first version got it wrong: it flashed the frame's BLOCKS, which is the
// tenth of the region the cockpit actually covers, so a region lighting up read as a few
// members brightening instead of a piece of the view coming on.
//
// It also produced the overblown colour the author saw. Flashing the blocks meant lerping the
// panel's orange toward white in a per-zone table, which passes through a hot salmon at half
// flash -- and that was being ADDED to an already-bright view, so the channels clipped at
// different points and the hue swung. There is no per-zone colour table any more. The region
// flash is a flat fill and the frame draws its authored level throughout, which cannot clip.
//
// The blocks still have to be withheld until their zone comes up: the gate runs BEFORE them in
// the row, so a block drawn early would sit glowing on the black. That is what the intro table
// is for -- its runs are cut at zone borders, so switching a zone on switches whole blocks on.
// The flight table is not cut that way and its zone tags mean nothing; see the generated header.
//
// The frame is held RIGID throughout. The gate and the frame have to agree pixel for pixel or
// the black would not end where the panel does, and the cheapest way to guarantee that is to
// have both read the same unwarped column. CANOPY_INTRO_SETTLE then ramps the flex in at the
// end, because the resting warp is a long way from flat and the cockpit would otherwise jump
// the moment the sequence released.
// THE MEMBERS LIGHT UP TOO, and this part started as a bug the author liked.
//
// The first version flashed the frame's BLOCKS instead of the region, by lerping the panel's
// orange toward white in a per-zone table. That was the wrong AREA -- and it also blew the hue
// out, because a near-white delta added onto an already-bright view clips the three channels at
// different points and the colour swings on the way. The author saw both, and asked for the
// second one back on top of the corrected first one.
//
// So it is deliberate now, and it has somewhere to live: not the white hold, where a member
// drawn additively onto white is invisible, but the DISSOLVE. As world cells come through, the
// members over them are hot and cooling, so the frame lights up as its region resolves and
// settles to its authored level afterwards.
//
// The saturation IS the effect. Nothing here reaches for a hue sweep, because the clip does it
// for free: red is five bits and saturates first, green has six and holds longer, so a rising
// white delta over a lit world passes through magenta and amber before it whites out. Note that
// CANOPY_INTRO_LIT_PEAK below is what governs how much of that is seen -- at 1.0 the members
// spend their first moments fully white, which is the part with no colour in it at all.
// s_intro_on is declared up beside s_can -- see the note there.
static float   s_intro_t    = 0.0f;
static uint8_t s_izon[VG_CANOPY_MAX_ZONES];   // 0 held, 255 fully dissolved to the world
static uint8_t s_ilive[VG_CANOPY_MAX_ZONES];  // whether this zone's blocks are drawn at all
static uint16_t s_ifill[VG_CANOPY_MAX_ZONES]; // a held pixel: black before the flash, white after
static uint8_t s_iglow[VG_CANOPY_MAX_ZONES];  // 255 white-hot members, 0 their authored level
static bool    s_icued     = false;       // the instruments' cue, latched -- see vg_canopy_intro_cued
static uint8_t s_iq[VG_CANOPY_MAX_ZONES];     // the quantised glow each table was built for
static uint16_t s_ilut[VG_CANOPY_MAX_ZONES][256];

// A zone's member colours, from white-hot down to the authored level.
//
// Quantised by the caller, so this runs a couple of dozen times across a region's cool-down
// rather than once a frame per zone.
static void canopy_ilut(int z) {
    const uint32_t t = (uint32_t)s_iglow[z];        // 0..255 toward white
    for (int g = 0; g < 256; g++) {
        const uint32_t v = s_can_lut[g];            // panel order
        const uint32_t n = (v >> 8) | ((v << 8) & 0xFF00u);
        const uint32_t r = (n >> 11) & 31u, gg = (n >> 5) & 63u, b = n & 31u;
        const uint32_t R = r  + (((31u - r)  * t) >> 8);
        const uint32_t G = gg + (((63u - gg) * t) >> 8);
        const uint32_t B = b  + (((31u - b)  * t) >> 8);
        const uint32_t o = (R << 11) | (G << 5) | B;
        s_ilut[z][g] = (uint16_t)(((o >> 8) | (o << 8)) & 0xFFFFu);
    }
    s_iq[z] = s_iglow[z];
}

// A DITHER, NOT A FADE, and it is both cheaper and a better fit.
//
// Holding a region is a store per pixel. Cross-fading one would be three multiplies per pixel,
// about 15 cycles against a handful, and across a 77,000-pixel zone that is milliseconds for a
// transition that lasts a third of a second.
//
// So the reveal is an ordered 4x4 threshold: a pixel gives up its hold once its cell's value is
// under the zone's reveal. Pixels come through in a fixed scatter instead of the whole region
// changing together, which reads as a picture being acquired rather than a lamp being turned
// down -- and it is the same language the backdrop's own reveal already speaks.
static const uint8_t BAYER4[16] = {
     0,  8,  2, 10,
    12,  4, 14,  6,
     3, 11,  1,  9,
    15,  7, 13,  5,
};

// One panel row of the gate. `lx` is the true screen column, never a warped one.
//
// Runs cover the whole column with no gap, so this walks them all and does nothing for a zone
// that is already fully in. Three bytes a run and about three runs a column, so the walk itself
// is free and the cost is entirely in the pixels it holds.
//
// A held pixel takes the zone's fill -- black before its flash, white during it -- so the same
// loop serves both. That is the whole reason the flash is affordable across a region rather than
// only across the frame: a flat fill is the cheapest thing this file does.
static __attribute__((noinline))
void canopy_gate(uint16_t* row, int lx, int py) {
    const uint8_t* p = &s_can->zdata[s_can->zofs[lx]];
    const uint8_t* e = &s_can->zdata[s_can->zofs[lx + 1]];
    const uint8_t* bay = &BAYER4[(py & 3) << 2];
    while (p + 3 <= e) {
        const uint8_t h  = p[0];
        const int     y0 = ((h & 1) << 8) | p[1];
        const int     n  = ((h & 2) << 7) | p[2];
        const int     z  = (h >> 2) & 15;
        p += 3;
        if (z >= s_can->zones) continue;
        const uint32_t rev  = s_izon[z];
        if (rev >= 255u) continue;                  // this region is all the way in
        const uint16_t fill = s_ifill[z];
        uint16_t* q = &row[y0];
        if (rev == 0u) {
            for (int i = 0; i < n; i++) q[i] = fill;  // held: black, or the flash
        } else {
            // 0..16, not 0..15. `rev >> 4` tops out at 15, which leaves one cell of the
            // sixteen still held at the very end of the dissolve -- a sixteenth of the region
            // popping through in one frame. *17>>8 reaches 16, which is past every cell.
            const uint32_t th = (rev * 17u) >> 8;
            for (int i = 0; i < n; i++)
                if ((uint32_t)bay[(y0 + i) & 3] >= th) q[i] = fill;
        }
    }
}

template <bool WARP, bool INTRO>
static void canopy_rows_t(uint16_t* band, int by0, int r0, int r1) {
    if (!s_can_ready) canopy_lut();

    // HOISTED OUT OF THE ROW LOOP, all of it. Which table is a compile-time choice still, so
    // the branch folds; the drawing behind it is a runtime pointer now, and reading it once per
    // call rather than once per row is what keeps that free.
    const VgCanopy* const cp = s_can;
    const uint16_t* const T_OFS  = INTRO ? cp->iofs  : cp->ofs;
    const uint8_t*  const T_DATA = INTRO ? cp->idata : cp->data;
    const int  T_COLS   = (int)cp->cols;
    const bool T_MIRROR = (cp->mirror != 0);

    for (int py = by0 + r0; py < by0 + r1; py++) {
        int lx = SCR_H - 1 - py;                       // this panel row IS a column
        if (WARP) {
            // CLAMPED TO THE EDGE, NOT DROPPED.
            //
            // Shifting or magnifying moves the frame, and a row that lands off the drawing used
            // to be skipped -- which leaves the screen edge empty on the side the frame moved
            // away from, so the cockpit appears to slide and tear rather than to move. Clamping
            // repeats the edge column instead, which is what a texture sampler does at its
            // border and reads as the frame continuing past the screen. Where that column is
            // background, nothing is drawn and nothing is smeared.
            lx += s_lag_px;                       // the frame trailing the turn
            if (lx < 0) lx = 0; else if (lx >= SCR_H) lx = SCR_H - 1;
            lx = s_wcol[lx];
            if (lx < 0) lx = 0; else if (lx >= SCR_H) lx = SCR_H - 1;
        }
        // Mirrored only when the drawing is symmetric, which the baker decides and
        // records. An asymmetric frame stores every column and is read straight
        // through -- a cockpit is allowed to be lopsided.
        int c = (T_MIRROR && lx >= T_COLS) ? (SCR_W - 1 - lx) : lx;
        // Clamped for the same reason, and it also covers a drawing narrower than the screen.
        if (c < 0) c = 0; else if (c >= T_COLS) c = T_COLS - 1;

        const uint8_t* p = &T_DATA[T_OFS[c]];
        const uint8_t* e = &T_DATA[T_OFS[c + 1]];
        uint16_t* row = &band[(py - by0) * SCR_W];

        // THE WORLD FIRST, then the frame on top of it. Everything behind the cockpit is
        // already drawn by the time this primitive runs and the instruments come after it, so
        // this is the one point in the band where blacking the view hides the world without
        // touching the panel. Rigid, so it uses the raw column and lands where the frame does.
        if (INTRO) canopy_gate(row, SCR_H - 1 - py, py);
        // The gate ran; the frame does not. See s_can_rear -- in rear view the world still
        // has to arrive region by region, and the cockpit still has to be absent.
        if (INTRO && s_can_rear) continue;

        // Three bytes of header, then either one level for the whole block or a level
        // per pixel. Nine bits each of start and length, so the odd bit of both rides
        // in the flag byte. The side is in the header too: a block never crosses the
        // background, so nothing here tests light against shade per pixel.
        const int   wofs  = WARP ? (s_wc[c] + s_lag_py
                                    + (int)((float)(c - SCR_H / 2) * s_lag_sh)) : 0;
        // dx is the same for every block in this column, so its share of the sphere hoists.
        const float zbase = WARP ? s_w_zbase[c] : 0.0f;

        // CLAMPING THE OTHER AXIS. The column clamp above covers logical x; this covers logical
        // y, which is a panel row's x. The two need different treatment: a column is SAMPLED, so
        // repeating it is one index clamp, while the runs along a row are a sparse list and the
        // ones touching a border have to be EXTENDED to it -- or the frame ends in mid-air and
        // reads as clipped. Out of line, so the walk below never sees it.
        if (WARP) canopy_edges(row, p, e, wofs, zbase);

        while (p + 3 <= e) {
            const uint8_t h   = p[0];
            const int     y0  = ((h & 1) << 8) | p[1];   // the picture's y is panel x
            const int     len = ((h & 2) << 7) | p[2];
            p += 3;

            // WHOSE TURN IT IS. The zone is only meaningful in the intro table, where a block
            // never spans two of them, so a zone that has not come up yet skips whole blocks.
            //
            // The region's own flash is a flat fill and is already down by the time this runs;
            // what the zone picks here is how hot its MEMBERS still are. Two separate things, and
            // conflating them is what went wrong the first time -- see the note at the intro state.
            const uint16_t* lut = s_can_lut;
            if (INTRO) {
                const int z = (h >> 2) & 15;
                if (z >= s_can->zones || !s_ilive[z]) {
                    p += (h & 0x80) ? len : 1;
                    continue;
                }
                lut = s_ilut[z];
            }

            int at = y0, n = len, skip = 0, n0 = len;
            if (WARP) {
                // Both ends through the same map, and the length is the difference -- so the
                // run still ends exactly where the next one starts.
                at = warp_y(y0,       zbase) + wofs;
                n  = warp_y(y0 + len, zbase) + wofs - at;
                n0 = n;                       // before clipping: what the step is measured on
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
                if (WARP && n0 != len) {
                    // Stretched or squeezed: walk the levels at the ratio rather than one
                    // for one, or the block reads past its own data. See span_lit_add_rs.
                    const uint32_t step = ((uint32_t)len << 16) / (uint32_t)n0;
                    const uint32_t i0   = (uint32_t)skip * step;
                    if (h & 0x40) span_lit_sub_rs(&row[at], n, p, lut, i0, step);
                    else          span_lit_add_rs(&row[at], n, p, lut, i0, step);
                } else {
                    if (h & 0x40) span_lit_sub(&row[at], n, p + skip, lut);
                    else          span_lit_add(&row[at], n, p + skip, lut);
                }
                p += len;
            } else {                                     // one level, delta hoisted
                const uint16_t d = lut[*p++];
                if (h & 0x40) span_sub(&row[at], n, d);
                else          span_add(&row[at], n, d);
            }
        }
    }
}

// WHERE THE SEQUENCE IS UP TO.
//
// The zones come up in the order the artist painted, which is already the zone INDEX: the baker
// sorts the green values it found and stores each block's position in that order, so zone 0 is
// simply first and this needs no table of its own.
void vg_canopy_intro_begin(void) {
    // NO COCKPIT, NO SEQUENCE -- BUT THE CHAIN STILL HAS TO RUN.
    //
    // The sequence is the cockpit arriving a region at a time, and with no drawing there are no
    // regions and no zone map to hold the world black with. So it does not play.
    //
    // The cue is latched anyway, and that is the part that matters: the instruments hang off it
    // -- draw_instruments is vg.hud_cued -- so returning early here left a hull with no canopy
    // showing no cockpit AND no instruments, for ever. The chain degrades to what the game did
    // before there were canopies: the panel catches, then the player is ready, then the radio.
    if (!s_can) {
        s_intro_on = false;
        s_icued    = true;
        s_settle_t = -1.0f;
        for (int i = 0; i < 3; i++) { s_lag_q[i] = s_lag_v[i] = s_lag_x[i] = 0.0f; }
        s_lag_px = s_lag_py = 0;
        s_lag_sh = 0.0f;
        return;
    }
    s_intro_on = true;
    s_intro_t  = 0.0f;
    s_settle_t = -1.0f;
    s_icued    = false;
    if (!s_can_ready) canopy_lut();
    for (int z = 0; z < s_can->zones; z++) {
        s_izon[z] = 0; s_ilive[z] = 0; s_ifill[z] = 0;   // held, and held BLACK
        s_iglow[z] = 255;                                // and white-hot the moment it lights
        canopy_ilut(z);
    }
    // Nothing left over on the spring, or the frame would start the sequence already leaning.
    for (int i = 0; i < 3; i++) { s_lag_q[i] = s_lag_v[i] = s_lag_x[i] = 0.0f; }
    s_lag_px = s_lag_py = 0;
    s_lag_sh = 0.0f;
}

bool vg_canopy_intro_active(void) { return s_intro_on; }

// HAS THE SEQUENCE CALLED FOR THE INSTRUMENTS YET.
//
// A LATCH, and it is a latch because the obvious version was a bug. This started as a
// `progress() >= CANOPY_INTRO_HUD_AT` test against a progress function that returned 1.0 when
// nothing was running -- so the condition was satisfied by DEFAULT, and vg_hud_decay is reached
// from vg_world_step, which runs in the attract loop, the title and the cutscene. The cockpit's
// power-on sound played on the title screen.
//
// Worse, it was not idempotent across matches: vg_match_start is called from enter_intro, at the
// top of the CUTSCENE, so the game-side flag was cleared several seconds before the player ever
// took the seat -- and the cutscene then ran thousands of world steps with the cue armed.
//
// So the state cannot be inferred. Only the running sequence sets this, and only `reset` and
// `begin` clear it, which makes every ordering safe: nothing is cued at power-on, nothing is
// cued while a match is being built, and nothing is cued twice.
bool vg_canopy_intro_cued(void) { return s_icued; }

// HOW MANY REGIONS HAVE LIT IN THE RUNNING SEQUENCE, 0..CANOPY_ZONES.
//
// Reported rather than acted on, because the thing that wants it is a SOUND and this file is
// the band rasteriser -- it has no business talking to the mixer, and a cue fired from here
// would be a cue that does not happen when the panel is not being drawn. The caller watches
// this for a change and beeps, which also lets it pitch each one.
//
// ZERO WHEN NOTHING IS RUNNING, and that is not a detail. `reset` leaves every region live so
// the flight path draws the whole frame, so counting s_ilive unconditionally reports four the
// entire time a match is not booting -- and the caller, whose own counter was just zeroed,
// would have fired four beeps over the cutscene. That is the same trap the instruments' cue
// fell into: a query that answers for a state it is not in. The answer has to be scoped to the
// sequence, because that is the only thing the question means.
int vg_canopy_intro_lit(void) {
    if (!s_can || !s_intro_on) return 0;
    int n = 0;
    for (int z = 0; z < s_can->zones; z++) if (s_ilive[z]) n++;
    return n;
}

// Nothing running and nothing cued -- for when a match is BUILT, which happens at the top of the
// cutscene and is a long way from the player taking the seat.
void vg_canopy_intro_reset(void) {
    s_intro_on = false;
    s_icued    = false;
    s_settle_t = -1.0f;
    // A HULL WITH NO DRAWING REACHES HERE, and this used to read s_can->zones anyway.
    //
    // vg_match_start calls this from the top of the cutscene, before anything has selected a
    // cockpit for the hull about to fly -- so on a hull that HAS no cockpit, s_can is null and
    // the load faulted. The panic rebooted the board, and from the seat that looks like the
    // match refusing to start and dropping back to the title. Reported exactly that way.
    //
    // Guarded here rather than at the call, because the three scalars above are the boot
    // chain's disarm and have to happen for every hull. Only the per-zone arrays need a
    // drawing to be about.
    if (!s_can) return;
    for (int z = 0; z < s_can->zones; z++) { s_izon[z] = 255; s_ilive[z] = 1; s_iglow[z] = 0; }
}

// HOW MUCH FLEX THE FRAME IS ALLOWED, 0 through the sequence and 1 once it has settled.
//
// The caller multiplies both the warp and the lag by this. It is not an optimisation: the world
// gate reads the unwarped column, so a warped frame during the sequence would put the black
// somewhere other than where the panel ends. The ramp afterwards exists because the resting
// warp is full bulge -- releasing straight into it would pop.
float vg_canopy_intro_flex(void) {
    if (!s_can)     return 1.0f;    // nothing to hold flat, and nothing to settle
    if (s_intro_on) return 0.0f;
    if (s_settle_t < 0.0f) return 1.0f;
    float a = s_settle_t / CANOPY_INTRO_SETTLE;
    if (a >= 1.0f) return 1.0f;
    return a * a * (3.0f - 2.0f * a);          // smoothstep, so it arrives without a corner
}

bool vg_canopy_intro_update(float dt) {
    if (!s_can) return false;
    if (!s_intro_on) {
        if (s_settle_t >= 0.0f) {
            s_settle_t += dt;
            if (s_settle_t >= CANOPY_INTRO_SETTLE) s_settle_t = -1.0f;
        }
        return false;
    }
    s_intro_t += dt;

    // THE INSTRUMENTS' CUE, latched from inside the running sequence -- which is the only place
    // that can know the sequence is actually running. The length is derived here from the pacing
    // constants, so retuning them moves the cue with them rather than sliding it out of step.
    const float span = CANOPY_INTRO_LEAD + (float)(s_can->zones - 1) * CANOPY_INTRO_STEP
                     + CANOPY_INTRO_FLASH + CANOPY_INTRO_DISSOLVE + CANOPY_INTRO_LIT;
    if (!s_icued && span > 0.0f && s_intro_t >= span * CANOPY_INTRO_HUD_AT) s_icued = true;

    for (int z = 0; z < s_can->zones; z++) {
        const float e = s_intro_t - (CANOPY_INTRO_LEAD + (float)z * CANOPY_INTRO_STEP);
        if (e < 0.0f) continue;                  // this one's turn has not come
        s_ilive[z]  = 1;
        s_ifill[z]  = CANOPY_INTRO_WHITE;        // it has flashed, so a held pixel is white now

        // The region holds solid white for FLASH, then the world dissolves out of it. A flash
        // has an instant onset by definition, so there is no ramp up -- the fill switches from
        // black to white on the frame the zone's turn arrives.
        float r = (e - CANOPY_INTRO_FLASH) / CANOPY_INTRO_DISSOLVE;
        if (r < 0.0f) r = 0.0f;
        if (r > 1.0f) r = 1.0f;
        s_izon[z] = (uint8_t)(r * 255.0f + 0.5f);

        // The members HOLD at full heat through the dissolve and cool only afterwards.
        //
        // Cooling them from the moment the region lit was the obvious reading and it wasted the
        // effect: a member is invisible against the region's own white fill, so at the instant the
        // glow peaked there was nothing behind it to clip against, and by the time world cells
        // came through the colour had already gone. The heat has to still be there when the world
        // arrives, because the world is what it saturates against.
        //
        // So the frame is the LAST thing to settle, cooling over LIT after its region is fully in.
        float f = 1.0f - (e - CANOPY_INTRO_FLASH - CANOPY_INTRO_DISSOLVE) / CANOPY_INTRO_LIT;
        if (f < 0.0f) f = 0.0f;
        if (f > 1.0f) f = 1.0f;
        f *= CANOPY_INTRO_LIT_PEAK;
        const int q = (int)(f * CANOPY_INTRO_QSTEP + 0.5f);
        const uint8_t want = (uint8_t)((q * 255) / CANOPY_INTRO_QSTEP);
        if (want != s_iq[z]) { s_iglow[z] = want; canopy_ilut(z); }
    }

    // Over when the last zone's members have finished cooling, which is now the last thing to
    // happen anywhere in the sequence. The settle that follows is not part of it: the view is
    // fully in well before this and the frame is merely taking up its flex.
    const float end = CANOPY_INTRO_LEAD + (float)(s_can->zones - 1) * CANOPY_INTRO_STEP
                    + CANOPY_INTRO_FLASH + CANOPY_INTRO_DISSOLVE + CANOPY_INTRO_LIT;
    if (s_intro_t >= end) {
        s_intro_on = false;
        s_settle_t = 0.0f;
        for (int z = 0; z < s_can->zones; z++) {
            s_izon[z] = 255; s_ilive[z] = 1;
            if (s_iglow[z]) { s_iglow[z] = 0; canopy_ilut(z); }
        }
        return false;
    }
    return true;
}

// Three instantiations, not four. The intro is always rigid -- see the note above the intro
// state -- so there is no warped intro path to build.
static void canopy_rows(uint16_t* band, int by0, int r0, int r1) {
    if (!s_can) return;                    // no drawing selected: no cockpit, and no crash
    // Aft with no sequence running: there is no frame to draw and no world to hold back, so
    // the whole pass is skipped. Checked here rather than at the submit site, because the
    // primitive still has to exist for the intro's gate to run.
    if (s_can_rear && !s_intro_on) return;
    if (s_intro_on)     canopy_rows_t<false, true>(band, by0, r0, r1);
    else if (s_warp_on) canopy_rows_t<true, false>(band, by0, r0, r1);
    else                canopy_rows_t<false, false>(band, by0, r0, r1);
}

// A FAN OF LINES, both ways, over every slope.
//
// Endpoints are generated inside one band and across the full width, from a fixed sequence,
// so the set covers shallow, steep and diagonal alike and is the same set every run. Both
// banks are compared: a faster walk that puts a pixel in a different place is not faster.
//
// SHARED WITH THE BLEND BENCH BELOW, and it is worth saying why rather than each keeping
// its own. A second copy cost 3 KB of the scarce internal SRAM for a table that would hold
// the same 256 segments from the same seed -- and two fans that were meant to be the same
// set are two fans that can drift apart, which would quietly make the opaque and blended
// numbers describe different workloads.
struct BenchSeg { int16_t x0, y0, x1, y1; };
static BenchSeg s_bseg[256];
static int      s_nbseg = 0;

static void bench_fan(void) {
    if (s_nbseg) return;
    uint32_t r = 12345u;
    const int H = BAND_H - 1;
    for (int i = 0; i < 256; i++) {
        r = r * 1664525u + 1013904223u;
        s_bseg[i].x0 = (int16_t)((r >> 16) % SCR_W);
        r = r * 1664525u + 1013904223u;
        s_bseg[i].y0 = (int16_t)((r >> 16) % (H + 1));
        r = r * 1664525u + 1013904223u;
        s_bseg[i].x1 = (int16_t)((r >> 16) % SCR_W);
        r = r * 1664525u + 1013904223u;
        s_bseg[i].y1 = (int16_t)((r >> 16) % (H + 1));
    }
    s_nbseg = 256;
}

void vg_line_bench(VgLineCost* out) {
    if (!s_band[0] || !s_band[1]) { *out = VgLineCost{}; return; }

    bench_fan();
    const BenchSeg* seg = s_bseg;
    const int nseg = s_nbseg;

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

// ===========================================================================
// THE BLENDED PATH, PRICED
//
// performance.md has carried this as an open target since 2026-08-04: blend_px and
// plot_delta still do the naive form -- extract three channels, add or subtract each with
// a compare, shift all three back, swap twice -- which is exactly what px_add replaced in
// the canopy for 14.7%. They are also two verbatim copies of the same body, one with
// bounds checks.
//
// It was left undone ON PURPOSE, and the note says why: "there is no bench for the line
// path, so it could only be reasoned about, and reasoning about this loop is what produced
// every wrong conclusion above." Every wrong conclusion in that section came from an
// estimate. So this is the bench, and it is built BEFORE the change rather than after it.
//
// NOTHING BELOW IS REACHED BY THE GAME. The candidates are copies that live here so the
// question "what would it buy" can be answered in microseconds instead of adjectives. If
// the number justifies it, the shipping bodies get replaced and these become the _ref.
// ===========================================================================

// The delta pre-split into the two fields px_add wants, in NATIVE bit positions. The
// shipping path splits it per pixel; a caller can do it once a line, which is part of
// what is being priced here.
struct Delta2 { uint32_t rb, g; };
static inline Delta2 delta2(uint16_t d_native) {
    return Delta2{ (uint32_t)(d_native & 0xF81Fu), (uint32_t)(d_native & 0x07E0u) };
}

// blend_px done with the branchless pair. px_add takes and returns PANEL order and does
// both swaps itself, so the native round trip the shipping body writes out disappears.
static inline void blend_px_fast(uint16_t* p, Delta2 d, bool add) {
    *p = (uint16_t)(add ? px_add(*p, d.rb, d.g) : px_sub(*p, d.rb, d.g));
}

static inline void plot_delta_fast(uint16_t* band, int by0, int by1,
                                   int x, int y, Delta2 d, bool add) {
    if (y < by0 || y > by1) return;
    if ((unsigned)x >= (unsigned)SCR_W) return;
    uint16_t* p = &band[(y - by0) * SCR_W + x];
    *p = (uint16_t)(add ? px_add(*p, d.rb, d.g) : px_sub(*p, d.rb, d.g));
}

// The same Bresenham walk as band_line_delta, so what the bench reports is the difference
// in the pixel operation and not in the traversal.
static void band_line_delta_fast(uint16_t* band, int by0, int by1,
                                 int x0, int y0, int x1, int y1, uint16_t colour, bool add) {
    const uint16_t dn = (uint16_t)((colour >> 8) | (colour << 8));
    const Delta2   d  = delta2(dn);

    int dx =  abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    int x = x0, y = y0;
    for (;;) {
        plot_delta_fast(band, by0, by1, x, y, d, add);
        if (x == x1 && y == y1) break;
        const int e2 = err << 1;
        if (e2 >= dy) { err += dy; x += sx; }
        if (e2 <= dx) { err += dx; y += sy; }
    }
}

// THE BANK, AND WHY IT IS NOT ZEROED.
//
// A blend bench over a black band measures nothing: subtraction from black cannot borrow
// and addition to black cannot saturate, so every branch the shipping body pays for would
// predict perfectly and the naive form would come out looking free. Filled with a varying
// pattern instead, so some pixels are near white, some near black, and the clip is reached
// in both directions.
static void blend_fill_pattern(uint16_t* band) {
    for (int i = 0; i < SCR_W * BAND_H; i++) {
        const uint32_t v = (uint32_t)((uint32_t)i * 2654435761u) >> 16;
        const uint16_t n = (uint16_t)(v & 0xFFFFu);
        band[i] = (uint16_t)((n >> 8) | (n << 8));   // the band holds panel order
    }
}

// Four deltas, two that saturate their fields and two that do not, each used both ways --
// so neither the branchy form nor the guard-bit form gets an easy set. Derived from the
// index rather than stored: they are a pure function of it, and a table would be 3 KB of
// internal SRAM to hold what an AND can say.
static inline uint16_t bench_delta(int i) {
    static const uint16_t COLS[4] = { 0xF81F, 0x0841, 0x7BEF, 0x2104 };
    return COLS[i & 3];
}
static inline bool bench_adds(int i) { return ((i >> 2) & 1) != 0; }

void vg_blend_bench(VgBlendCost* out) {
    *out = VgBlendCost{};
    if (!s_band[0] || !s_band[1]) return;

    bench_fan();
    const BenchSeg* seg = s_bseg;
    const int nseg = s_nbseg;

    uint32_t la = 0, lb = 0, sa = 0, sb = 0;
    long lpx = 0;

    // ---- the lit line: plot_delta against plot_delta_fast ----
    blend_fill_pattern(s_band[0]);
    blend_fill_pattern(s_band[1]);
    for (int rep = 0; rep < 4; rep++) {
        const uint32_t t0 = esp_cpu_get_cycle_count();
        for (int i = 0; i < nseg; i++)
            band_line_delta(s_band[0], 0, BAND_H - 1,
                            seg[i].x0, seg[i].y0, seg[i].x1, seg[i].y1,
                            bench_delta(i), bench_adds(i));
        const uint32_t t1 = esp_cpu_get_cycle_count();
        for (int i = 0; i < nseg; i++)
            band_line_delta_fast(s_band[1], 0, BAND_H - 1,
                                 seg[i].x0, seg[i].y0, seg[i].x1, seg[i].y1,
                                 bench_delta(i), bench_adds(i));
        const uint32_t t2 = esp_cpu_get_cycle_count();
        la += t1 - t0;
        lb += t2 - t1;
    }
    const bool line_same = memcmp(s_band[0], s_band[1], SCR_W * BAND_H * 2) == 0;
    for (int i = 0; i < nseg; i++) {
        const int a = abs(seg[i].x1 - seg[i].x0), b = abs(seg[i].y1 - seg[i].y0);
        lpx += (a > b ? a : b) + 1;
    }

    // ---- the member: the span loop out of band_tri, both ways ----
    //
    // Full-width rows rather than triangle spans. band_tri's edge interpolation is common
    // to both bodies, and timing it would only dilute the thing that differs.
    blend_fill_pattern(s_band[0]);
    blend_fill_pattern(s_band[1]);
    for (int rep = 0; rep < 4; rep++) {
        const uint32_t t0 = esp_cpu_get_cycle_count();
        for (int y = 0; y < BAND_H; y++) {
            uint16_t* row = &s_band[0][y * SCR_W];
            const uint16_t c = bench_delta(y & 255);
            const uint16_t dn = (uint16_t)((c >> 8) | (c << 8));
            const bool add = bench_adds(y & 255);
            for (int x = 0; x < SCR_W; x++) blend_px(&row[x], dn, add);
        }
        const uint32_t t1 = esp_cpu_get_cycle_count();
        for (int y = 0; y < BAND_H; y++) {
            uint16_t* row = &s_band[1][y * SCR_W];
            const uint16_t c = bench_delta(y & 255);
            const uint16_t dn = (uint16_t)((c >> 8) | (c << 8));
            const Delta2 d = delta2(dn);
            const bool add = bench_adds(y & 255);
            for (int x = 0; x < SCR_W; x++) blend_px_fast(&row[x], d, add);
        }
        const uint32_t t2 = esp_cpu_get_cycle_count();
        sa += t1 - t0;
        sb += t2 - t1;
    }
    const bool span_same = memcmp(s_band[0], s_band[1], SCR_W * BAND_H * 2) == 0;

    out->line_ref_us = la / 240u;
    out->line_now_us = lb / 240u;
    out->span_ref_us = sa / 240u;
    out->span_now_us = sb / 240u;
    out->lines       = nseg * 4;
    out->line_px     = lpx * 4;
    out->span_px     = (long)SCR_W * BAND_H * 4;
    out->same        = line_same && span_same;
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
    uint32_t pc = 0, fc = 0, tc = 0, sum = 2166136261u;
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
        // AND AGAIN WITH THE BOUNDARY TINT ON. Held at 0.6 rather than 1.0: at full the
        // innermost ring reaches the centre and every pixel is inside the gradient, which is
        // the geometry at the instant of death and not the one a player flies in.
        //
        // NO TINT TO FORCE AROUND IT ANY MORE. This used to set the ring to 0.625 --
        // exactly on a quantiser step, so the checksum stayed comparable -- because the
        // fill's cost depended on whether the player was near a wall. The ring is gone, so
        // the fill has one cost and this is it.
        // Timed on one core like the fill it is compared against -- the frame splits both.
        const uint32_t t3 = esp_cpu_get_cycle_count();
        vg_sky_fill_rows(s_band[0], by0, 0, BAND_H);
        const uint32_t t4 = esp_cpu_get_cycle_count();

        pc += t1 - t0;
        fc += t2 - t1;
        tc += t4 - t3;

        // Outside the timing: what the band came out as, folded in a word at a time.
        const uint32_t* w = (const uint32_t*)(const void*)s_band[0];
        for (int i = 0; i < SCR_W * BAND_H / 2; i++) sum = (sum ^ w[i]) * 16777619u;
    }
    vg_sky_bench_pin(false);
    out->prep_us = pc / 240u;
    out->fill_us = fc / 240u;
    out->tint_us = tc / 240u;
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
    if (!s_can) { *out = VgCanopyCost{}; return; }
    if (!s_can_ready) canopy_lut();

    // BOTH STATES, because the warp is the thing most likely to be changed next and its cost
    // is the question. The game's own setting is saved and put back, so running the bench does
    // not leave the frame flexing at whatever the bench used last.
    const int   save_q  = s_wq;
    const bool  save_on = s_warp_on;

    // AND THE INTRO IS PINNED OFF for the first two passes, because canopy_rows now dispatches
    // on it. Left alone, a bench run during the sequence would time the intro path and report
    // it as the flight cost -- the same failure the glyph bench had, where the number is wrong
    // and believed. Measured on purpose in the third pass below.
    const bool  save_in = s_intro_on;
    uint8_t     save_live[VG_CANOPY_MAX_ZONES], save_rev[VG_CANOPY_MAX_ZONES];
    uint16_t    save_fill[VG_CANOPY_MAX_ZONES];
    for (int z = 0; z < s_can->zones; z++) {
        save_live[z] = s_ilive[z]; save_rev[z] = s_izon[z]; save_fill[z] = s_ifill[z];
    }
    s_intro_on = false;

    uint32_t cyc = 0, cal = 0, wcyc = 0, icyc = 0;
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
    // THE INTRO, AT ITS WORST, which is the only figure worth having.
    //
    // Its cost is not constant: a zone that has not come up is a full-screen black fill, one
    // mid-dissolve is a threshold test per pixel, and one that is all the way in is skipped
    // entirely. The dear case is every zone dissolving at once -- which the sequence never
    // actually reaches, since the zones are staggered -- so this is a ceiling and not a
    // measurement of the sequence.
    //
    // Perf is not the point of the intro and the author has said so. This exists so that "it
    // dips" is a number rather than an impression, and so the next person to widen the zones
    // knows what they are spending.
    s_intro_on = true;
    for (int z = 0; z < s_can->zones; z++) {
        s_ilive[z] = 1; s_izon[z] = 128; s_ifill[z] = CANOPY_INTRO_WHITE;
    }
    vg_canopy_warp(0.0f);
    for (int py = 0; py < SCR_H; py++) {
        for (int x = 0; x < SCR_W; x++) scratch[x] = 0x1084;
        const uint32_t t0 = esp_cpu_get_cycle_count();
        canopy_rows(scratch, py, 0, 1);
        icyc += esp_cpu_get_cycle_count() - t0;
    }

    s_wq = save_q; s_warp_on = save_on;   // leave the game's own flex as it was
    s_intro_on = save_in;
    for (int z = 0; z < s_can->zones; z++) {
        s_ilive[z] = save_live[z]; s_izon[z] = save_rev[z]; s_ifill[z] = save_fill[z];
    }

    out->us       = (cyc  > cal ? cyc  - cal : 0) / 240u;
    out->warp_us  = (wcyc > cal ? wcyc - cal : 0) / 240u;
    out->intro_us = (icyc > cal ? icyc - cal : 0) / 240u;
    out->blocks   = (int)s_can->blocks;
    out->flat_px  = (int)s_can->flat_px;
    out->lit_px   = (int)s_can->lit_px;
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
            // Warped, the baked balance point is for a distribution that no longer applies.
            // During the intro neither applies: the world gate blacks out whole columns of
            // screen and dwarfs the frame, and its work is spread evenly enough across a band
            // that the midpoint is the right guess.
            if (!s_can) break;
            const int at = s_intro_on ? ROW_SPLIT
                         : s_warp_on  ? s_wsplit[band_index]
                                      : s_can->split[band_index];
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
        // `aa` COUNTS EVERY BLENDED LINE, NOT ONLY SMOOTHED ONES. The field holds
        // LINE_AA, LINE_ADD and LINE_SUB alike, so an additive line lands in this bucket
        // and out of `ln`. That is worth knowing before reading the two against each
        // other: making the ship trails additive moved them wholesale from one to the
        // other, which looked like the line cost falling and antialiasing switching itself
        // on, and was neither.
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
        // Rotating over THREE now. Bands b-1 and b-2 may both still be on the wire, so
        // b needs a buffer neither of them is using -- see BAND_BUFS.
        uint16_t* buf = s_band[b % BAND_BUFS];

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
        // THE WALL WARNING USED TO BE APPLIED HERE, over the finished band, so that it
        // coloured the scanlines too. Then it moved to the source -- the sky fill and every
        // primitive's colour at submit -- and this became a no-op holding a variable alive.
        // Now it is the cockpit's own colour table, so there is nothing at any of the three
        // places. See vg_canopy_alarm.
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
