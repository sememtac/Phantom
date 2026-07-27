#pragma once
#include "vg_vec.h"
#include "vg_config.h"

// Camera-space projection, shared by the renderer (world -> screen) and by the
// weapon code (tap point -> firing direction). Keeping both directions in one
// place is what guarantees you shoot exactly where the crosshair says, bank and
// screen-shake included.

struct VgCam {
    float bank_s, bank_c;   // cosmetic roll
    float sx, sy;           // screen-shake offset, pixels
};

static inline VgCam vg_cam_make(float bank, float shake_x, float shake_y) {
    VgCam c;
    c.bank_s = sinf(bank);
    c.bank_c = cosf(bank);
    c.sx = shake_x;
    c.sy = shake_y;
    return c;
}

// View space is +x right, +y up, +z forward. Returns false for anything at or
// behind the near plane; callers that draw edges must clip in 3D first.
static inline bool vg_project(const VgCam& c, Vec3 p, float* out_x, float* out_y) {
    if (p.z < NEAR_Z) return false;
    float inv = FOCAL / p.z;
    float x = p.x * inv;
    float y = p.y * inv;
    *out_x = SCR_CX + (x * c.bank_c - y * c.bank_s) + c.sx;
    *out_y = SCR_CY - (x * c.bank_s + y * c.bank_c) + c.sy;
    return true;
}

// Exact inverse of vg_project's screen mapping, for a point on the projection
// plane: undo shake, undo bank, divide out the focal length.
static inline Vec3 vg_unproject_dir(const VgCam& c, float sx, float sy) {
    float rx =  (sx - SCR_CX - c.sx);
    float ry = -(sy - SCR_CY - c.sy);
    float x  =  rx * c.bank_c + ry * c.bank_s;
    float y  = -rx * c.bank_s + ry * c.bank_c;
    return vnorm(v3(x / FOCAL, y / FOCAL, 1.0f));
}
