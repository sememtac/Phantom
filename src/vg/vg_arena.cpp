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

// ---------------------------------------------------------------------------
// The boundary's displacement. See ARENA_WARP_NU in cfg_world.h.
// ---------------------------------------------------------------------------

static float s_warp_k = 0.0f;      // 0..1, scaled by ARENA_WARP_MAX

bool vg_arena_warped(void) {
    return s_warp_k > 0.0f && vg_arena.kind == ARENA_TORUS;
}

void vg_arena_warp_set(float k) {
    if (k < 0.0f) k = 0.0f; else if (k > 1.0f) k = 1.0f;
    s_warp_k = k;
}

// A hash, not a stored lattice. A table would be four loads per vertex and would have to live
// in internal SRAM to be worth having, and internal SRAM is the scarcest memory on this part --
// about 10 KB free, and the backdrop has already been broken once by spending it. This is a
// multiply, a shift and an xor, and it needs no memory at all.
static inline float warp_hash(int i, int j) {
    uint32_t h = (uint32_t)i * 374761393u + (uint32_t)j * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    // -1..1
    return (float)(int32_t)h * (1.0f / 2147483648.0f);
}

static inline float warp_smooth(float t) { return t * t * (3.0f - 2.0f * t); }

float vg_arena_warp(float u, float v) {
    if (s_warp_k <= 0.0f || vg_arena.kind != ARENA_TORUS) return 0.0f;

    const float TAU = 6.28318531f;
    float amp = vg_arena.r_minor * ARENA_WARP_MAX * s_warp_k;
    float sum = 0.0f, norm = 0.0f;
    int   nu = ARENA_WARP_NU, nv = ARENA_WARP_NV;
    float a = 1.0f;

    for (int o = 0; o < ARENA_WARP_OCTAVES; o++) {
        // Lattice coordinates.
        float fu = u * (float)nu * (1.0f / TAU);
        float fv = v * (float)nv * (1.0f / TAU);
        // FLOOR WITHOUT floorf, which is a call out to libm and was a fifth of this function.
        // A cast truncates toward zero, so it is already the floor for a positive value and one
        // too high for a negative one. The angles ARE negative -- atan2 returns -pi..pi -- so
        // the correction is not a formality.
        int   iu = (int)fu; if (fu < (float)iu) iu--;
        int   iv = (int)fv; if (fv < (float)iv) iv--;
        float tu = warp_smooth(fu - (float)iu), tv = warp_smooth(fv - (float)iv);
        // The wrap is what makes the field PERIODIC, which is what makes the tunnel join
        // itself. A mask does it because the lattice is a power of two -- see ARENA_WARP_NU --
        // and a mask is also correct for a negative index, where a remainder is not.
        int   u0 = iu & (nu - 1), u1 = (u0 + 1) & (nu - 1);
        int   v0 = iv & (nv - 1), v1 = (v0 + 1) & (nv - 1);

        float h00 = warp_hash(u0, v0), h10 = warp_hash(u1, v0);
        float h01 = warp_hash(u0, v1), h11 = warp_hash(u1, v1);
        float t0  = h00 + (h10 - h00) * tu;
        float t1  = h01 + (h11 - h01) * tu;
        sum  += a * (t0 + (t1 - t0) * tv);
        norm += a;
        a    *= 0.5f;
        nu   *= 2; nv *= 2;
    }
    return amp * sum / norm;
}

Vec3 vg_arena_surf_t(float cu, float su, float cv, float sv, float dr) {
    if (vg_arena.kind == ARENA_TORUS) {
        // c(u) + rm * (cos v * radial(u) + sin v * up), where rm is the tube's radius HERE --
        // the displacement moves the surface along that same normal, which is the whole reason
        // the clearance solve does not have to change shape. See ARENA_WARP_NU.
        const float rm = vg_arena.r_minor + dr;
        const float rr = vg_arena.r_major + rm * cv;
        return v3(rr * cu, rm * sv, rr * su);
    }
    const float R = vg_arena.r_major + dr;
    return v3(R * cv * cu, R * sv, R * cv * su);
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
        *clearance = vg_arena.r_minor + vg_arena_warp(uu, *v) - vlen(w);
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
        float cu = cosf(uu), su = sinf(uu);
        Vec3  w  = vsub(p, v3(vg_arena.r_major * cu, 0.0f, vg_arena.r_major * su));
        // THE WALL IS WHERE IT LOOKS. Without this the boundary would kill the player at a
        // radius the drawn tunnel disagrees with, which is worse than either a smooth arena
        // or a displaced one. The extra atan2 recovers v; the nearest point on the tube axis
        // is unchanged, so this is still O(1) -- see ARENA_WARP_NU.
        if (s_warp_k > 0.0f) {
            const float radial = w.x * cu + w.z * su;
            const float vv     = atan2f(w.y, radial);
            return vg_arena.r_minor + vg_arena_warp(uu, vv) - vlen(w);
        }
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
