#include "vg_draw.h"
#include "vg_game.h"
#include "vg_arena.h"
#include <math.h>

// The boundary wireframe. Kept apart from the world objects because it is
// generated rather than drawn from stored geometry, and its cost profile is
// completely different: hundreds of procedurally generated segments a frame, all
// of it landing in `submit`, which is the stage that actually costs frame time.

// One grid segment, already in view space, faded by distance. The player sits at
// the origin, so a view-space point's length IS its distance from the ship.
static void arena_seg(const VgCam& cam, Vec3 a, Vec3 b, uint16_t base) {
    // TESTED in the direction the camera is pointing, DRAWN from the untouched
    // points -- vg_edge turns them itself, and turning them twice is not turning
    // them at all.
    //
    // These two rejects duplicate vg_edge's on purpose, to save the sqrt and the
    // colour work below on a segment that will not land. Reading the untouched z
    // is what made the rear view show no boundary at all: "wholly behind" is
    // exactly the half of the grid that a pilot looking aft is looking at, and
    // the comment below is a fair description of how much of it that is.
    const Vec3 va = vg_view(cam, a), vb = vg_view(cam, b);

    // Reject wholly-behind segments before paying for the sqrt and the colour
    // scale -- with the player inside a closed world, roughly half the grid is
    // behind them at any moment.
    if (va.z < NEAR_Z && vb.z < NEAR_Z) return;

    // Reject here too, not just inside vg_edge: everything below -- a sqrt, a
    // colour scale and possibly a mix -- is wasted on a segment that is never
    // going to land on screen, and most of this grid does not.
    if (vg_cull_code(va) & vg_cull_code(vb)) return;

    float d = vlen(vmul(vadd(a, b), 0.5f));
    float f = 1.15f - d / ARENA_FADE_FAR;
    if (f > 1.0f)            f = 1.0f;
    if (f < ARENA_FADE_MIN)  f = ARENA_FADE_MIN;

    uint16_t col = vg_dim(base, f);

    // Blend toward red as the ship closes on the boundary, weighted by this
    // segment's own distance so only the wall you are actually approaching lights
    // up. A uniform tint would say "danger" without saying "that way".
    if (vg.wall_clear < ARENA_DANGER_RANGE) {
        float danger = 1.0f - vg.wall_clear / ARENA_DANGER_RANGE;
        float near_w = 1.0f - d / (ARENA_DANGER_RANGE * 2.2f);
        if (danger > 0.0f && near_w > 0.0f)
            col = vg_mix(col, COL_DANGER, danger * near_w);
    }

    // The grid does not antialias, at any distance.
    //
    // Measured, after getting this wrong once by gating on brightness: an
    // antialiased span costs per PIXEL, so its price tracks length, and
    // brightness tracks distance -- the opposite. Culling AA from the far
    // segments saved nothing because those are the short ones; the near
    // segments are the ones that stripe half the screen at ~100us each. With
    // barely a hundred primitives on screen the grid alone was already burning
    // 2.9ms a frame.
    //
    // Dropping it wholesale is the right call because the jitter that bought
    // antialiasing in the first place was on the INSTRUMENTS -- the throttle
    // rail and the hull frame, static geometry a few pixels wide where a
    // half-pixel crawl is obvious. Those keep it. The arena is moving structure
    // at distance and nothing about it holds still long enough to shimmer.
    vg_line_aa_mode(false);
    vg_edge(cam, a, b, col);
}

// A polyline along the surface with `u` fixed (along_v) or `v` fixed.
//
// The varying angle is advanced by an incremental rotation rather than calling
// sin/cos per point: 4 trig calls per line instead of 2 per segment. With a few
// hundred segments a frame that difference was most of the arena's cost. Drift is
// irrelevant over the <= 32 steps any one line uses.
static void arena_line(const VgCam& cam, bool along_v,
                       float fixed, float from, float to, int segs, uint16_t col) {
    if (segs < 1) return;
    const float step = (to - from) / (float)segs;

    const float cf = cosf(fixed), sf = sinf(fixed);
    const float dc = cosf(step),  ds = sinf(step);
    float c = cosf(from), s = sinf(from);

    // Transform to view space once per POINT, not once per segment endpoint --
    // consecutive segments share a vertex, so the naive form does it twice.
    Vec3 prev = vg_arena_to_view(along_v ? vg_arena_surf_t(cf, sf, c, s)
                                         : vg_arena_surf_t(c, s, cf, sf));

    for (int i = 1; i <= segs; i++) {
        float nc = c * dc - s * ds;
        float ns = s * dc + c * ds;
        c = nc; s = ns;
        Vec3 cur = vg_arena_to_view(along_v ? vg_arena_surf_t(cf, sf, c, s)
                                            : vg_arena_surf_t(c, s, cf, sf));
        arena_seg(cam, prev, cur, col);
        prev = cur;
    }
}

// A single structural grid over the whole boundary. Proximity is conveyed by
// arena_seg's distance fade -- the wall you are closing on simply gets brighter
// and redder -- rather than by overlaying extra geometry near it.
void vg_draw_arena_grid(const VgCam& cam) {
    const float PI  = 3.14159265f;
    const float TAU = 6.28318531f;

    // Only the torus needs this, to centre its hoop window on the player.
    Vec3  pl = vg_arena_local_of(v3(0, 0, 0));
    float u0, v0, clearance;
    vg_arena_nearest(pl, &u0, &v0, &clearance);
    (void)v0; (void)clearance;   // the torus only needs u0, to centre its hoops

    const uint16_t col_struct = COL_ARENA;

    if (vg_arena.kind == ARENA_TORUS) {
        // Hoops around the tube, spaced by arc length along the tunnel and drawn
        // only within a window ahead of and behind the player. Drawing the whole
        // ring would be mostly wasted -- and the receding hoops are what sell it
        // as a corridor.
        float du_hoop = ARENA_HOOP_SPACING / vg_arena.r_major;
        int   nhoop   = (int)(ARENA_HOOP_SPAN / ARENA_HOOP_SPACING);
        // Snap to a global grid so hoops stay put in the world instead of sliding
        // along with the player.
        int   base    = (int)floorf(u0 / du_hoop);

        // The mirror gets half the hoops and half the rails. It is a 145 pixel
        // instrument: the corridor still reads -- receding rings are what sell
        // it -- but at that size nobody counts them, and the full grid was a
        // millisecond of submission for the second time in the same frame.
        // Every other hoop from an EVEN base, so the set drawn is stable as the
        // window slides rather than flickering between two interleavings.
        const int hs = cam.lite ? 2 : 1;
        const int b0 = cam.lite ? (base - (base & 1)) : base;

        for (int i = -nhoop; i <= nhoop; i += hs) {
            float u = (float)(b0 + i) * du_hoop;
            arena_line(cam, true, u, 0.0f, TAU, ARENA_HOOP_SEGS, col_struct);
        }
        for (int j = 0; j < ARENA_RAILS; j += hs) {
            float v = TAU * (float)j / (float)ARENA_RAILS;
            arena_line(cam, false, v, u0 - du_hoop * nhoop, u0 + du_hoop * nhoop,
                       nhoop * 2, col_struct);
        }
    } else {
        // Sphere: meridians and parallels at ~20 degrees. That is the density
        // needed for a few grid lines to sit in a 62-degree FOV whichever way you
        // look -- at 36 degrees you could face a gap and see nothing at all.
        const int NMER = 16;
        for (int i = 0; i < NMER; i++)
            arena_line(cam, true, TAU * (float)i / (float)NMER,
                       -PI * 0.5f, PI * 0.5f, 14, col_struct);

        const int NPAR = 9;
        for (int j = 1; j <= NPAR; j++) {
            float v = -PI * 0.5f + PI * (float)j / (float)(NPAR + 1);
            // Parallels shrink towards the poles; scale their segment count so a
            // small circle does not get segments it cannot show.
            int segs = (int)(26.0f * cosf(v));
            if (segs < 8) segs = 8;
            arena_line(cam, false, v, 0.0f, TAU, segs, col_struct);
        }
    }

    // arena_seg leaves the flag wherever the last segment put it.
    vg_line_aa_mode(true);
}
