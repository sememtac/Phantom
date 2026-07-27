#include "vg_sim.h"
#include "vg_arena.h"
#include <math.h>

// Enemy fighter behaviour, in strict priority order:
//
//   1. wall avoidance   -- outranks even running from a missile, because flying
//                          into the boundary to dodge is just a slower death
//   2. missile evasion  -- break ACROSS the seeker, never away from it
//   3. break-off        -- extend after a pass instead of boring in
//   4. pursue           -- aim beside the player, not at them
//
// Enemies live under the same physics the player does: bleeding speed buys turn
// rate, and flat out they cannot shoot.

// Closest player missile currently tracking this enemy.
static const Missile* incoming_for(int enemy_index, float* out_range) {
    const Missile* best = nullptr;
    float bestr = 1e9f;
    for (int i = 0; i < MAX_MISSILES; i++) {
        const Missile* m = &vg.msl[i];
        if (!m->alive || !m->from_player || !m->locked) continue;
        if (m->target != enemy_index) continue;
        float r = vlen(vsub(m->pos, vg.enemy[enemy_index].pos));
        if (r < bestr) { bestr = r; best = m; }
    }
    if (out_range) *out_range = bestr;
    return best;
}

// Pick a break direction perpendicular to `along`, with a random side and a
// little out-of-plane bias so successive breaks do not look identical.
static Vec3 break_across(Vec3 along, Vec3 up, float toward_bias) {
    Vec3 side = vcross(along, up);
    if (vlen2(side) < 1e-4f) side = vcross(along, v3(1, 0, 0));
    side = vnorm(side);
    if (vg_frand01() < 0.5f) side = vmul(side, -1.0f);
    return vnorm(vadd(side, vmul(along, toward_bias)));
}

void vg_update_enemy(Ship* s, int index, float dt) {
    if (!s->alive) return;
    if (s->hit_flash > 0) s->hit_flash -= dt;

    float  inc_range = 1e9f;
    const Missile* inc = incoming_for(index, &inc_range);

    Vec3 desired;

    Vec3  elocal = vg_arena_local_of(s->pos);
    float eclear = vg_arena_clearance(elocal);

    if (eclear < ARENA_ENEMY_MARGIN) {
        desired = vg_arena_dir_to_view(vg_arena_inward(elocal));
        s->target_speed = (ENEMY_SPEED_MIN + ENEMY_SPEED_MAX) * 0.5f;
        s->evade_t = 0;
        s->break_t = 0;

    } else if (inc && inc_range < ENEMY_EVADE_RANGE) {
        // Turning across the seeker is what forces the bearing outside its cone;
        // turning away from it just gets you run down.
        if (s->evade_t <= 0) {
            s->evade_dir = vnorm(vadd(break_across(inc->dir, s->up, 0.0f),
                                      vmul(vg_rand_unit(), 0.35f)));
            s->evade_t   = vg_frand(0.9f, 1.6f);
        }
        desired = s->evade_dir;
        // Bleed speed to tighten the break -- the same trick the player has.
        s->target_speed = ENEMY_SPEED_MIN * 1.15f;

    } else {
        s->evade_t = 0;

        Vec3  to    = vsub(v3(0, 0, 0), s->pos);   // player is the origin
        float range = vlen(to);

        if (s->break_t > 0 || range < ENEMY_BREAK_RANGE) {
            // Break off and extend. Holding the current heading here instead --
            // which pursuit had already pointed at the player -- flew them
            // straight into the player. Commit to a direction across AND away
            // from the line of sight, long enough to actually open the range.
            if (s->break_t <= 0) {
                s->break_dir = break_across(vnorm(to), s->up, -0.55f);
                s->break_t   = vg_frand(ENEMY_BREAK_TIME_MIN, ENEMY_BREAK_TIME_MAX);
                // Re-roll the approach offset so the next pass comes in from a
                // different side instead of repeating the same merge forever.
                s->offset_dir = vg_rand_unit();
            }
            desired = s->break_dir;
            s->target_speed = ENEMY_SPEED_MAX;
        } else {
            // Aim at a point BESIDE the player. A pursuit curve that converges
            // perfectly on its aim point then produces a firing pass rather than
            // a collision.
            Vec3 aim = vmul(s->offset_dir, ENEMY_OFFSET);
            desired  = vsub(aim, s->pos);
            // Close fast, then bleed off to a speed they can actually fight at --
            // the same decision the player has to make.
            s->target_speed = (range > ENEMY_CLOSE_RANGE)
                              ? ENEMY_SPEED_MAX * 0.85f
                              : ENEMY_SPEED_MIN + (ENEMY_SPEED_MAX - ENEMY_SPEED_MIN) * 0.35f;
        }
    }

    if (s->evade_t > 0) s->evade_t -= dt;
    if (s->break_t > 0) s->break_t -= dt;

    // Same trade the player gets: bleeding speed buys turn rate. Without this
    // their evasive break is too lazy to ever defeat a seeker.
    float snorm = (s->speed - ENEMY_SPEED_MIN) / (ENEMY_SPEED_MAX - ENEMY_SPEED_MIN);
    if (snorm < 0) snorm = 0; else if (snorm > 1) snorm = 1;
    float erate = ENEMY_TURN_RATE * (1.0f + 0.60f * (1.0f - snorm) - 0.25f * snorm);

    // Turn, and derive the visual bank from which way we are pulling.
    Vec3 before = s->fwd;
    s->fwd = vg_turn_toward(s->fwd, desired, erate * dt);

    Vec3  u     = vnorm(vsub(s->up, vmul(s->fwd, vdot(s->up, s->fwd))));
    Vec3  right = vcross(u, s->fwd);
    Vec3  dv    = vsub(s->fwd, before);
    float roll_target = -vdot(dv, right) * 26.0f;
    if (roll_target >  1.1f) roll_target =  1.1f;
    if (roll_target < -1.1f) roll_target = -1.1f;
    s->roll_vis += (roll_target - s->roll_vis) * (dt * 6.0f > 1 ? 1 : dt * 6.0f);
    s->up = u;

    float k = dt * 1.5f;
    if (k > 1) k = 1;
    s->speed += (s->target_speed - s->speed) * k;

    // Fire when the player is in the nose cone and in range -- and only when slow
    // enough. Without that last clause an enemy could disengage AND keep firing,
    // which beats every option the player has.
    s->fire_cd -= dt;
    Vec3  to      = vsub(v3(0, 0, 0), s->pos);
    float range   = vlen(to);
    float fire_sn = (s->speed - ENEMY_SPEED_MIN) / (ENEMY_SPEED_MAX - ENEMY_SPEED_MIN);

    if (s->fire_cd <= 0 && range < ENEMY_FIRE_RANGE && range > 60.0f &&
        fire_sn < ENEMY_ENGAGE_SPEED &&
        vdot(s->fwd, vnorm(to)) > ENEMY_FIRE_COS) {
        vg_launch_missile(false, vadd(s->pos, vmul(s->fwd, 4.0f)), s->fwd, -1);
        s->fire_cd = ENEMY_FIRE_GAP * vg_frand(0.8f, 1.3f);
    }
}
