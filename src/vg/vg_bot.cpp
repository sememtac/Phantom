#include "vg_bot.h"
#include "vg_sim.h"
#include "vg_input.h"
#include "vg_weapons.h"
#include "cfg_combat.h"
#include "cfg_flight.h"
#include "cfg_world.h"
#include <math.h>
#ifndef VG_BOT_TRACE
#define VG_BOT_TRACE 0
#endif
#if VG_BOT_TRACE
#include <stdio.h>
#endif

bool vg_bot_on = false;
VgBotTap vg_bot_tap = nullptr;

// ---------------------------------------------------------------------------
// Observing
// ---------------------------------------------------------------------------

// The one this seat is fighting: nearest live opponent. A pilot with four
// contacts would choose, and choosing is a decision worth learning rather than
// hard-coding -- but a policy cannot choose a target it was never shown, and the
// observation carries ONE. That is a limit of this layout and it is written down
// here rather than discovered later: making the bot fight a crowd means widening
// VgObs, not rewriting the controller.
static int nearest_enemy(float* out_r) {
    int   best = -1;
    float bestr = 1e30f;
    for (int i = 0; i < MAX_ENEMIES; i++) {
        const Ship* s = &vg.enemy[i];
        if (!s->alive) continue;
        const float r = vlen(s->pos);
        if (r < bestr) { bestr = r; best = i; }
    }
    if (out_r) *out_r = bestr;
    return best;
}

// The closest missile that is tracking US. from_player is the half that matters:
// our own rounds are not a threat and reading them as one would have the bot
// break every time it fired.
static const Missile* incoming(float* out_r) {
    const Missile* best = nullptr;
    float bestr = 1e30f;
    for (int i = 0; i < MAX_MISSILES; i++) {
        const Missile* m = &vg.msl[i];
        if (!m->alive || m->from_player || !m->locked) continue;
        const float r = vlen(m->pos);
        if (r < bestr) { bestr = r; best = m; }
    }
    if (out_r) *out_r = bestr;
    return best;
}

static inline float clamp1(float x) {
    return x < -1.0f ? -1.0f : (x > 1.0f ? 1.0f : x);
}

void vg_bot_observe(VgObs* o) {
    if (!o) return;
    for (int i = 0; i < VG_OBS_N; i++) o->v[i] = 0.0f;
    o->has_target = false;

    const ShipSpec* sp = vg.spec;
    const float own_span = sp->speed_max - sp->speed_min;

    // ---- our own state, which is true whether or not anyone is out there ----
    o->v[OBS_OWN_HULL]  = (vg.health_max > 0.0f) ? vg.health / vg.health_max : 0.0f;
    o->v[OBS_OWN_SPEED] = (own_span > 1.0f) ? (vg.speed - sp->speed_min) / own_span : 0.0f;
    o->v[OBS_OWN_THROTTLE] = vg.throttle;
    o->v[OBS_OWN_ROUNDS]   = (sp->magazine > 0)
                           ? (float)vg_wpn.rounds / (float)sp->magazine : 0.0f;
    o->v[OBS_OWN_RELOADING] = (vg_wpn.reload_t > 0.0f) ? 1.0f : 0.0f;
    // Progress rather than the bare flag, so a policy can learn to HOLD a nose
    // that is nearly there instead of only reacting once the lock exists.
    o->v[OBS_OWN_LOCK] = vg_wpn.locked ? 1.0f
                       : ((vg_wpn.lock_need > 1e-4f)
                          ? clamp1(vg_wpn.lock_t / vg_wpn.lock_need) : 0.0f);
    // 1 far from the boundary, 0 against it. Inverted from the raw clearance so
    // that every "this is bad" reading in the vector is near zero or near -1,
    // which is one less thing for a policy to learn before it learns to fly.
    o->v[OBS_WALL] = clamp1(vg_wall.clearance / ARENA_ENEMY_MARGIN);
    o->v[OBS_ROLL] = clamp1(vg.roll / 3.14159265f);
    o->v[OBS_OWN_SAAM] = sp->msl_saam ? 1.0f : 0.0f;
    {
        int air = 0;
        for (int i = 0; i < MAX_MISSILES; i++) {
            const Missile* m = &vg.msl[i];
            if (m->alive && m->from_player && m->locked) air++;
        }
        o->v[OBS_OWN_INFLIGHT] = (sp->magazine > 0)
                               ? clamp1((float)air / (float)sp->magazine) : 0.0f;
    }

    // ---- the opponent ----
    float r = 0.0f;
    const int ti = nearest_enemy(&r);
    if (ti >= 0) {
        const Ship* t = &vg.enemy[ti];
        o->has_target = true;

        const Vec3  to  = t->pos;
        const Vec3  los = (r > 1e-3f) ? vmul(to, 1.0f / r) : v3(0, 0, 1);
        o->v[OBS_TGT_X] = los.x;
        o->v[OBS_TGT_Y] = los.y;
        o->v[OBS_TGT_Z] = los.z;
        o->v[OBS_TGT_RANGE] = clamp1(r / (sp->lock_range > 1.0f ? sp->lock_range : 1.0f));

        // Their motion as WE see it: the world slides past us at our own speed,
        // so their apparent velocity is theirs minus ours down +z. Getting this
        // wrong would make a head-on look like a stern chase.
        const float comb = sp->speed_max + t->spec->speed_max;
        const Vec3  tv   = vsub(vmul(t->fwd, t->speed), v3(0, 0, vg.speed));
        o->v[OBS_TGT_VX] = clamp1(tv.x / comb);
        o->v[OBS_TGT_VY] = clamp1(tv.y / comb);
        o->v[OBS_TGT_VZ] = clamp1(tv.z / comb);
        // Closing is the component of that along the line of sight, negated:
        // positive means the range is shrinking, which is the sign a pilot means.
        o->v[OBS_TGT_CLOSURE] = clamp1(-vdot(tv, los) / comb);

        // Who is pointed at whom -- the two halves of a merge, and the pair of
        // numbers that says whether this pass is being won or lost.
        o->v[OBS_TGT_ASPECT]  = clamp1(vdot(t->fwd, vmul(los, -1.0f)));
        o->v[OBS_TGT_OFFBORE] = los.z;   // our nose is +z, so this IS the cosine
        o->v[OBS_TGT_HULL] = (t->spec->hull > 0.0f) ? t->hull / t->spec->hull : 0.0f;
        o->v[OBS_TGT_RANGE_W] = clamp1(r / BOT_RANGE_REF);
    }

    // ---- anything with our name on it ----
    float mr = 0.0f;
    const Missile* m = incoming(&mr);
    if (m) {
        const Vec3 mlos = (mr > 1e-3f) ? vmul(m->pos, 1.0f / mr) : v3(0, 0, 1);
        o->v[OBS_MSL_IN] = 1.0f;
        o->v[OBS_MSL_X]  = mlos.x;
        o->v[OBS_MSL_Y]  = mlos.y;
        o->v[OBS_MSL_Z]  = mlos.z;
        o->v[OBS_MSL_RANGE] = clamp1(mr / ENEMY_EVADE_RANGE);
    }
}

// ---------------------------------------------------------------------------
// Deciding
// ---------------------------------------------------------------------------
//
// A SCRIPTED POLICY, and its only ambition is to be a fair sparring partner.
// Everything it does, the enemy tactics already do from the other side of the
// origin: point at them, slow down to shoot, break across a missile, extend
// after a pass. It is here so that a fight can happen with nobody holding the
// board -- which is what a training run and a measured matchup both need.
//
// IT READS NOTHING BUT THE OBSERVATION. That is the rule the module exists to
// enforce, and it is the reason a trained policy can replace this function
// without any other file changing.

// Committed manoeuvres, so the bot does not re-decide every frame and end up
// doing nothing decisively. The only state it keeps.
static float s_break_t  = 0.0f;   // >0 while extending after a pass
static float s_evade_t  = 0.0f;   // >0 while breaking across a missile
static float s_evade_sx = 1.0f;   // which way it chose to break
static float s_evade_sy = 0.0f;
// The trigger's own memory: whether a press is already outstanding, and the rack
// count it was made at. See the note where the trigger is decided.
static bool  s_fired       = false;
static float s_prev_rounds = 1.0f;

void vg_bot_reset(void) {
    s_fired       = false;
    s_prev_rounds = 1.0f;
    s_break_t = s_evade_t = 0.0f;
    s_evade_sx = 1.0f;
    s_evade_sy = 0.0f;
}

void vg_bot_act(const VgObs* o, VgInput* in, float dt) {
    if (!o || !in) return;

    *in = VgInput{};
    in->throttle = o->v[OBS_OWN_THROTTLE];

    if (s_break_t > 0.0f) s_break_t -= dt;
    if (s_evade_t > 0.0f) s_evade_t -= dt;

    // The bearing we want on the nose, in view space. Steering is then one rule
    // for every case below: bring THIS to the centre of the canopy.
    float wx = o->v[OBS_TGT_X];
    float wy = o->v[OBS_TGT_Y];
    float wz = o->v[OBS_TGT_Z];
    float want_speed = 0.5f;
    bool  may_fire   = true;

    // ---- the wall outranks everybody, exactly as it does for an enemy ----
    if (o->v[OBS_WALL] < 0.35f) {
        // There is no inward bearing in the observation -- a pilot does not get a
        // vector to the middle of the arena, they get a wall coming up. Turning
        // away from the boundary is the same as turning toward the middle only
        // because the arena is convex; that is worth knowing when this is
        // replaced by something trained, which will have to learn it.
        wz = 1.0f; wx = 0.0f; wy = 0.0f;   // fly out along the nose and level off
        want_speed = 0.55f;
        may_fire = false;

    } else if (o->v[OBS_MSL_IN] > 0.5f && o->v[OBS_MSL_RANGE] < 1.0f
               // GUIDING SOMETHING OF MY OWN OUTRANKS DODGING, up to a point.
               //
               // Breaking is right for a dogfighter and ruinous for a sniper: the
               // turn that saves the ship is the turn that drops the lock every
               // one of its rounds is riding. The bot did this every time -- it
               // scored with three classes and never once with BALLISTA, across
               // every opponent, because it threw its own shots away to dodge.
               //
               // So while rounds are out AND the incoming one is not yet at the
               // door, hold the nose and keep illuminating. Inside the last
               // fraction of the evade range it breaks anyway: a hit landed is
               // worth nothing to a ship that is not there to see it.
               && !(o->v[OBS_OWN_SAAM] > 0.5f
                    && o->v[OBS_OWN_INFLIGHT] > 0.0f
                    && o->v[OBS_MSL_RANGE] > 0.40f)) {
        // ACROSS the seeker, never away from it -- turning away is how you get run
        // down. Chosen once and held, or the break is a wobble.
        if (s_evade_t <= 0.0f) {
            s_evade_t  = 1.2f;
            // Perpendicular to where the round is, in the plane of the canopy.
            s_evade_sx = -o->v[OBS_MSL_Y];
            s_evade_sy =  o->v[OBS_MSL_X];
            const float n = sqrtf(s_evade_sx * s_evade_sx + s_evade_sy * s_evade_sy);
            if (n > 1e-3f) { s_evade_sx /= n; s_evade_sy /= n; }
            else           { s_evade_sx = 1.0f; s_evade_sy = 0.0f; }
        }
        wx = s_evade_sx; wy = s_evade_sy; wz = 0.25f;
        // Bleed speed: the turn is only tighter if the throttle goes, which is
        // the same trade the enemy makes and the same one the player has to.
        want_speed = 0.12f;
        may_fire = false;

    } else if (!o->has_target) {
        wx = 0.0f; wy = 0.0f; wz = 1.0f;
        want_speed = 0.6f;
        may_fire = false;

    } else if (s_break_t > 0.0f) {
        // Extending after a pass. Away from them, flat out, and not shooting --
        // the firing gate would refuse at this speed anyway.
        wx = -o->v[OBS_TGT_X]; wy = -o->v[OBS_TGT_Y]; wz = -o->v[OBS_TGT_Z];
        want_speed = 1.0f;
        may_fire = false;

    } else if (o->v[OBS_OWN_SAAM] > 0.5f && o->v[OBS_TGT_RANGE_W] * BOT_RANGE_REF
                                            > ENEMY_BREAK_RANGE * 1.6f) {
        // A SEMI-ACTIVE WEAPON DOES NOT LET YOU LOOK AWAY, so it cannot be flown
        // the way everything else here is flown. The merge below fires and then
        // manoeuvres, and manoeuvring is exactly what drops the lock these rounds
        // are riding -- so every shot went ballistic and a thousand simulated
        // seconds of it did no damage at all.
        //
        // Hold the range and keep them near the nose instead: aim PAST them by
        // enough to stop the range closing, but not so far that the bearing
        // leaves the firing cone. That is the same trick tactic_standoff plays
        // from the other seat, and for the same reason -- this ship cannot fly
        // backwards, so holding a distance means never pointing straight at what
        // it is holding away from.
        float lx = o->v[OBS_TGT_VX], ly = o->v[OBS_TGT_VY], lz = o->v[OBS_TGT_VZ];
        const float along = lx * wx + ly * wy + lz * wz;
        lx -= along * wx; ly -= along * wy; lz -= along * wz;
        const float ln = sqrtf(lx * lx + ly * ly + lz * lz);
        if (ln > 1e-3f) {
            const float k = STANDOFF_ARC / ln;
            wx += lx * k; wy += ly * k; wz += lz * k;
        }
        // Slow, because the firing gate demands it and because closing is the one
        // thing this ship must not do.
        want_speed = 0.15f;

    } else {
        // The merge, and this is where the bot used to kill itself.
        //
        // TWO FAULTS, BOTH OF THEM A COLLISION COURSE BY CONSTRUCTION.
        //
        // It aimed straight at them. A pursuit curve that converges perfectly on
        // its aim point does not produce a firing pass, it produces a ram -- which
        // is the exact sentence tactic_fighter has carried all along, and the
        // reason every enemy aims at a point ENEMY_OFFSET BESIDE the player. The
        // bot was the only pilot in the game flying pure pursuit.
        //
        // And it broke off on `range < 0.10 of my lock range AND the target is no
        // longer in front`. In a head-on merge the target stays dead ahead the
        // whole way in, so the second half was never true and the break never
        // fired. Collision is always fatal, so both of them died on the merge --
        // reported as "they always crash into each other", which is what a pure
        // pursuit flown by both seats looks like.
        const float rw = o->v[OBS_TGT_RANGE_W] * BOT_RANGE_REF;   // world units
        want_speed = (rw > 1100.0f) ? 0.85f : 0.30f;

        // LAG PURSUIT: aim BEHIND their crossing motion rather than at them.
        //
        // The offset direction is not a random side, it is read off where they
        // are going -- the component of their velocity across the line of sight.
        // Aiming opposite it passes astern, which both misses them and arrives in
        // their rear quarter, so the safe choice and the good one are the same
        // choice. Aiming AHEAD of it would be a lead, which is another way to
        // spell collision.
        //
        // Scaled ENEMY_OFFSET / range, so it is nearly nothing at distance and
        // opens up as the merge closes -- the geometry that turns a converging
        // approach into a pass.
        float lx = o->v[OBS_TGT_VX], ly = o->v[OBS_TGT_VY], lz = o->v[OBS_TGT_VZ];
        const float along = lx * wx + ly * wy + lz * wz;
        lx -= along * wx; ly -= along * wy; lz -= along * wz;
        const float ln = sqrtf(lx * lx + ly * ly + lz * lz);
        if (ln > 1e-3f && rw > 1.0f) {
            const float k = (ENEMY_OFFSET / rw) / ln;
            wx -= lx * k; wy -= ly * k; wz -= lz * k;
        }

        // AND THE PASS ENDS ON RANGE ALONE. Whatever the aspect, whoever is
        // pointed at whom: inside this the two of them are about to occupy the
        // same piece of sky. The same number the enemies break at, because it is
        // the same fight seen from the other seat.
        if (rw < ENEMY_BREAK_RANGE) s_break_t = 1.4f;
    }

    // ---- one steering rule ----
    //
    // Bring (wx, wy) to the middle of the canopy. Positive x is right of centre
    // and wants a nose-right command, which is a POSITIVE yaw; positive y is
    // above centre -- world +y projects up, see vg_proj.h -- and wants nose up,
    // which is a NEGATIVE pitch. Getting that second sign wrong flies the ship
    // determinedly into the floor, which is at least an obvious failure.
    //
    // Divided by the forward component so the command grows as the target goes
    // wide, and clamped hard: this is a stick, and a stick has stops.
    const float fwd = (wz > 0.20f) ? wz : 0.20f;
    float yaw   =  (wx / fwd) * 2.2f;
    float pitch = -(wy / fwd) * 2.2f;
    // A target BEHIND us gives a tiny or negative forward component, and the
    // ratio above stops meaning anything. Turn hard, either way, and let the
    // next frame sort out which.
    if (wz < 0.0f) {
        yaw   = (wx >= 0.0f) ? 1.0f : -1.0f;
        pitch = (wy >= 0.0f) ? -1.0f : 1.0f;
    }
    in->yaw   = clamp1(yaw);
    in->pitch = clamp1(pitch);

#if VG_BOT_TRACE
    // A window on what the policy is actually seeing and doing, for when this
    // function is replaced by something whose reasoning cannot be read. OFFBORE
    // is the cosine between the nose and the target, so a controller that is
    // working drives RANGE down while holding that near 1.
    {
        static float acc = 0.0f;
        acc += dt;
        if (acc > 1.0f) {
            acc = 0.0f;
            printf("bot rng=%.3f offbore=%+.3f tgt=(%+.2f %+.2f %+.2f) "
                   "yaw=%+.2f pitch=%+.2f thr=%.2f lock=%.2f\n",
                   (double)o->v[OBS_TGT_RANGE], (double)o->v[OBS_TGT_OFFBORE],
                   (double)o->v[OBS_TGT_X], (double)o->v[OBS_TGT_Y],
                   (double)o->v[OBS_TGT_Z],
                   (double)in->yaw, (double)in->pitch, (double)in->throttle,
                   (double)o->v[OBS_OWN_LOCK]);
        }
    }
#endif

    // ---- throttle, eased ----
    // The real control is a thumb on a strip and cannot jump, so this should not
    // either -- and a policy that learned to chatter the throttle would be
    // learning something the hardware cannot do.
    float t = in->throttle + (want_speed - in->throttle) * (dt * 2.6f);
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    in->throttle = t;

    // ---- the trigger ----
    //
    // AN EDGE, WHICH IS WHAT THE FIELD IS CALLED AND WAS NOT WHAT IT HELD. This
    // used to be a level -- true for as long as a lock stood -- so the bot flew
    // with the trigger squeezed and the flag read 1 on 13.7% of all frames while
    // the weapon's own gates quietly threw nearly all of them away. As behaviour
    // that is merely untidy. As a TRAINING LABEL it is a lie: a policy learning
    // from it would learn to hold a trigger that nobody presses, and the one
    // action that matters would be the noisiest column in the set.
    //
    // Re-armed when the rack count drops, so there is exactly one press per round
    // that actually leaves the rail.
    const bool locked = (o->v[OBS_OWN_LOCK] >= 0.999f);
    const bool want   = may_fire && locked && o->v[OBS_OWN_ROUNDS] > 0.0f;
    const float rounds_now = o->v[OBS_OWN_ROUNDS];
    if (rounds_now < s_prev_rounds - 1e-4f) s_fired = false;   // one left the rail
    in->fire_edge = want && !s_fired;
    in->fire_btn  = in->fire_edge;
    if (in->fire_edge) s_fired = true;
    if (!want)         s_fired = false;
    s_prev_rounds = rounds_now;
}
