#include "vg_sim.h"
#include "vg_shake.h"
#include "vg_arena.h"
#include "vg_sky.h"
#include <math.h>

// The flight model and the per-frame world transform.

// ---------------------------------------------------------------------------
// World step
//
// Everything lives in VIEW SPACE: the player is nailed to the origin looking
// down +z, and the world rotates and translates around it. One counter-rotation
// per frame is applied to every star, mote, rock, ship, missile, trail point and
// fragment -- plus the arena and the backdrop.
// ---------------------------------------------------------------------------

// `roll_in` is a rotation about the view axis for THIS frame, in radians. It
// goes into the world transform rather than into the camera, which is the whole
// difference between a tumble and a rotated picture: once it is in R, the next
// frame's pitch and yaw act in the rolled frame and the path actually
// corkscrews. Flight passes zero -- the player's roll stays cosmetic.
// The roll command as an angle for this frame. Lives here rather than at the
// three call sites because it is part of the flight model, and because the
// throttle it depends on is the smoothed one -- roll authority should lag a
// shove of the slider exactly as speed does.
float vg_roll_angle(const VgInput* in, float dt) {
    const float scale = ROLL_SLOW_SCALE
                      + (ROLL_FAST_SCALE - ROLL_SLOW_SCALE) * vg.throttle;
    // ...and the airframe. A LANCE and a CHARIOT rolled at exactly the same rate
    // before this, which quietly said the classes were interchangeable on the one
    // axis the player had just been handed.
    const float want = in->roll * ROLL_RATE * scale * vg_ship_mobility(vg.spec);

    // CHASED, not set. The rate the player asks for is where the airframe is
    // going, not where it already is -- so a roll has to be started and has to be
    // allowed to stop, and letting go leaves the ship still turning for a moment.
    // That carried-through part is the difference between flying it and rotating
    // the picture.
    float k = dt * ROLL_LERP;
    if (k > 1.0f) k = 1.0f;
    vg.roll_rate += (want - vg.roll_rate) * k;

    return vg.roll_rate * dt;
}

void vg_world_step(float dt, float pitch_in, float yaw_in, float roll_in,
                       float throttle_in) {
    float k = dt * THROTTLE_LERP;
    if (k > 1.0f) k = 1.0f;
    vg.throttle += (throttle_in - vg.throttle) * k;
    // An exponential lerp only ever approaches its target, so a slider held hard
    // against the stop settled at ~0.997 and top speed came out one unit short.
    // Snap the last sliver: reaching genuine maximum speed is the difference
    // between outrunning a missile and not.
    if (fabsf(throttle_in - vg.throttle) < 0.002f) vg.throttle = throttle_in;

    vg.speed = vg.spec->speed_min
             + (vg.spec->speed_max - vg.spec->speed_min) * vg.throttle;

    // What the airframe is doing about it. Quadratic in speed against the fastest
    // class, times this ship's own nervousness -- so a CHARIOT at full is rougher
    // than a BALLISTA at full, rather than the two merely being equally rough at
    // their own maxima, which is what a throttle-fraction curve gives you and is
    // not the same fantasy at all.
    const float sn = vg.speed / SPEED_SHAKE_REF;
    vg.buzz = sn * sn * vg.spec->shake
    // Rolling is work, and the airframe should be seen doing it. Without this the
    // ship rattled harder the faster it went and then rolled through ninety
    // degrees in perfect calm, which is the picture turning rather than anything
    // happening to the machine.
            + fabsf(vg.roll_rate) * ROLL_BUZZ;

    // Visual-only tracking of the same command, several times faster.
    float kv = dt * THROTTLE_VIS_LERP;
    if (kv > 1.0f) kv = 1.0f;
    vg.throttle_vis += (throttle_in - vg.throttle_vis) * kv;
    if (fabsf(throttle_in - vg.throttle_vis) < 0.002f) vg.throttle_vis = throttle_in;

    // Turn rate falls off with speed. Backing off tightens the turn; firewalling
    // it flattens you out. This is what makes the throttle a combat control.
    vg.agility = 1.0f + vg.spec->agility_slow_bonus * (1.0f - vg.throttle)
                      - vg.spec->agility_fast_malus * vg.throttle;

    float rate  = vg.spec->turn_rate * vg.agility;
    float yaw   = yaw_in   * rate * dt;
    float pitch = pitch_in * rate * dt;

    vg.roll += roll_in;
    if (vg.roll >  6.28318531f) vg.roll -= 6.28318531f;
    if (vg.roll < -6.28318531f) vg.roll += 6.28318531f;

    Mat3  R  = mat3_euler(-pitch, -yaw, roll_in);
    float dz = vg.speed * dt;

    // The arena is static in the world, so it rides exactly the same transform.
    vg_arena_step(R, dz);
    vg.wall_clear = vg_arena_clearance(vg_arena_local_of(v3(0, 0, 0)));

    // The backdrop is at infinity, so only rotation moves it -- and it does NOT
    // ride R, so it has to be handed the total apparent roll by hand: the true
    // roll now baked into world positions, plus the cosmetic bank that the
    // projection will add on top. Miss either term and the nebula slides against
    // the starfield.
    // The SAME R, which is the entire fix: the backdrop used to integrate the
    // scalar pitch and yaw next door into a flat pan, and angles do not compose.
    // Only the cosmetic bank goes separately, because that one is added by the
    // projection and is not in R.
    vg_sky_orient(R, vg.bank);

    // The cosmetic lean, from the yaw command as always -- plus a lead into any
    // roll. Rolling used to have NO visual signature of its own: yaw is forced to
    // zero while the roll key is held, so the bank actually decayed to nothing
    // during the one manoeuvre that deserved the most emphasis.
    //
    // Same sign as the roll, which is not a guess: vg_sky_step above is handed
    // `vg.bank + vg.roll` as a single apparent angle, so the two already share a
    // convention and adding them exaggerates rather than cancels.
    const float roll_rate_now = (dt > 0.0f) ? (roll_in / dt) : 0.0f;
    const float bank_target   = (-yaw_in * BANK_MAX)
                              + roll_rate_now * ROLL_BANK_LEAD;

    float kb = dt * BANK_LERP;
    if (kb > 1.0f) kb = 1.0f;
    vg.bank += (bank_target - vg.bank) * kb;

    for (int i = 0; i < NUM_STARS; i++) vg.star[i] = mat3_apply(R, vg.star[i]);

    // Motes are ordinary static world points: rotate, translate, recycle once
    // they fall behind.
    for (int i = 0; i < NUM_MOTES; i++) {
        Vec3 m = mat3_apply(R, vg.mote[i]);
        m.z -= dz;
        if (m.z < MOTE_CULL_Z || vlen2(m) > 1700.0f * 1700.0f)
            m = vg_mote_spawn(MOTE_Z_MIN, MOTE_Z_MAX);
        vg.mote[i] = m;
    }

    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        Asteroid* a = &vg.ast[i];
        if (!a->alive) continue;
        a->pos = mat3_apply(R, a->pos);
        a->vel = mat3_apply(R, a->vel);
        a->pos = vadd(a->pos, vmul(a->vel, dt));
        a->pos.z -= dz;
        for (int s = 0; s < 3; s++) a->spin[s] += a->spin_rate[s] * dt;
        if (a->pos.z < CULL_Z_BEHIND || vlen2(a->pos) > CULL_RADIUS * CULL_RADIUS ||
            vg_arena_clearance(vg_arena_local_of(a->pos)) < -40.0f)
            a->alive = false;
    }

    // Trails are world geometry, so every stored point rides the same transform
    // the objects do -- otherwise a ribbon would smear sideways the moment you
    // manoeuvred instead of staying pinned to the track that was actually flown.
    vg.trail_acc += dt;
    for (int t = 0; t < vg.trail_n; t++) {
        int idx = (vg.trail_head - t + SHIP_TRAIL * 2) % SHIP_TRAIL;
        vg.trail[idx] = mat3_apply(R, vg.trail[idx]);
        vg.trail[idx].z -= dz;
    }
    if (vg.trail_acc >= SHIP_TRAIL_DT) {
        vg.trail_acc = 0;
        vg.trail_head = (uint8_t)((vg.trail_head + 1) % SHIP_TRAIL);
        // The player is nailed to the origin, so their track is seeded there and
        // is carried backwards by the transform above like everything else.
        vg.trail[vg.trail_head]   = v3(0, 0, 0);
        vg.trail_p[vg.trail_head] = (uint8_t)(vg.throttle * 255.0f);
        if (vg.trail_n < SHIP_TRAIL) vg.trail_n++;
    }

    // The cutscene ship rides the ROTATION but not the translation. During the
    // death sequence that holds the wreck at a fixed distance while the camera
    // turns around it, which is the whole shot -- applying dz as well would
    // simply leave it behind at the player's last cruising speed.
    if (vg.cine.on) {
        Ship* c = &vg.cine.ship;
        c->pos = mat3_apply(R, c->pos);
        c->fwd = vnorm(mat3_apply(R, c->fwd));
        c->up  = vnorm(mat3_apply(R, c->up));
        // Rotation cannot skew an orthonormal pair, but floating point drift
        // over thousands of frames can, and the failure is silent -- the model
        // simply stops having front faces.
        {
            Vec3 u = vsub(c->up, vmul(c->fwd, vdot(c->up, c->fwd)));
            if (vlen2(u) > 1e-6f) c->up = vnorm(u);
        }
        if (vg.cine.gate_t > 0.0f) {
            vg.cine.gate_pos = mat3_apply(R, vg.cine.gate_pos);
            vg.cine.gate_r   = vnorm(mat3_apply(R, vg.cine.gate_r));
            vg.cine.gate_u   = vnorm(mat3_apply(R, vg.cine.gate_u));
        }
        // The ribbon has to ride the same rotation as the ship that laid it.
        // Missing this is why the cutscene ships appeared to emit nothing: the
        // camera pans at up to two radians a second during a pass, so within a
        // few frames the stored points were left pointing into an old view
        // frame -- swung behind the near plane and culled before they could be
        // drawn. The trail was always there; it was just no longer in front.
        for (int t = 0; t < c->trail_n; t++) {
            int idx = (c->trail_head - t + SHIP_TRAIL * 2) % SHIP_TRAIL;
            c->trail[idx] = mat3_apply(R, c->trail[idx]);
        }
    }

    for (int i = 0; i < MAX_ENEMIES; i++) {
        Ship* s = &vg.enemy[i];
        if (!s->alive) continue;
        s->pos = mat3_apply(R, s->pos);
        s->fwd = vnorm(mat3_apply(R, s->fwd));
        s->up  = vnorm(mat3_apply(R, s->up));
        s->pos = vadd(s->pos, vmul(s->fwd, s->speed * dt));
        s->pos.z -= dz;

        for (int t = 0; t < s->trail_n; t++) {
            int idx = (s->trail_head - t + SHIP_TRAIL * 2) % SHIP_TRAIL;
            s->trail[idx] = mat3_apply(R, s->trail[idx]);
            s->trail[idx].z -= dz;
        }
        s->trail_acc += dt;
        if (s->trail_acc >= SHIP_TRAIL_DT) {
            s->trail_acc = 0;
            s->trail_head = (uint8_t)((s->trail_head + 1) % SHIP_TRAIL);
            s->trail[s->trail_head] = s->pos;
            // Their throttle, read back out of their speed -- so an enemy
            // extending at full power streams exactly the way the player does.
            float tp = (s->speed - s->spec->speed_min)
                     / (s->spec->speed_max - s->spec->speed_min);
            if (tp < 0.0f) tp = 0.0f; else if (tp > 1.0f) tp = 1.0f;
            s->trail_p[s->trail_head] = (uint8_t)(tp * 255.0f);
            if (s->trail_n < SHIP_TRAIL) s->trail_n++;
        }
        // Backstop: the AI steers away from the wall, but never let one escape
        // the world if it cuts a turn too fine.
        s->pos = vg_arena_clamp_inside(s->pos, ENEMY_HIT_RADIUS);

        // NO DISTANCE CULL. There used to be one at CULL_RADIUS, which is 4200,
        // and ARENA_TORUS_RMAJ is also 4200. The arena is a LOOP of that radius,
        // so two ships on opposite sides of it are 8400 apart -- twice the
        // distance that deleted one of them. An opponent spawns at up to 3625,
        // already most of the way there, and any drift down the tunnel finished
        // the job.
        //
        // It killed the opponent with no debris, no explosion and no last line,
        // the match saw nobody left alive, and the player won a fight that never
        // happened. The clamp above already keeps every enemy inside the torus,
        // so there is nothing left for a cull to save us from.
    }

    // The gate is world geometry like anything else: it counter-rotates and
    // recedes, so the course needs no idea how flight works.
    if (vg.course.ring_alive) {
        vg.course.ring_pos  = mat3_apply(R, vg.course.ring_pos);
        vg.course.ring_norm = vnorm(mat3_apply(R, vg.course.ring_norm));
        vg.course.ring_pos.z -= dz;
    }

    for (int i = 0; i < MAX_MISSILES; i++) {
        Missile* m = &vg.msl[i];
        if (!m->alive) continue;
        m->pos = mat3_apply(R, m->pos);
        m->dir = vnorm(mat3_apply(R, m->dir));
        m->pos.z -= dz;
        // The trail is world geometry too, so it has to ride the same transform
        // or it would smear behind the missile as you manoeuvre.
        for (int t = 0; t < m->trail_n; t++) {
            int idx = (m->trail_head - t + MISSILE_TRAIL * 2) % MISSILE_TRAIL;
            m->trail[idx] = mat3_apply(R, m->trail[idx]);
            m->trail[idx].z -= dz;
        }
    }

    for (int i = 0; i < MAX_DEBRIS; i++) {
        Debris* d = &vg.deb[i];
        if (!d->alive) continue;
        d->pos = mat3_apply(R, d->pos);
        d->vel = mat3_apply(R, d->vel);
        d->seg = mat3_apply(R, d->seg);
        d->pos = vadd(d->pos, vmul(d->vel, dt));
        d->pos.z -= dz;
        d->life -= dt;
        if (d->life <= 0) d->alive = false;
    }

    // Fireballs ride the world exactly as the shards do. Here rather than in a
    // pass of their own because this is the one function every flying state goes
    // through -- a separate update called from the state cases would quietly
    // freeze mid-explosion the moment the player died.
    for (int i = 0; i < MAX_FIREBALLS; i++) {
        Fireball* f = &vg.fire[i];
        if (!f->alive) continue;
        f->pos = mat3_apply(R, f->pos);
        f->vel = mat3_apply(R, f->vel);
        f->pos = vadd(f->pos, vmul(f->vel, dt));
        f->pos.z -= dz;
        f->life -= dt;
        if (f->life <= 0) f->alive = false;
        // INSIDE THE FIRE. Positions are player-relative, so vlen is the range to
        // the cockpit and this is simply "is the canopy in it". The nominal radius
        // rather than the drawn one: the drawn size is a curve the renderer owns,
        // and the simulation has no business reading it.
        const float rng = vlen(f->pos);
        if (rng < f->r) {
            const float deep = 1.0f - rng / f->r;
            vg_shake_rumble(FIRE_RUMBLE * deep);
            // The one thing that glitches the panel without the hull being
            // touched. Flying through a fireball should look like it costs you
            // something even when it does not.
            const float g = DAMAGE_GLITCH * FIRE_GLITCH_K * deep;
            if (g > vg.damage_glitch) vg.damage_glitch = g;
        }
    }

    vg_vfx_tick(dt);

    // Every knock the airframe has taken, decayed and rolled into this frame's
    // offset. See vg_shake.h -- the level itself lives in that module now, not on
    // vg, because a dozen things contribute to it and none of them owns it.
    vg_shake_update(dt);

    // Buzz, on top of whatever the impact shake is doing. Quadratic in speed, so
    // it is present the whole way up rather than switching on near the stop, and
    // scaled by the airframe -- see vg.buzz in the world step.
    if (vg.buzz > 0.0f) {
        const float a = SPEED_SHAKE_MAX * vg.buzz;
        vg.shake_x += vg_frand(-a, a);
        vg.shake_y += vg_frand(-a, a);
    }

    // THE TAIL IS NOT THE WORLD STEP. What follows are four different modules'
    // timers that ended up in one run because they all need a dt and this is
    // where the dt was. Each is named for what owns it, so they can leave for
    // those modules one at a time.
    //
    // They are all independent scalar decays, which is what makes the grouping
    // safe: nothing in the tail reads a value another part of it writes, so the
    // order within it never mattered. Only their position AFTER vg_vfx_tick does
    // -- a bench shot raises the flash and the knock, and both have to be laid
    // down before they are decayed and before the shake is assembled.
    vg_hud_decay(dt);
    vg_comms_step(dt);
    // The course's, and the only one left inline. It decays here rather than in
    // vg_course_update because that runs from the state dispatch, a different
    // point in the frame, and moving a countdown between two points in a frame
    // is a behaviour change wearing a refactor's clothes.
    if (vg.cine.gate_t > 0) vg.cine.gate_t -= dt;
}

// A fighter crossing close aboard. Two airframes going opposite ways at a
// combined 800 units a second, passing inside ten ship lengths, and until now the
// cockpit reported nothing whatsoever -- which made the most exciting thing that
// happens in a dogfight the least physical.
//
// The knock lands at CLOSEST APPROACH, found the way the missile fuse finds it:
// the frame the range stops shrinking. You cannot know you were at the minimum
// until you are past it, and firing on the way in would put the thump before the
// event.
void vg_update_passes(void) {
    for (int i = 0; i < MAX_ENEMIES; i++) {
        Ship* s = &vg.enemy[i];
        if (!s->alive) { s->pass_range = 0.0f; s->pass_done = false; continue; }

        const float r = vlen(s->pos);

        // How fast they are going by. Radial closure is zero at the minimum by
        // definition, so the speed has to come from the airframes themselves -- a
        // hard crossing pass and a slow drift-by are the same geometry, and only
        // one of them should be felt.
        float sp = 0.0f;
        if (vg.spec) {
            const float ref = vg.spec->speed_max * 2.0f;
            if (ref > 1.0f) sp = (vg.speed + s->speed) / ref;
        }
        if (sp > 1.0f) sp = 1.0f;

        // The buffeting, renewed every frame they are inside the envelope. This is
        // the part that reads as violent: a single impulse is one frame of
        // movement no matter how large it is.
        if (r < PASS_RANGE) {
            const float near = 1.0f - r / PASS_RANGE;
            vg_shake_rumble(near * near * (0.30f + 0.70f * sp) * PASS_RUMBLE);
        }

        if (r < s->pass_range) s->pass_done = false;      // closing again: re-arm
        else if (!s->pass_done && s->pass_range > 0.0f
                 && s->pass_range < PASS_RANGE) {
            // ...and the thump, once, at the moment of closest approach.
            const float close = 1.0f - s->pass_range / PASS_RANGE;
            vg_shake_hit(close * (0.30f + 0.70f * sp) * PASS_SHAKE);
            s->pass_done = true;
        }
        s->pass_range = r;
    }
}
