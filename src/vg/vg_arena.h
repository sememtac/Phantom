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
//
// AND THE WARP DOES NOT, which is worth stating rather than leaving to be discovered.
// The sphere answers for every other query -- init, surf_t, nearest, clearance and inward
// all handle it, and surf_t already applies a radial displacement to it. vg_arena_warp is
// the one thing that refuses, and it refuses for a geometric reason and not an oversight:
//
// The field is PERIODIC IN BOTH PARAMETERS by construction, which is what makes the tunnel
// join itself. A torus has that: u and v both run the full turn and both wrap. A sphere
// does not. Its v is asinf(y), so it spans half a turn and never wraps, and at the two
// poles every u names the SAME POINT while the field still varies with u -- so a displaced
// sphere would tear open at both poles. Fixing that needs a field that collapses to one
// value there, which is a different field, not this one with another radius on it.
//
// So the rule above holds with one recorded exception, and the exception is the newest
// thing the arena grew. If a third shape arrives, this is the question to ask of it first.
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
// A point on the boundary, from the sines and cosines of its two angles, displaced radially
// by `dr` -- 0 for the smooth tube the course is flown in.
//
// The displacement arrives as a distance and not as the angles it is a function of, because
// the caller already holds those. Recovering them here would be an atan2 per vertex, which is
// more than the whole displacement costs. See vg_arena_warp.
Vec3  vg_arena_surf_t(float cu, float su, float cv, float sv, float dr);

// HOW FAR THE WALL MOVES AT (u, v), in world units. Zero when the arena is smooth.
//
// Both arguments are angles in radians and need no wrapping: the lattice is periodic, so
// any u and any v land somewhere sensible.
float vg_arena_warp(float u, float v);

// Is the boundary displaced at all? Worth asking once per line rather than paying a call per
// vertex to be told no: most of the game -- the menus, the attract loop, the course -- flies a
// round tube, and the calls alone cost 34us of `sub` there with nothing to show for them.
bool  vg_arena_warped(void);

// Set every frame while an anomaly is running, and once when a match or a course run begins. 0 is a perfectly round tube -- see
// ARENA_WARP_MAX for what 1 means, and note that it is a FRACTION of r_minor, not units.
void  vg_arena_warp_set(float k);
void  vg_arena_nearest(Vec3 local, float* u, float* v, float* clearance);
float vg_arena_clearance(Vec3 local);     // >0 inside, <0 outside
Vec3  vg_arena_inward(Vec3 local);        // unit normal pointing back inside

// Push a point back inside the boundary if it is closer than `margin`. Used so
// spawns cannot land in, or beyond, the wall.
Vec3  vg_arena_clamp_inside(Vec3 view_pos, float margin);
