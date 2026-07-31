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
    // Effective focal length. A real optical zoom rather than a model scale:
    // it magnifies the arena, the starfield and the ship alike, which is the
    // only way a push-in reads as a camera moving in rather than an object
    // growing. Flight always runs at 1.0 -- changing the field of view mid-fight
    // would break every judgement the player makes about closure and lead.
    float focal;
    // Looking aft. The simulation is already in view space -- the player sits at
    // the origin and the world counter-rotates around them -- so looking behind
    // is not a second scene or a second camera position. It is half a turn about
    // the vertical, which is two sign flips.
    //
    // A turn of the head, NOT a mirror. A mirror would flip z alone and leave
    // left and right swapped, which is what a real rear-view mirror does and
    // exactly the wrong thing here: the same view fills the screen when the
    // player holds the patch, and a full-screen mirrored world would make every
    // lead and every break turn the wrong way round.
    bool  rear;

    // A small repeater rather than the window. Drops what is decoration at 150
    // pixels wide: the contrails, which at that size are a bright smear across
    // the one instrument whose job is to show a shape closing on you, and which
    // are also among the heaviest things in the frame.
    bool  lite;
};

static inline VgCam vg_cam_make(float bank, float shake_x, float shake_y,
                                float zoom) {
    VgCam c;
    c.bank_s = sinf(bank);
    c.bank_c = cosf(bank);
    c.sx = shake_x;
    c.sy = shake_y;
    c.focal = FOCAL * ((zoom > 0.05f) ? zoom : 1.0f);
    c.rear  = false;
    c.lite  = false;
    return c;
}

// A view-space point as this camera sees it.
//
// MUST BE APPLIED BEFORE CULLING, not inside vg_project. The trivial reject and
// the near clip in vg_draw.h both read p.z directly and run before any
// projection, so a flip hidden inside the projection would leave everything
// behind the player rejected for being behind the player.
static inline Vec3 vg_view(const VgCam& c, Vec3 p) {
    if (!c.rear) return p;
    p.x = -p.x;
    p.z = -p.z;
    return p;
}

// View space is +x right, +y up, +z forward. Returns false for anything at or
// behind the near plane; callers that draw edges must clip in 3D first.
static inline bool vg_project(const VgCam& c, Vec3 p, float* out_x, float* out_y) {
    if (p.z < NEAR_Z) return false;
    float inv = c.focal / p.z;
    float x = p.x * inv;
    float y = p.y * inv;
    *out_x = SCR_CX + (x * c.bank_c - y * c.bank_s) + c.sx;
    *out_y = SCR_CY - (x * c.bank_s + y * c.bank_c) + c.sy;
    return true;
}
