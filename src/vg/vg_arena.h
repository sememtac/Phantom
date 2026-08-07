#pragma once
#include <stdint.h>
#include "vg_vec.h"
#include "vg_config.h"

// The arena is the closed world the fight happens inside: a sphere, a torus
// tube, and so on. It gives the player somewhere to *be* instead of an
// undifferentiated starfield, and its wall is a hazard with a real cost.
//
// Boundaries are analytic, not meshes. Every shape is a parametric surface
// point(u,v) plus a "how far am I from it" query, which means the renderer can
// generate grid lines wherever it wants, at whatever density it wants, without
// storing any geometry. That is what makes proximity-adaptive detail cheap: to
// draw a finer grid we just evaluate more points.
//
// Like everything else, the arena lives in VIEW SPACE -- it carries a centre
// and an orthonormal basis that ride the same per-frame world transform as
// ships and asteroids.

// ARENA_SPHERE IS BUILT AND NOT USED. Every vg_arena_init call in the game passes
// ARENA_TORUS. The sphere has its surface, nearest-point, inward and patch-extent
// functions and its radius, and it works -- it is simply not the arena the game is
// played in, because a tunnel gives the fight depth and motion that the inside of a
// sphere does not.
//
// Kept on purpose, and worth keeping: it is the second shape, so it is what proves the
// arena code is not secretly torus-only. Anything added here should still satisfy both.
enum ArenaKind : uint8_t {
    ARENA_SPHERE = 0,   // inside a big hollow sphere -- implemented, never selected
    ARENA_TORUS,        // inside the tube of a doughnut: a closed-loop tunnel
    ARENA_KINDS
};

struct Arena {
    ArenaKind kind;
    Vec3      center;        // arena origin, view space
    Vec3      ax, ay, az;    // arena basis in view space, orthonormal
    float     r_major;       // sphere radius, or torus major radius
    float     r_minor;       // torus tube radius
};

extern Arena vg_arena;

void        vg_arena_init(ArenaKind kind);
void        vg_arena_step(const Mat3& R, float dz);   // ride the world transform

// --- frame conversions ---
Vec3 vg_arena_to_view(Vec3 local);        // arena-local point  -> view space
Vec3 vg_arena_dir_to_view(Vec3 local_d);  // arena-local vector -> view space
Vec3 vg_arena_local_of(Vec3 view_pos);    // view-space point   -> arena-local

// --- surface queries, all in arena-local coordinates ---
// Same, but from precomputed trig. Lets a caller walk a polyline with an
// incremental rotation (4 trig calls per line) instead of a sin/cos pair per
// point (2 per segment) -- the grid is dense enough that this dominates.
Vec3  vg_arena_surf_t(float cu, float su, float cv, float sv);
void  vg_arena_nearest(Vec3 local, float* u, float* v, float* clearance);
float vg_arena_clearance(Vec3 local);     // >0 inside, <0 outside
Vec3  vg_arena_inward(Vec3 local);        // unit normal pointing back inside

// Push a point back inside the boundary if it is closer than `margin`. Used so
// spawns cannot land in, or beyond, the wall.
Vec3  vg_arena_clamp_inside(Vec3 view_pos, float margin);
