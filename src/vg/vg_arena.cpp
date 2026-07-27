#include "vg_arena.h"
#include <math.h>

Arena vg_arena;

void vg_arena_init(ArenaKind kind) {
    vg_arena.kind = kind;
    vg_arena.ax   = v3(1, 0, 0);
    vg_arena.ay   = v3(0, 1, 0);
    vg_arena.az   = v3(0, 0, 1);

    if (kind == ARENA_TORUS) {
        vg_arena.r_major = ARENA_TORUS_RMAJ;
        vg_arena.r_minor = ARENA_TORUS_RMIN;
        // Put the arena origin off to one side so the player starts exactly on
        // the tube centreline at u=0 -- where the tunnel runs along +z, i.e.
        // straight ahead. You begin already flying down the corridor.
        vg_arena.center = v3(-ARENA_TORUS_RMAJ, 0, 0);
    } else {
        vg_arena.r_major = ARENA_SPHERE_R;
        vg_arena.r_minor = 0.0f;
        vg_arena.center  = v3(0, 0, 0);
    }
}

const char* vg_arena_name(void) {
    switch (vg_arena.kind) {
    case ARENA_TORUS: return "TORUS";
    default:          return "SPHERE";
    }
}

void vg_arena_step(const Mat3& R, float dz) {
    vg_arena.center = mat3_apply(R, vg_arena.center);
    vg_arena.center.z -= dz;

    vg_arena.ax = mat3_apply(R, vg_arena.ax);
    vg_arena.ay = mat3_apply(R, vg_arena.ay);

    // Re-orthonormalise every frame. Repeatedly folding a rotation into a
    // stored basis accumulates shear otherwise, and a skewed arena basis would
    // slowly warp the whole world.
    vg_arena.ax = vnorm(vg_arena.ax);
    vg_arena.ay = vnorm(vsub(vg_arena.ay, vmul(vg_arena.ax, vdot(vg_arena.ay, vg_arena.ax))));
    vg_arena.az = vcross(vg_arena.ax, vg_arena.ay);
}

Vec3 vg_arena_to_view(Vec3 l) {
    return vadd(vg_arena.center,
                vadd(vmul(vg_arena.ax, l.x),
                     vadd(vmul(vg_arena.ay, l.y), vmul(vg_arena.az, l.z))));
}

Vec3 vg_arena_dir_to_view(Vec3 d) {
    return vadd(vmul(vg_arena.ax, d.x),
                vadd(vmul(vg_arena.ay, d.y), vmul(vg_arena.az, d.z)));
}

Vec3 vg_arena_local_of(Vec3 p) {
    Vec3 rel = vsub(p, vg_arena.center);
    return v3(vdot(rel, vg_arena.ax), vdot(rel, vg_arena.ay), vdot(rel, vg_arena.az));
}

// ---------------------------------------------------------------------------
// Surface
// ---------------------------------------------------------------------------

Vec3 vg_arena_surf_t(float cu, float su, float cv, float sv) {
    if (vg_arena.kind == ARENA_TORUS) {
        // c(u) + r_minor * (cos v * radial(u) + sin v * up)
        float rr = vg_arena.r_major + vg_arena.r_minor * cv;
        return v3(rr * cu, vg_arena.r_minor * sv, rr * su);
    }
    float R = vg_arena.r_major;
    return v3(R * cv * cu, R * sv, R * cv * su);
}

Vec3 vg_arena_surf(float u, float v) {
    return vg_arena_surf_t(cosf(u), sinf(u), cosf(v), sinf(v));
}

void vg_arena_nearest(Vec3 p, float* u, float* v, float* clearance) {
    if (vg_arena.kind == ARENA_TORUS) {
        float uu = atan2f(p.z, p.x);
        float cu = cosf(uu), su = sinf(uu);
        // Vector from the tube centreline to the point.
        Vec3  w  = vsub(p, v3(vg_arena.r_major * cu, 0.0f, vg_arena.r_major * su));
        float radial = w.x * cu + w.z * su;          // dot(w, radial(uu))
        *u = uu;
        *v = atan2f(w.y, radial);
        *clearance = vg_arena.r_minor - vlen(w);
        return;
    }

    float r = vlen(p);
    Vec3  d = (r > 1e-4f) ? vmul(p, 1.0f / r) : v3(0, 0, 1);
    float y = d.y;
    if (y >  1.0f) y =  1.0f;
    if (y < -1.0f) y = -1.0f;
    *u = atan2f(d.z, d.x);
    *v = asinf(y);
    *clearance = vg_arena.r_major - r;
}

float vg_arena_clearance(Vec3 p) {
    if (vg_arena.kind == ARENA_TORUS) {
        float uu = atan2f(p.z, p.x);
        Vec3  w  = vsub(p, v3(vg_arena.r_major * cosf(uu), 0.0f,
                              vg_arena.r_major * sinf(uu)));
        return vg_arena.r_minor - vlen(w);
    }
    return vg_arena.r_major - vlen(p);
}

Vec3 vg_arena_inward(Vec3 p) {
    if (vg_arena.kind == ARENA_TORUS) {
        float uu = atan2f(p.z, p.x);
        Vec3  w  = vsub(p, v3(vg_arena.r_major * cosf(uu), 0.0f,
                              vg_arena.r_major * sinf(uu)));
        if (vlen2(w) < 1e-6f) return v3(0, 1, 0);
        return vmul(vnorm(w), -1.0f);
    }
    if (vlen2(p) < 1e-6f) return v3(0, 0, 1);
    return vmul(vnorm(p), -1.0f);
}

Vec3 vg_arena_clamp_inside(Vec3 view_pos, float margin) {
    Vec3  l = vg_arena_local_of(view_pos);
    float c = vg_arena_clearance(l);
    if (c >= margin) return view_pos;
    Vec3 inward = vg_arena_dir_to_view(vg_arena_inward(l));
    return vadd(view_pos, vmul(inward, margin - c));
}
