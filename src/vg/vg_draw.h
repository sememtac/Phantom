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

// Draw order matters and is set by vg_render_frame: starfield, then the arena
// grid over it, then everything solid, whose hidden-line fills occlude both.
void vg_draw_starfield(const VgCam& cam);
void vg_draw_arena_grid(const VgCam& cam);
void vg_draw_world(const VgCam& cam);
void vg_draw_hud(const VgCam& cam, const VgInput* in, float fps);
void vg_draw_overlays(void);
