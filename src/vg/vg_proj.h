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

// A world-space sphere as a screen-space circle: where its centre falls, and how
// wide it is there.
//
// Five drawing sites need exactly this and every one of them had written it out
// by hand, which is how three of them came to be wrong in two separate ways.
// Two measured with the fixed FOCAL, so they ignored the zoom and ignored the
// rear-view patch, whose focal length is a third of the window's. One divided by
// raw world z, which is NEGATIVE for anything behind the player: looking aft at
// a contact astern, the lock bracket was sized off a clamped near-plane depth
// and blew up to a radius well outside the screen, so the reticle simply was not
// there. Both faults are invisible in the one case anybody tests -- forward, no
// zoom, main window -- which is why they survived so long.
//
// So the arithmetic lives here once. It is four steps that must happen in this
// order, and the order is the part that is easy to get wrong: turn the point
// into THIS camera's space, reject it against the near plane, clamp the depth,
// and only then divide.
struct VgSpan {
    Vec3  v;        // the centre in this camera's view space
    float z;        // v.z clamped to the near plane, so it is safe to divide by
    float cx, cy;   // where the centre lands on screen; only if `vis`
    bool  vis;      // the centre itself projected. False does NOT mean nothing
                    // is worth drawing: it means the camera is close enough to
                    // be inside the object's reach, and a hull whose middle is
                    // behind the near plane can still have visible faces.
    float rpx;      // screen radius of `radius` at depth z
};

// `reach` is how far the body extends from its centre, and is used only to
// decide whether it is worth considering at all. `radius` is the length the
// caller wants measured on screen. They are the same number for a rock or a
// fireball and they are not for a ship, whose hull reaches three scale units
// while every size gate is written against one.
//
// Returns false when the whole body is behind the near plane. Anything else is
// the caller's decision, because the cheap-form ladders differ per site.
static inline bool vg_screen_size(const VgCam& c, Vec3 world, float radius,
                                  float reach, VgSpan* out) {
    out->v = vg_view(c, world);
    if (out->v.z + reach < NEAR_Z) return false;
    out->z   = out->v.z > NEAR_Z ? out->v.z : NEAR_Z;
    out->rpx = c.focal * radius / out->z;
    out->vis = vg_project(c, out->v, &out->cx, &out->cy);
    return true;
}
