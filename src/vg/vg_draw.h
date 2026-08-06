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

// Where a round body stops being worth its geometry. Below DOT it is one pixel;
// below BLOB a diamond says "something is there" for four lines instead of the
// thirty a hull costs. Both are SCREEN radii, so they follow the camera by
// themselves -- the rear-view patch runs at a third of the window's focal length
// and its objects drop to the cheap forms sooner, which is right for a 145 pixel
// instrument.
//
// The asteroids and the fireballs share these. SHIPS DELIBERATELY DO NOT: they
// go to a dot at 2.0 and have no diamond tier at all, because a hull is the one
// thing in the frame whose shape is worth reading at any size, and because the
// cutscene model is 128 units where a fighter is 7 -- it was measured against
// the combat constant once and vanished into a single pixel at z=1400 while its
// trail carried on across the screen. Named here so the two that DO agree cannot
// drift apart, not to pull the third into line.
#define VG_LOD_DOT   2.5f
#define VG_LOD_BLOB  7.0f

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
// THE BOUNDARY WIREFRAME, IN TWO HALVES, because it is the largest single item on
// submit's critical path and its two loops are independent.
//
// Hoops run around the tube, rails run along it. Measured at 846 us and 426 us, and
// they share nothing: each arena_line carries its own incremental rotation. So the
// two can be built on different cores, which is the only reason this argument exists.
//
// The halves land in DIFFERENT primitive slices -- see SUB_AT in vg_raster.cpp -- and
// both slices are joined ahead of the world's, so the grid still lies under the hulls.
// ARENA_GRID_ALL is for the rear-view patch, which submits its own copy on one core.
#define ARENA_GRID_ALL   0
#define ARENA_GRID_HOOPS 1
#define ARENA_GRID_RAILS 2
void vg_draw_arena_grid(const VgCam& cam, int part);
void vg_draw_world(const VgCam& cam);
void vg_draw_hud(const VgCam& cam, const VgInput* in, float fps);
void vg_draw_overlays(void);

// HUD pieces that must NOT be warped, so the render layer calls them from outside
// the warp bracket. That is the only reason they are separate from vg_draw_hud.
//
// DECLARED HERE, not privately in vg_render.cpp where they used to be. The reason
// is legibility, and it is worth being precise about what it is NOT: these are
// C++, so a stale declaration cannot bind silently -- the mangled name changes and
// the link fails. Tested rather than assumed.
//
// What the move buys is that the interface is stated once, in the header both the
// caller and vg_hud.cpp already include, so a change is made in view of the
// declaration instead of in view of a copy. A return type or const mismatch is now
// a compile error at the definition rather than an undefined reference at the end
// of the build, and a new caller finds these where every other draw entry point
// lives.
void vg_draw_lock_box(const VgCam& cam);
void vg_draw_steer_indicator(const VgInput* in);
void vg_draw_target_markers(const VgCam& cam);
// The bounds and the size clamps are the caller's: this runs twice with very
// different room, the main window and the rear-view patch at 145x44.
void vg_draw_missile_markers(const VgCam& cam, float x0, float y0,
                             float x1, float y1, float hmin, float hmax);
void vg_draw_threat_indicator(const VgCam& cam);

// The broadcast caption. Drawn by the render layer rather than by the HUD,
// because the IFT speaks over the INTRO -- which has no instruments at all -- as
// well as over a finished match.
void vg_draw_ift(void);
