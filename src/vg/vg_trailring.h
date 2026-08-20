#pragma once
#include <stdint.h>
#include "vg_vec.h"
#include "vg_config.h"

// ===========================================================================
// THE RIBBON RING, ONCE
//
// Four things in this game leave a track behind them -- the player, the enemy
// fighters, the cutscene ship and every missile in the air -- and each carried
// its own copy of the same ring buffer: an accumulator, a count, a head, the
// points. Same fields, same append, same modulo walk, written out four times,
// so a fix to one was a fix you had to remember to make three more times.
//
// This is HYGIENE, not speed. All the trails together were measured at about
// 2 us of raster; nothing here is faster than what it replaced, and nothing is
// allowed to be DIFFERENT either -- the pixel-hash regression holds this module
// to the exact floats the four copies produced. Which is why the sampler takes
// its interval as a parameter instead of the four owners agreeing on one: the
// ships lay track at SHIP_TRAIL_DT, the cutscene ship at half that, missiles on
// their own clock entirely, and harmonising them would change the picture.
//
// Plain aggregates, deliberately. The rings inside Ship and Missile are wiped
// by the memset at the top of vg_game_init, so the type has to stay trivially
// zeroable -- no constructors, ever.
// ===========================================================================

// A track with per-point emission power. `p` is the throttle setting each
// point was laid down at, 0..255 -- what makes a contrail lengthen under power
// and persist after the ship has backed off.
template <int N> struct TrailRing {
    float   acc;
    uint8_t n;
    uint8_t head;
    uint8_t p[N];
    Vec3    pt[N];
};

// A track without one. Missiles burn at constant brightness -- their fade is a
// curve over age alone -- and fourteen of them carrying thirty dead power
// bytes each would be 420 bytes of RAM saying nothing.
template <int N> struct TrailRingBare {
    float   acc;
    uint8_t n;
    uint8_t head;
    Vec3    pt[N];
};

typedef TrailRing<SHIP_TRAIL>        ShipTrailRing;
typedef TrailRingBare<MISSILE_TRAIL> MissileTrailRing;

// Back to no track. The explicit form of what the memset does. Callers that
// used to zero acc/n/head by hand come through here now; wiping the point
// arrays as well changes nothing anyone can see, because the walk below never
// reads past `n` and every slot under `n` is a fresh append.
template <typename R> static inline void trail_clear(R& r) { r = R{}; }

// Ride the frame's world transform. Trails are world geometry, so every stored
// point takes the same rotation and recession the objects do -- otherwise a
// ribbon would smear sideways the moment its owner manoeuvred instead of
// staying pinned to the track that was actually flown. The cutscene ship rides
// the rotation but not the translation and passes dz = 0, which subtracts
// nothing, exactly.
template <int N>
static inline void trail_advect(TrailRing<N>& r, const Mat3& M, float dz) {
    for (int t = 0; t < r.n; t++) {
        int idx = (r.head - t + N * 2) % N;
        r.pt[idx] = mat3_apply(M, r.pt[idx]);
        r.pt[idx].z -= dz;
    }
}
template <int N>
static inline void trail_advect(TrailRingBare<N>& r, const Mat3& M, float dz) {
    for (int t = 0; t < r.n; t++) {
        int idx = (r.head - t + N * 2) % N;
        r.pt[idx] = mat3_apply(M, r.pt[idx]);
        r.pt[idx].z -= dz;
    }
}

// Lay track. The accumulator gathers real frame time and a point goes down
// each time it crosses `interval`, so the ribbon is spaced by TIME, not by
// frames, and a hitch cannot bunch it. `pos` and `power` are whatever is true
// on the frame the sample fires; callers pass them every frame and the ring
// keeps the ones that land. Call AFTER trail_advect -- a point laid first
// would ride a transform it was not there for.
template <int N>
static inline void trail_sample(TrailRing<N>& r, float dt, Vec3 pos,
                                uint8_t power, float interval) {
    r.acc += dt;
    if (r.acc >= interval) {
        r.acc = 0;
        r.head = (uint8_t)((r.head + 1) % N);
        r.pt[r.head] = pos;
        r.p[r.head]  = power;
        if (r.n < N) r.n++;
    }
}
template <int N>
static inline void trail_sample(TrailRingBare<N>& r, float dt, Vec3 pos,
                                float interval) {
    r.acc += dt;
    if (r.acc >= interval) {
        r.acc = 0;
        r.head = (uint8_t)((r.head + 1) % N);
        r.pt[r.head] = pos;
        if (r.n < N) r.n++;
    }
}
