#include "vg_raster.h"
#include "vg_raster_int.h"
#include "vg_font.h"
#include "vg_port.h"
#include "vg_capture.h"
#include "vg_sky.h"
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

// Clip a line to a y-range. x is already inside the screen, so only y needs
// trimming; a parametric clip on y alone is cheaper and exact.
static inline bool clip_band_y(float* ax, float* ay, float* bx, float* by,
                               float ymin, float ymax) {
    float y0 = *ay, y1 = *by;
    if (y0 == y1) return (y0 >= ymin && y0 <= ymax);

    float dy = y1 - y0;
    float ta = (ymin - y0) / dy;
    float tb = (ymax - y0) / dy;
    if (ta > tb) { float t = ta; ta = tb; tb = t; }

    float t0 = ta > 0.0f ? ta : 0.0f;
    float t1 = tb < 1.0f ? tb : 1.0f;
    if (t0 > t1) return false;

    float x0 = *ax, dx = *bx - x0;
    *ax = x0 + dx * t0;  *ay = y0 + dy * t0;
    *bx = x0 + dx * t1;  *by = y0 + dy * t1;
    return true;
}

static inline void band_line(uint16_t* band, int band_y0,
                             int x0, int y0, int x1, int y1, uint16_t color) {
    // Endpoints are clamped to the band, so Bresenham cannot leave it: every
    // pixel it visits lies within the endpoints' bounding box.
    int dx =  abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;

    int x = x0, y = y0;
    for (;;) {
        band[(y - band_y0) * SCR_W + x] = color;
        if (x == x1 && y == y1) break;
        int e2 = err << 1;
        if (e2 >= dy) { err += dy; x += sx; }
        if (e2 <= dx) { err += dx; y += sy; }
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

// Bresenham, but clipped only in y -- the fast path used for trails and other
// dim one-pixel geometry. Unlike band_line above it cannot assume x stays in
// range, because a caller that skipped AA still went through the same clip.
static inline void band_line_fast(uint16_t* band, int by0, int by1,
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
static void band_tri(uint16_t* band, int by0, int by1,
                     int x0, int y0, int x1, int y1, int x2, int y2,
                     uint16_t color) {
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
        for (int x = xl; x <= xr; x++) row[x] = color;
    }
}

static inline void band_glyph(uint16_t* band, int by0, int by1, const Prim* p) {
    const int      scale = p->x1;
    const uint8_t* glyph = VG_FONT5X7[p->y1 - VG_FONT_FIRST];

    // The font bitmap is authored in the LOGICAL frame, so each set pixel's
    // offset is mapped into panel space here. Without this the text would stay
    // aligned to the panel while the rest of the world turned.
    for (int col = 0; col < 5; col++) {
        uint8_t bits = glyph[col];
        if (!bits) continue;
        for (int row = 0; row < 7; row++) {
            if (!(bits & (1 << row))) continue;
            for (int jy = 0; jy < scale; jy++) {
                for (int jx = 0; jx < scale; jx++) {
                    const int dx = col * scale + jx;
                    const int dy = row * scale + jy;
#if VG_ROTATE == 1
                    const int xx = p->x0 + dy, yy = p->y0 - dx;
#elif VG_ROTATE == 2
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

static inline void band_scanlines(uint16_t* band, int by0) {
    int first = (SCANLINE_PITCH - (by0 % SCANLINE_PITCH)) % SCANLINE_PITCH;

    for (int y = first; y < BAND_H; y += SCANLINE_PITCH) {
        // Rows are 960 bytes and the buffer is 16-byte aligned, so this is
        // always 4-byte aligned. The zero test still pays off wherever the
        // backdrop is dark.
        // Unrolled four ways. Measured at ~3.3ms a frame -- 20% of a 60fps
        // budget -- for 38,400 words of work that is five operations each, so
        // most of it was loop overhead rather than the arithmetic. SCR_W/2 is
        // 240, divisible by 4, so no remainder handling is needed.
        uint32_t* p = (uint32_t*)&band[y * SCR_W];
        for (int i = 0; i < SCR_W / 2; i += 4) {
            uint32_t a = p[i], b = p[i + 1], c = p[i + 2], d = p[i + 3];
            if (a) p[i]     = scanline_pair(a);
            if (b) p[i + 1] = scanline_pair(b);
            if (c) p[i + 2] = scanline_pair(c);
            if (d) p[i + 3] = scanline_pair(d);
        }
    }
}

// ---------------------------------------------------------------------------

// Raster cost, split three ways. Guessing which of these dominates has now been
// wrong twice: the primitive count alone cannot tell a long antialiased span
// from a triangle fill covering a third of the screen, and neither shows up
// against a constant backdrop cost. So measure all three.
static uint32_t s_sky_us = 0, s_prim_us = 0, s_scan_us = 0;

uint32_t vg_rast_sky_us(void)  { return s_sky_us; }
uint32_t vg_rast_prim_us(void) { return s_prim_us; }
uint32_t vg_rast_scan_us(void) { return s_scan_us; }

static void draw_band(int band_index, uint16_t* band) {
    const int by0 = band_index * BAND_H;
    const int by1 = by0 + BAND_H - 1;

    const uint32_t t_sky = micros();

    // The backdrop fill REPLACES the clear rather than adding to it, so its net
    // cost is only what it exceeds a memset by.
    if (vg_sky_ready()) vg_sky_fill_band(band, by0);
    else                memset(band, 0, SCR_W * BAND_H * 2);

    const uint32_t t_prim = micros();
    s_sky_us += t_prim - t_sky;

    const Prim* prims = vg_prim_list();
    const int   n     = vg_prim_live();

    for (int i = 0; i < n; i++) {
        const Prim* p = &prims[i];
        if (p->ymax < by0 || p->ymin > by1) continue;

        switch (p->type) {
        case PRIM_POINT:
            band[(p->y0 - by0) * SCR_W + p->x0] = p->color;
            break;

        case PRIM_LINE: {
            float ax = p->x0, ay = p->y0, bx = p->x1, by = p->y1;
            if (!clip_band_y(&ax, &ay, &bx, &by, (float)by0, (float)by1)) break;

            int ix0 = (int)lrintf(ax), iy0 = (int)lrintf(ay);
            int ix1 = (int)lrintf(bx), iy1 = (int)lrintf(by);
            // Guard against float rounding pushing an endpoint one row out.
            if (iy0 < by0) iy0 = by0; else if (iy0 > by1) iy0 = by1;
            if (iy1 < by0) iy1 = by0; else if (iy1 > by1) iy1 = by1;
            if (ix0 < 0) ix0 = 0; else if (ix0 > SCR_W - 1) ix0 = SCR_W - 1;
            if (ix1 < 0) ix1 = 0; else if (ix1 > SCR_W - 1) ix1 = SCR_W - 1;
#if VG_LINE_AA
            if (p->aa) band_line_aa(band, by0, by1, ix0, iy0, ix1, iy1, p->color);
            else       band_line_fast(band, by0, by1, ix0, iy0, ix1, iy1, p->color);
#else
            band_line(band, by0, ix0, iy0, ix1, iy1, p->color);
#endif
            break;
        }

        case PRIM_TRI:
            band_tri(band, by0, by1, p->x0, p->y0, p->x1, p->y1, p->x2, p->y2, p->color);
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
    }

    s_prim_us += micros() - t_prim;
}

static uint32_t s_raster_us = 0;
static uint32_t s_wait_us   = 0;

uint32_t vg_rast_raster_us(void) { return s_raster_us; }
uint32_t vg_rast_wait_us(void)   { return s_wait_us; }

void vg_rast_flush(void) {
    // Drain the LAST band of the previous frame before touching its buffer
    // again. Waiting here rather than at the end of the loop means that final
    // transfer overlaps this frame's input and simulation, which are already
    // done by the time we get here.
    const uint32_t w0 = micros();
    vg_panel_wait();
    s_wait_us = micros() - w0;

    uint32_t raster = 0;
    s_sky_us = s_prim_us = s_scan_us = 0;

    for (int b = 0; b < NUM_BANDS; b++) {
        uint16_t* buf = s_band[b & 1];

        // Timed WITHOUT the push: vg_panel_push_band blocks on the previous
        // transfer, so folding it in here would charge us for the DMA we are
        // trying to hide under. What this counter measures is the CPU work that
        // has to fit inside the transfer window, which is the number that
        // actually decides the frame rate.
        const uint32_t r0 = micros();
        draw_band(b, buf);
        // Over the finished band, so the backdrop and the instruments drawn on
        // top of it are striped alike. Baking it into the backdrop texture
        // instead silently skipped every vector element.
        const uint32_t t_scan = micros();
        band_scanlines(buf, b * BAND_H);
        s_scan_us += micros() - t_scan;
        raster += micros() - r0;

        // Captured AFTER the scanline pass and BEFORE the blit, so the recording
        // is exactly the bytes the panel receives -- effects included, and with
        // no chance of catching a buffer mid-redraw. Reads the band without
        // touching it, so the transfer that follows is unaffected.
        if (b == 0) vg_capture_frame_begin();
        vg_capture_band(b * BAND_H, BAND_H, buf);

        // Queues and returns: the next iteration rasterises into the other
        // buffer while this one is on the wire.
        vg_panel_push_band(b * BAND_H, BAND_H, buf);
    }

    vg_capture_frame_end();
    s_raster_us = raster;
}
