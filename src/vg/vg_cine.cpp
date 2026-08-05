#include "vg_cine.h"
#include "vg_sim.h"
#include "vg_arena.h"
#include "vg_sky.h"
#include <math.h>

// The whole shot rests on one property of this renderer: the player IS the
// origin, so there is no camera to move or aim. Both are done by steering the
// world instead -- which is exactly what a camera operator does, and is why a
// fixed viewpoint tracking a subject falls out of the existing flight model
// rather than needing a second one.

static float s_turn = 0.0f;    // lazy arc on the ship, set per shot

void vg_cine_clear(void) {
    vg.cine.on         = false;
    vg.cine.gate_t          = 0.0f;
    vg.cine.hold       = 0.0f;
    vg.cine.ship.trail_n    = 0;
    vg.cine.ship.trail_head = 0;
    vg.cine.ship.trail_acc  = 0;
}

// A fly-by past a fixed position, the way a rally camera is planted at the
// outside of a corner: in from a long way down the tunnel, across at about 470
// units, and on BEHIND the camera -- which is what makes the operator whip
// round after it.
//
// Departing shots were the mistake before this. A ship that only ever recedes
// keeps the same bearing, so the camera barely turns, and the whole thing reads
// as chase footage flying along behind it. Nothing whizzes past unless it
// actually goes past.
//
// Drawn well above combat size: at ENEMY_SCALE a fighter is about five pixels
// across at any distance the near plane allows, which is fine for a contact and
// useless for a subject.
static void cine_launch(const ShipSpec* spec, float hue, bool mirror) {
    Ship* c = &vg.cine.ship;
    const float sx = mirror ? -1.0f : 1.0f;

    c->alive = true;
    c->spec  = spec;
    c->hue   = hue;
    c->scale = 112.0f;

    c->pos   = v3(sx * 640.0f, 100.0f, 3490.0f);
    c->fwd   = vnorm(v3(sx * -0.30f, -0.05f, -0.95f));
    c->up    = v3(0, 1, 0);
    c->speed = 815.0f;
    c->roll_vis = sx * 0.5f;

    c->trail_n = c->trail_head = 0;
    c->trail_acc = 0;

    // The gate opens first and the ship waits behind it. Building the plane's
    // axes from the ship's heading means cross(gate_r, gate_u) is that heading
    // again, so on release the ship can be re-seated straight off the gate --
    // wherever the world has rotated it to by then.
    Vec3 r = vcross(v3(0, 1, 0), c->fwd);
    if (vlen2(r) < 1e-4f) r = vcross(v3(1, 0, 0), c->fwd);
    vg.cine.gate_r   = vnorm(r);
    vg.cine.gate_u   = vnorm(vcross(c->fwd, vg.cine.gate_r));
    vg.cine.gate_pos = c->pos;
    vg.cine.gate_hue = hue;
    vg.cine.gate_t   = GATE_TIME;

    vg.cine.hold = GATE_EMERGE;
    vg.cine.on   = false;      // nothing to see until the gate has opened
}

// Move the whole shot somewhere else. The two fighters launch from separate
// places, and cutting between them without relocating showed the same stretch
// of tunnel twice -- which reads as the pair starting on top of each other
// rather than a corridor apart.
static void cine_relocate(void) {
    const Mat3 R = mat3_euler(vg_frand(-0.55f, 0.55f), vg_frand(-1.4f, 1.4f), 0.0f);

    // Rotation first, and rotation alone is always safe: turning about the
    // camera cannot change where the camera sits inside the tube.
    vg_arena_step(R, 0.0f);

    // The jump down the tunnel is NOT safe, because after an arbitrary turn
    // "forward" may point straight at the wall. So it is attempted and then
    // checked, and backed out if it left the arena. Blindly stepping a few
    // thousand units was one of the two ways this could strand the viewpoint
    // outside the torus and kill the player the instant the match began.
    const float dz = vg_frand(2600.0f, 4400.0f);
    const Mat3  I  = mat3_euler(0.0f, 0.0f, 0.0f);
    vg_arena_step(I, dz);
    if (vg_arena_clearance(vg_arena_local_of(v3(0, 0, 0))) < ARENA_ATTRACT_MARGIN)
        vg_arena_step(I, -dz);

    vg.wall_clear = vg_arena_clearance(vg_arena_local_of(v3(0, 0, 0)));

    for (int i = 0; i < NUM_STARS; i++) vg.star[i] = mat3_apply(R, vg.star[i]);
    // Near-field dust has no business surviving a jump.
    for (int i = 0; i < NUM_MOTES; i++) vg.mote[i] = vg_mote_spawn(MOTE_Z_MIN, MOTE_Z_MAX);

    // The backdrop is at infinity, so only the turn applies to it.
    vg_sky_step(vg_frand(-0.9f, 0.9f), vg_frand(-1.6f, 1.6f), vg.bank);
}

// Advance along its own track and lay ribbon. Runs AFTER the world step, which
// has already turned both the ship and the background by this frame's pan.
static void cine_fly(float dt) {
    Ship* c = &vg.cine.ship;

    const Mat3 T = mat3_euler(-0.04f * dt, s_turn * dt, 0.0f);
    c->fwd = vnorm(mat3_apply(T, c->fwd));

    // Re-orthonormalise UP against the new heading, as the AI does. Turning fwd
    // without this leaves the pair skewed, vg_ship_basis builds a matrix that is
    // no longer orthonormal, and the winding test that decides which faces are
    // front-facing starts answering wrongly -- so every face is culled and a
    // solid ship renders as almost nothing.
    Vec3 u = vsub(c->up, vmul(c->fwd, vdot(c->up, c->fwd)));
    if (vlen2(u) > 1e-6f) c->up = vnorm(u);

    c->roll_vis += ((s_turn > 0 ? 0.70f : -0.70f) - c->roll_vis) * dt * 0.8f;
    c->pos = vadd(c->pos, vmul(c->fwd, c->speed * dt));

    // Sampled twice as often as a combat trail: this one crosses the frame at
    // 815 units a second, and at the normal rate the ribbon is a row of
    // disconnected dashes rather than a streak. Laid from the engine, not the
    // hull centre, or at this model size it would start inside the ship.
    c->trail_acc += dt;
    if (c->trail_acc >= SHIP_TRAIL_DT * 0.5f) {
        c->trail_acc = 0;
        c->trail_head = (uint8_t)((c->trail_head + 1) % SHIP_TRAIL);
        c->trail[c->trail_head]   = vsub(c->pos, vmul(c->fwd, c->scale * 1.3f));
        c->trail_p[c->trail_head] = 255;
        if (c->trail_n < SHIP_TRAIL) c->trail_n++;
    }
}

bool vg_cine_update(float dt, bool skip) {
    const float t = vg.state_t;

    // The venue materialises during the opening drift, so the first shot is of
    // somewhere arriving rather than of somewhere already there.
    vg_sky_set_reveal(t / (INTRO_DRIFT * 0.85f));

    const bool subject = vg.cine.on || vg.cine.gate_t > 0.0f;

    // Zoom is driven by RANGE, not by a timeline. Holding focal length
    // proportional to distance keeps the ship a constant size, so the operator
    // is tight on it while it is far out and pulls wide as it closes -- which is
    // what a camera crew does, and it stays in step with the geometry without a
    // hand-authored curve. The wide clamp is what lets the pass be dramatic:
    // inside about 1350 units the lens has nothing left to give and the ship is
    // finally allowed to get big, right as it goes by.
    if (subject) {
        float z = vlen(vg.cine.on ? vg.cine.ship.pos : vg.cine.gate_pos) / 2400.0f;
        if (z < 0.74f) z = 0.74f;
        if (z > 2.40f) z = 2.40f;
        float k = dt * 4.0f;
        if (k > 1.0f) k = 1.0f;
        vg.cam_zoom += (z - vg.cam_zoom) * k;      // smoothed, or it judders
    } else {
        vg.cam_zoom = 1.0f;
    }

    // Which shot we are in. Each cut re-anchors, so the second setup reads as a
    // different camera in a different place rather than a repeat.
    static int s_shot = 0;
    const int  shot = (t > INTRO_OPP_START && t < INTRO_OPP_END) ? 2
                    : (t > INTRO_DRIFT     && t < INTRO_YOU_END) ? 1 : 0;

    if (shot != s_shot) {
        const bool was_first = (s_shot == 1);
        s_shot = shot;
        if (shot == 1) {
            s_turn = 0.10f;
            cine_launch(vg.spec, vg.trail_hue, false);
        } else if (shot == 2) {
            if (was_first) cine_relocate();
            s_turn = -0.10f;
            cine_launch(vg.enemy[0].spec, vg.enemy[0].hue, true);
        } else {
            // Between shots the ribbon goes too, or the cut lands on a stranded
            // trail with nothing on the end of it.
            vg_cine_clear();
        }
    }

    // Release the ship once the gate has opened and been held. Seated straight
    // off the plane rather than from its stored position, so it emerges from
    // wherever the gate has rotated to -- aligned, with nothing to drift.
    if (vg.cine.hold > 0.0f) {
        vg.cine.hold -= dt;
        if (vg.cine.hold <= 0.0f) {
            Ship* c = &vg.cine.ship;
            c->pos = vg.cine.gate_pos;
            c->fwd = vnorm(vcross(vg.cine.gate_r, vg.cine.gate_u));
            Vec3 u = vsub(c->up, vmul(c->fwd, vdot(c->up, c->fwd)));
            c->up  = (vlen2(u) > 1e-6f) ? vnorm(u) : vg.cine.gate_u;
            vg.cine.on = true;
        }
    }

    float pitch_in = 0.0f, yaw_in = 0.0f;
    if (subject) {
        // Aim at whichever exists. Before the ship is out, the gate is the
        // subject, and holding on it is what tells the viewer where to look.
        const Vec3 w = vnorm(vg.cine.on ? vg.cine.ship.pos : vg.cine.gate_pos);
        yaw_in   =  w.x * 3.2f;
        pitch_in = -w.y * 3.2f;
    } else {
        vg_attract_autopilot(t, &pitch_in, &yaw_in);
    }

    // Wall avoidance applies even mid-shot. Following a ship is not a reason to
    // fly the viewpoint out through the side of the arena, and unconstrained
    // panning did exactly that -- the cutscene ended outside the torus and the
    // first collision test of the match killed the player instantly.
    {
        const Vec3  pl    = vg_arena_local_of(v3(0, 0, 0));
        const float clear = vg_arena_clearance(pl);
        if (clear < ARENA_ATTRACT_MARGIN) {
            const Vec3  inw = vg_arena_dir_to_view(vg_arena_inward(pl));
            const float k   = 2.6f * (ARENA_ATTRACT_MARGIN - clear) / ARENA_ATTRACT_MARGIN;
            yaw_in   +=  inw.x * k;
            pitch_in += -inw.y * k;
        }
    }

    if (yaw_in   >  1.0f) yaw_in   =  1.0f;
    if (yaw_in   < -1.0f) yaw_in   = -1.0f;
    if (pitch_in >  1.0f) pitch_in =  1.0f;
    if (pitch_in < -1.0f) pitch_in = -1.0f;

    // Throttle zero: the camera is anchored, not flying. The world step still
    // rotates everything by the pan, which is what carries the arena and the
    // starfield across behind the subject.
    vg_world_step(dt, pitch_in, yaw_in, 0.0f, 0.0f);
    if (vg.cine.on) cine_fly(dt);

    if (t > INTRO_END || skip) {
        s_shot = 0;
        // THE VENUE IS ALL THE WAY THERE ONCE THE INTRO IS OVER, however it ended.
        //
        // The reveal is set from `t` every frame above, so an intro that runs its course
        // leaves it past 1 and clamped. A SKIP does not: it returns from here with the
        // dissolve stopped wherever the player's thumb found it, and the backdrop fill goes on
        // blacking out every row the reveal had not reached yet -- for the rest of the match.
        // It reads as heavy banding, because that is what it is: seven rows in eight are the
        // clear colour.
        //
        // Set at the exit rather than at the skip, so it covers both ways out and any third
        // one added later.
        vg_sky_set_reveal(1.0f);
        return true;
    }
    return false;
}
