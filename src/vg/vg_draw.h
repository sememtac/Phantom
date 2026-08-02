#pragma once
#include "vg_vec.h"
#include "vg_proj.h"
#include "vg_raster.h"
#include "vg_input.h"

// Shared drawing helpers and the entry point of each draw module.
//
// The renderer is split by SUBJECT rather than by primitive type, because that
// is how it actually gets edited: the arena grid, the world objects, the
// instruments and the full-screen overlays change for completely unrelated
// reasons and share almost nothing but the projection.

// Trivial-reject bound, as a half-extent per unit of depth. Uses the screen's
// CIRCUMSCRIBED radius rather than its half-width, which makes the test
// invariant to bank -- a rolled camera rotates the frustum, and a tight
// rectangular bound would start clipping geometry that is still visible.
#define VG_CULL_K  ((SCR_CX * 1.4143f + 8.0f) / FOCAL)

// Cohen-Sutherland style outcode in VIEW space, before any projection. Points
// behind the near plane return 0 so they never contribute to a reject -- the
// near clip deals with those.
static inline int vg_cull_code(Vec3 p) {
    if (p.z < NEAR_Z) return 0;
    const float lim = VG_CULL_K * p.z;
    int c = 0;
    if (p.x < -lim) c |= 1; else if (p.x > lim) c |= 2;
    if (p.y < -lim) c |= 4; else if (p.y > lim) c |= 8;
    return c;
}

// Draw a 3D segment, clipping against the near plane FIRST. Without that clip an
// edge straddling z=0 projects to a wild coordinate and streaks across screen.
static inline void vg_edge_w(const VgCam& cam, Vec3 a, Vec3 b, uint16_t col, int w) {
    // Aft view first, so everything below -- the near test, the trivial reject,
    // the clip -- is working in the space the camera is actually looking at.
    a = vg_view(cam, a);
    b = vg_view(cam, b);
    if (a.z < NEAR_Z && b.z < NEAR_Z) return;

    // Reject before paying for two divides and a screen clip. Worth it because
    // the arena grid and missile trails generate far more segments than land on
    // screen -- and submit is the stage that bills frame time directly.
    if (vg_cull_code(a) & vg_cull_code(b)) return;
    if (a.z < NEAR_Z) {
        float t = (NEAR_Z - a.z) / (b.z - a.z);
        a = vadd(a, vmul(vsub(b, a), t));
    } else if (b.z < NEAR_Z) {
        float t = (NEAR_Z - b.z) / (a.z - b.z);
        b = vadd(b, vmul(vsub(a, b), t));
    }
    float ax, ay, bx, by;
    if (!vg_project(cam, a, &ax, &ay)) return;
    if (!vg_project(cam, b, &bx, &by)) return;
    vg_line_w(ax, ay, bx, by, col, w);
}

static inline void vg_edge(const VgCam& cam, Vec3 a, Vec3 b, uint16_t col) {
    vg_edge_w(cam, a, b, col, 1);
}

// Four strokes with their corners on the axes. A diamond and not a box, which is
// a distinction the HUD relies on: the lock brackets own the square, so anything
// that is a marker rather than a target wears this instead.
//
// Written out three times before it lived here, at two stroke widths, and in two
// different rotations of the same cycle -- which is harmless while the primitive
// list has room and not otherwise, since the strokes that land before the ceiling
// are the ones that survive. One order for all three now.
//
// vg_line_w falls through to vg_line at w <= 1, so the thin sites pay nothing.
static inline void vg_diamond(float cx, float cy, float r, uint16_t col, int w) {
    vg_line_w(cx,     cy - r, cx + r, cy,     col, w);
    vg_line_w(cx + r, cy,     cx,     cy + r, col, w);
    vg_line_w(cx,     cy + r, cx - r, cy,     col, w);
    vg_line_w(cx - r, cy,     cx,     cy - r, col, w);
}

// Distance haze, as a brightness multiplier. Near things are full strength, far
// things bottom out at `floor` rather than going to black, because a contact that
// fades to nothing is indistinguishable from one that is not there -- and the
// difference between those two matters more than the depth cue does.
//
// `over` is the depth at which brightness would reach zero if it were not
// clamped, and the curve deliberately starts above 1.0 so that everything inside
// a working distance is equally bright and the ramp only bites further out.
static inline float vg_fade(float z, float bias, float over, float floor_) {
    float f = bias - z / over;
    if (f > 1.0f)   f = 1.0f;
    if (f < floor_) f = floor_;
    return f;
}

// Draw order matters and is set by vg_render_frame: starfield, then the arena
// grid over it, then everything solid, whose hidden-line fills occlude both.
void vg_draw_starfield(const VgCam& cam);
void vg_draw_arena_grid(const VgCam& cam);
void vg_draw_world(const VgCam& cam);
void vg_draw_hud(const VgCam& cam, const VgInput* in, float fps);
void vg_draw_overlays(void);

// The broadcast caption. Drawn by the render layer rather than by the HUD,
// because the IFT speaks over the INTRO -- which has no instruments at all -- as
// well as over a finished match.
void vg_draw_ift(void);
