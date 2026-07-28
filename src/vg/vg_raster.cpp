#include "vg_raster.h"
#include "vg_raster_int.h"
#include "vg_font.h"
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <math.h>

// The submit half. Everything here runs ONCE per frame and costs frame time
// directly -- unlike the band raster in vg_band.cpp, which hides under DMA. Work
// added here is the expensive kind.
//
// Order of operations for every primitive: HUD warp (logical space) -> display
// rotation -> screen clip -> append. Doing the rotation here rather than by
// transposing band buffers keeps it to a couple of ops per primitive instead of
// a per-pixel copy.

// ---------------------------------------------------------------------------
// Primitive list
// ---------------------------------------------------------------------------

static Prim* s_prims    = nullptr;
static int   s_count    = 0;
static bool  s_overflow = false;

bool vg_prim_init(void) {
    // Internal: the list is swept once per band, 15 times a frame.
    s_prims = (Prim*)heap_caps_malloc(sizeof(Prim) * MAX_PRIMS,
                                      MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_prims) {
        Serial.println("vg_prim_init: alloc failed");
        return false;
    }
    return true;
}

const Prim* vg_prim_list(void) { return s_prims; }
int         vg_prim_live(void) { return s_count; }

static uint8_t s_line_aa = 1;
void vg_line_aa_mode(bool on) { s_line_aa = on ? 1 : 0; }

bool vg_rast_init(void) {
    if (!vg_prim_init()) return false;
    if (!vg_band_init()) return false;

    Serial.printf("vg_rast_init: prims %uKB bands 2x%uKB (%d bands) internal-free %uKB\n",
                  (unsigned)(sizeof(Prim) * MAX_PRIMS / 1024),
                  (unsigned)(SCR_W * BAND_H * 2 / 1024),
                  (int)NUM_BANDS,
                  (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
    return true;
}

void vg_rast_begin_frame(void) { s_count = 0; s_overflow = false; }
int  vg_rast_prim_count(void)  { return s_count; }
bool vg_rast_overflowed(void)  { return s_overflow; }

static inline Prim* push(void) {
    if (s_count >= MAX_PRIMS) { s_overflow = true; return nullptr; }
    return &s_prims[s_count++];
}

// ---------------------------------------------------------------------------
// Colour
//
// Values are stored byte-swapped for the panel (see VGC in cfg_palette.h), so
// anything that interprets a colour has to swap in and back out. Affordable
// precisely because these run per object, never per pixel.
// ---------------------------------------------------------------------------

uint16_t vg_dim(uint16_t c, float f) {
    if (f >= 1.0f) return c;
    if (f <= 0.0f) return 0;

    uint16_t n = (uint16_t)((c >> 8) | (c << 8));
    uint32_t r = (uint32_t)(((n >> 11) & 0x1F) * f);
    uint32_t g = (uint32_t)(((n >> 5)  & 0x3F) * f);
    uint32_t b = (uint32_t)(( n        & 0x1F) * f);

    uint16_t out = (uint16_t)((r << 11) | (g << 5) | b);
    return (uint16_t)((out >> 8) | (out << 8));
}

uint16_t vg_mix(uint16_t a, uint16_t b, float t) {
    if (t <= 0.0f) return a;
    if (t >= 1.0f) return b;

    uint16_t na = (uint16_t)((a >> 8) | (a << 8));
    uint16_t nb = (uint16_t)((b >> 8) | (b << 8));

    float ar = (float)((na >> 11) & 0x1F), br = (float)((nb >> 11) & 0x1F);
    float ag = (float)((na >> 5)  & 0x3F), bg = (float)((nb >> 5)  & 0x3F);
    float ab = (float)( na        & 0x1F), bb = (float)( nb        & 0x1F);

    uint32_t r  = (uint32_t)(ar + (br - ar) * t);
    uint32_t g  = (uint32_t)(ag + (bg - ag) * t);
    uint32_t bl = (uint32_t)(ab + (bb - ab) * t);

    uint16_t o = (uint16_t)((r << 11) | (g << 5) | bl);
    return (uint16_t)((o >> 8) | (o << 8));
}

// ---------------------------------------------------------------------------
// Spherical HUD warp
// ---------------------------------------------------------------------------

static bool  s_warp   = false;
static float s_warp_k = HUD_WARP_K;

void vg_hud_warp(bool on, float scale) {
    s_warp   = on;
    s_warp_k = HUD_WARP_K * scale;
}

static inline void warp_pt(float* x, float* y) {
    float dx = *x - SCR_CX, dy = *y - SCR_CY;
    float r2 = (dx * dx + dy * dy) * (1.0f / (SCR_CX * SCR_CX + SCR_CY * SCR_CY));
    float k  = 1.0f + s_warp_k * r2;
    *x = SCR_CX + dx * k;
    *y = SCR_CY + dy * k;
}

// ---------------------------------------------------------------------------
// Orientation
//
// Applied after the warp, so everything upstream works in a single logical frame
// with the buttons at the top and never has to think about how the panel is
// actually scanned.
// ---------------------------------------------------------------------------

static inline void rot_pt(float* x, float* y) {
#if VG_ROTATE == 1
    float ox = *x, oy = *y;
    *x = oy;
    *y = (float)(SCR_H - 1) - ox;
#elif VG_ROTATE == 2
    *x = (float)(SCR_W - 1) - *x;
    *y = (float)(SCR_H - 1) - *y;
#elif VG_ROTATE == 3
    float ox = *x, oy = *y;
    *x = (float)(SCR_W - 1) - oy;
    *y = ox;
#else
    (void)x; (void)y;
#endif
}

// A quarter turn maps an axis-aligned rectangle to another axis-aligned
// rectangle with width and height exchanged, so fills stay cheap.
static inline void rot_rect(int* x, int* y, int* w, int* h) {
#if VG_ROTATE == 1
    int nx = *y, ny = SCR_H - *x - *w, nw = *h, nh = *w;
    *x = nx; *y = ny; *w = nw; *h = nh;
#elif VG_ROTATE == 2
    *x = SCR_W - *x - *w;
    *y = SCR_H - *y - *h;
#elif VG_ROTATE == 3
    int nx = SCR_W - *y - *h, ny = *x, nw = *h, nh = *w;
    *x = nx; *y = ny; *w = nw; *h = nh;
#else
    (void)x; (void)y; (void)w; (void)h;
#endif
}

// ---------------------------------------------------------------------------
// Cohen-Sutherland clip against the full screen, done once here so band
// rasterisation never walks an off-screen span.
// ---------------------------------------------------------------------------

static inline int outcode(float x, float y) {
    int c = 0;
    if      (x < 0)         c |= 1;
    else if (x > SCR_W - 1) c |= 2;
    if      (y < 0)         c |= 4;
    else if (y > SCR_H - 1) c |= 8;
    return c;
}

static bool clip_screen(float* px0, float* py0, float* px1, float* py1) {
    float ax = *px0, ay = *py0, bx = *px1, by = *py1;
    int c0 = outcode(ax, ay), c1 = outcode(bx, by);

    for (int guard = 0; guard < 8; guard++) {
        if (!(c0 | c1)) { *px0 = ax; *py0 = ay; *px1 = bx; *py1 = by; return true; }
        if (c0 & c1)    return false;

        int   c = c0 ? c0 : c1;
        float x = 0, y = 0;
        if (c & 8)      { y = SCR_H - 1; x = ax + (bx - ax) * (y - ay) / (by - ay); }
        else if (c & 4) { y = 0;         x = ax + (bx - ax) * (y - ay) / (by - ay); }
        else if (c & 2) { x = SCR_W - 1; y = ay + (by - ay) * (x - ax) / (bx - ax); }
        else            { x = 0;         y = ay + (by - ay) * (x - ax) / (bx - ax); }

        if (!isfinite(x) || !isfinite(y)) return false;

        if (c == c0) { ax = x; ay = y; c0 = outcode(ax, ay); }
        else         { bx = x; by = y; c1 = outcode(bx, by); }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Submit
// ---------------------------------------------------------------------------

static void line_raw(float x0, float y0, float x1, float y1, uint16_t color) {
    if (!color) return;
    rot_pt(&x0, &y0);
    rot_pt(&x1, &y1);
    if (!isfinite(x0) || !isfinite(y0) || !isfinite(x1) || !isfinite(y1)) return;
    if (!clip_screen(&x0, &y0, &x1, &y1)) return;

    Prim* p = push();
    if (!p) return;
    p->type  = PRIM_LINE;
    p->aa    = s_line_aa;
    p->x0    = (int16_t)lrintf(x0);
    p->y0    = (int16_t)lrintf(y0);
    p->x1    = (int16_t)lrintf(x1);
    p->y1    = (int16_t)lrintf(y1);
    p->color = color;
    p->ymin  = p->y0 < p->y1 ? p->y0 : p->y1;
    p->ymax  = p->y0 < p->y1 ? p->y1 : p->y0;
}

void vg_line(float x0, float y0, float x1, float y1, uint16_t color) {
    if (!s_warp) { line_raw(x0, y0, x1, y1, color); return; }

    // Subdivide, or the warp would just displace the endpoints and leave the
    // line straight between them.
    float dx = x1 - x0, dy = y1 - y0;
    float len = sqrtf(dx * dx + dy * dy);
    int   n   = (int)(len / HUD_WARP_SEG) + 1;
    if (n > 10) n = 10;

    float px = x0, py = y0;
    warp_pt(&px, &py);
    for (int i = 1; i <= n; i++) {
        float t  = (float)i / (float)n;
        float cx = x0 + dx * t, cy = y0 + dy * t;
        warp_pt(&cx, &cy);
        line_raw(px, py, cx, cy, color);
        px = cx; py = cy;
    }
}

void vg_line_w(float x0, float y0, float x1, float y1, uint16_t color, int w) {
    if (w <= 1) { vg_line(x0, y0, x1, y1, color); return; }

    float dx = x1 - x0, dy = y1 - y0;
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 1e-3f) { vg_line(x0, y0, x1, y1, color); return; }

    // Screen-space normal; offset copies straddle the true line so the stroke
    // stays centred on the geometry.
    float px = -dy / len, py = dx / len;
    float start = -(float)(w - 1) * 0.5f;
    for (int i = 0; i < w; i++) {
        float o = start + (float)i;
        vg_line(x0 + px * o, y0 + py * o, x1 + px * o, y1 + py * o, color);
    }
}

void vg_point(int x, int y, uint16_t color) {
    if (!color) return;
    {
        float fx = (float)x, fy = (float)y;
        rot_pt(&fx, &fy);
        x = (int)lrintf(fx);
        y = (int)lrintf(fy);
    }
    if ((unsigned)x >= (unsigned)SCR_W || (unsigned)y >= (unsigned)SCR_H) return;

    Prim* p = push();
    if (!p) return;
    p->type  = PRIM_POINT;
    p->x0    = (int16_t)x;
    p->y0    = (int16_t)y;
    p->x1    = 0;
    p->y1    = 0;
    p->color = color;
    p->ymin  = p->ymax = (int16_t)y;
}

static void fill_rect_raw(int x, int y, int w, int h, uint16_t color) {
    if (!color || w <= 0 || h <= 0) return;
    rot_rect(&x, &y, &w, &h);
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > SCR_W) w = SCR_W - x;
    if (y + h > SCR_H) h = SCR_H - y;
    if (w <= 0 || h <= 0) return;

    Prim* p = push();
    if (!p) return;
    p->type  = PRIM_FILL;
    p->x0    = (int16_t)x;
    p->y0    = (int16_t)y;
    p->x1    = (int16_t)w;
    p->y1    = (int16_t)h;
    p->color = color;
    p->ymin  = (int16_t)y;
    p->ymax  = (int16_t)(y + h - 1);
}

void vg_fill_rect(int x, int y, int w, int h, uint16_t color) {
    if (!s_warp) { fill_rect_raw(x, y, w, h, color); return; }
    if (!color || w <= 0 || h <= 0) return;

    // A warped rectangle is no longer axis-aligned, so it goes out as a strip of
    // quads (two triangles each), subdivided along its long axis so the bend is
    // visible rather than just a displaced box.
    const bool horiz = (w >= h);
    const int  span  = horiz ? w : h;
    int n = (int)((float)span / HUD_WARP_SEG) + 1;
    if (n > 8) n = 8;

    for (int i = 0; i < n; i++) {
        float a0 = (float)(horiz ? x : y) + (float)span * (float)i       / (float)n;
        float a1 = (float)(horiz ? x : y) + (float)span * (float)(i + 1) / (float)n;

        float qx[4], qy[4];
        if (horiz) {
            qx[0] = a0; qy[0] = (float)y;
            qx[1] = a1; qy[1] = (float)y;
            qx[2] = a1; qy[2] = (float)(y + h);
            qx[3] = a0; qy[3] = (float)(y + h);
        } else {
            qx[0] = (float)x;       qy[0] = a0;
            qx[1] = (float)(x + w); qy[1] = a0;
            qx[2] = (float)(x + w); qy[2] = a1;
            qx[3] = (float)x;       qy[3] = a1;
        }
        for (int k = 0; k < 4; k++) warp_pt(&qx[k], &qy[k]);

        // vg_tri applies the rotation, so these stay in logical space.
        vg_tri(qx[0], qy[0], qx[1], qy[1], qx[2], qy[2], color);
        vg_tri(qx[0], qy[0], qx[2], qy[2], qx[3], qy[3], color);
    }
}

void vg_rect(int x, int y, int w, int h, uint16_t color) {
    if (w <= 0 || h <= 0) return;
    vg_fill_rect(x,         y,         w, 1, color);
    vg_fill_rect(x,         y + h - 1, w, 1, color);
    vg_fill_rect(x,         y,         1, h, color);
    vg_fill_rect(x + w - 1, y,         1, h, color);
}

void vg_tri(float x0, float y0, float x1, float y1, float x2, float y2, uint16_t color) {
    if (!isfinite(x0) || !isfinite(y0) || !isfinite(x1) ||
        !isfinite(y1) || !isfinite(x2) || !isfinite(y2)) return;
    rot_pt(&x0, &y0);
    rot_pt(&x1, &y1);
    rot_pt(&x2, &y2);

    // Trivial reject against the screen.
    float minx = x0 < x1 ? (x0 < x2 ? x0 : x2) : (x1 < x2 ? x1 : x2);
    float maxx = x0 > x1 ? (x0 > x2 ? x0 : x2) : (x1 > x2 ? x1 : x2);
    float miny = y0 < y1 ? (y0 < y2 ? y0 : y2) : (y1 < y2 ? y1 : y2);
    float maxy = y0 > y1 ? (y0 > y2 ? y0 : y2) : (y1 > y2 ? y1 : y2);
    if (maxx < 0 || minx > SCR_W - 1 || maxy < 0 || miny > SCR_H - 1) return;

    // Vertices are stored unclipped so the scanline interpolation stays exact;
    // the per-band fill clamps spans instead. Clamping to +-16000 only bites for
    // geometry within half a unit of the near plane, which is already inside a
    // collision.
    #define TCLAMP(v) ((int16_t)((v) < -16000.0f ? -16000 : ((v) > 16000.0f ? 16000 : (int)lrintf(v))))

    Prim* p = push();
    if (!p) return;
    p->type  = PRIM_TRI;
    p->x0 = TCLAMP(x0); p->y0 = TCLAMP(y0);
    p->x1 = TCLAMP(x1); p->y1 = TCLAMP(y1);
    p->x2 = TCLAMP(x2); p->y2 = TCLAMP(y2);
    p->color = color;
    p->ymin = (int16_t)(miny < 0 ? 0 : (int)miny);
    p->ymax = (int16_t)(maxy > SCR_H - 1 ? SCR_H - 1 : (int)maxy);
    #undef TCLAMP
}

int vg_text_width(const char* s, int scale) {
    int n = 0;
    while (s[n]) n++;
    return n > 0 ? (n * 6 - 1) * scale : 0;
}

// NOTE: colour 0 means INVISIBLE here, not black -- passing COL_BLACK draws
// nothing at all. Inverse video (dark glyphs on a lit fill) must use INK_ONFILL,
// which is the palette entry that exists for exactly that.
void vg_text(int x, int y, const char* s, uint16_t color, int scale) {
    if (!color || scale <= 0) return;
    const int gh = 7 * scale;

    for (; *s; s++, x += 6 * scale) {
        char ch = *s;
        if (ch >= 'a' && ch <= 'z') ch -= 32;
        if (ch < VG_FONT_FIRST || ch > VG_FONT_LAST || ch == ' ') continue;
        if (x + 5 * scale < 0 || x >= SCR_W) continue;

        // Glyphs stay upright and unbent; only their origin follows the curve.
        // Warping the bitmaps themselves would cost far more and read worse at
        // this size than letting the baseline arc.
        float gx = (float)x, gy = (float)y;
        if (s_warp) warp_pt(&gx, &gy);
        rot_pt(&gx, &gy);

        Prim* p = push();
        if (!p) return;
        p->type  = PRIM_GLYPH;
        p->x0    = (int16_t)lrintf(gx);
        p->y0    = (int16_t)lrintf(gy);
        p->x1    = (int16_t)scale;
        p->y1    = (int16_t)ch;
        p->color = color;

        // The stored origin is the rotated LOGICAL top-left, so the glyph's
        // panel-space extent runs a different way per quadrant.
#if VG_ROTATE == 1
        p->ymin = (int16_t)(p->y0 - (5 * scale - 1));
        p->ymax = p->y0;
#elif VG_ROTATE == 2
        p->ymin = (int16_t)(p->y0 - (gh - 1));
        p->ymax = p->y0;
#elif VG_ROTATE == 3
        p->ymin = p->y0;
        p->ymax = (int16_t)(p->y0 + (5 * scale - 1));
#else
        p->ymin = p->y0;
        p->ymax = (int16_t)(p->y0 + gh - 1);
#endif
    }
}
