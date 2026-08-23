#include "vg_sim.h"
#include "vg_arena.h"
#include <math.h>

// Enemy fighter behaviour, in strict priority order:
//
//   1. wall avoidance   -- outranks even running from a missile, because flying
//                          into the boundary to dodge is just a slower death,
//                          and it outranks a suicide run because the wall
//                          cannot be aimed at anybody
//   2. suicide run      -- once committed, nothing else matters, including a
//                          missile on their tail
//   3. missile evasion  -- break ACROSS the seeker, never away from it
//   4. break-off        -- extend after a pass instead of boring in
//   5. pursue           -- aim beside the player, not at them
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

// ---------------------------------------------------------------------------
// THE TACTIC: what this class wants to be doing when nothing is trying to kill
// it this instant.
//
// Steps 1 to 3 above -- the wall, the suicide run, the missile on the tail --
// are survival, and every hull does them the same way. What is left is
// POSITIONING, and that is where one class should differ from another: where it
// wants to be relative to the player, and how fast.
//
// Split out so a second tactic can exist without touching the priorities that
// have to hold for all of them. Dispatched on ShipSpec::tactic; today every
// class carries TACTIC_FIGHTER, so this is one function and the behaviour is
// exactly what it was.
// ---------------------------------------------------------------------------

static void tactic_fighter(Ship* s, const ShipSpec* sp, Vec3 to, float range,
                           float close_r, float smin, float smax, Vec3* desired) {
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
        *desired = s->break_dir;
        s->target_speed = smax;
    } else {
        // Aim at a point BESIDE the player. A pursuit curve that converges
        // perfectly on its aim point then produces a firing pass rather than
        // a collision.
        Vec3 aim  = vmul(s->offset_dir, ENEMY_OFFSET);
        *desired  = vsub(aim, s->pos);
        // Close fast, then bleed off to a speed they can actually fight at --
        // the same decision the player has to make.
        s->target_speed = (range > close_r)
                          ? smax * 0.85f
                          : smin + (smax - smin) * 0.35f;
    }
}

void vg_update_enemy(Ship* s, int index, float dt) {
    if (!s->alive) return;
    if (s->hit_flash > 0) s->hit_flash -= dt;

    // Everything below is expressed as a fraction of THIS ship's own capability
    // rather than as an absolute, so a BALLISTA settles to a fighting speed at
    // 2600 units and a CHARIOT at 1200 without the behaviour code knowing that
    // ship classes exist. It is not per-archetype behaviour -- a BALLISTA still
    // flies the same fight an AEGIS does, just from further out.
    const ShipSpec* sp  = s->spec;
    const float smin    = sp->speed_min;
    const float smax    = sp->speed_max;
    const float close_r = sp->lock_range * ENEMY_CLOSE_RANGE_K;
    const float fire_r  = sp->lock_range * ENEMY_FIRE_RANGE_K;

    float  inc_range = 1e9f;
    const Missile* inc = incoming_for(index, &inc_range);

    // The fight has started once they are close enough for the player to see
    // them. Their own lock range is the right measure of that: it is the
    // distance this class fights at, so a BALLISTA counts from further out than
    // a CHARIOT, which is exactly the difference between those ships.
    if (!s->engaged && vlen2(s->pos) < (sp->lock_range * sp->lock_range)) {
        s->engaged = true;
    }

    Vec3 desired;

    Vec3  elocal = vg_arena_local_of(s->pos);
    float eclear = vg_arena_clearance(elocal);

    // Commit to a suicide run: this pilot is the sort, they are nearly dead, and
    // the player is close enough for it to be a decision rather than a plan.
    if (!s->kamikaze_on && s->kamikaze_will &&
        s->hull <= sp->hull * ENEMY_KAMIKAZE_HULL &&
        vlen2(s->pos) < ENEMY_KAMIKAZE_RANGE * ENEMY_KAMIKAZE_RANGE) {
        s->kamikaze_on = true;
        // Said once, on the decision. The player gets a line and a ship that
        // stops manoeuvring and starts pointing straight at them, which together
        // are the only warning they get.
        vg_comms_say(s, VOICE_TAUNT);
    }

    if (eclear < ARENA_ENEMY_MARGIN) {
        desired = vg_arena_dir_to_view(vg_arena_inward(elocal));
        s->target_speed = (smin + smax) * 0.5f;
        s->evade_t = 0;
        s->break_t = 0;

    } else if (s->kamikaze_on) {
        // Straight at the player, everything open. Above missile evasion on
        // purpose: a pilot who has decided to ram does not care what is chasing
        // them. Still BELOW wall avoidance, because dying against the boundary
        // achieves nothing at all -- the whole point is to take the player with
        // them, and the wall cannot be aimed.
        //
        // Aimed a little past the player rather than at them. A pursuit curve
        // that converges exactly on its aim point arrives with the closing speed
        // fallen to nothing, which is a stern chase, not a collision.
        Vec3 to = vsub(v3(0, 0, 0), s->pos);
        desired = vadd(to, vmul(vnorm(to), ENEMY_KAMIKAZE_LEAD));
        s->target_speed = smax;
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
        s->target_speed = smin * 1.15f;

    } else {
        s->evade_t = 0;

        Vec3  to    = vsub(v3(0, 0, 0), s->pos);   // player is the origin
        float range = vlen(to);

        switch (sp->tactic) {
            case TACTIC_FIGHTER:
            default:
                tactic_fighter(s, sp, to, range, close_r, smin, smax, &desired);
                break;
        }
    }

    if (s->evade_t > 0) s->evade_t -= dt;
    if (s->break_t > 0) s->break_t -= dt;

    // Same trade the player gets: bleeding speed buys turn rate. Without this
    // their evasive break is too lazy to ever defeat a seeker.
    float snorm = (s->speed - smin) / (smax - smin);
    if (snorm < 0) snorm = 0; else if (snorm > 1) snorm = 1;
    float erate = sp->turn_rate * s->skill
                * (1.0f + sp->agility_slow_bonus * (1.0f - snorm)
                        - sp->agility_fast_malus * snorm);

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
    float fire_sn = (s->speed - smin) / (smax - smin);

    if (s->fire_cd <= 0 && range < fire_r && range > 60.0f &&
        fire_sn < ENEMY_ENGAGE_SPEED &&
        vdot(s->fwd, vnorm(to)) > ENEMY_FIRE_COS) {
        vg_launch_missile(false, vadd(s->pos, vmul(s->fwd, 4.0f)), s->fwd, -1, sp);
        // The class's SUSTAINED rate: a full clip at its own trigger speed, plus
        // the reload, spread over the rounds. Enemies carry no magazine of their
        // own, so this is how a class's fire control reaches them at all.
        //
        // It used to read sp->reload directly, which was seconds-per-round until
        // that field became seconds-per-clip. Left alone, an enemy CHARIOT would
        // have gone from firing every 1.4s to every 9 -- a class deleted by a
        // change to a word's meaning, with nothing to show it had happened.
        const float sustained = ((float)sp->magazine * sp->fire_gap + sp->reload)
                              / (float)(sp->magazine > 0 ? sp->magazine : 1);
        s->fire_cd = sustained * ENEMY_FIRE_GAP_K * vg_frand(0.8f, 1.3f);
        // Called on the launch, not on the lock: the line and the missile leave
        // together, so the radio is a warning rather than a commentary.
        vg_comms_say(s, VOICE_FIRE);
    }
}
