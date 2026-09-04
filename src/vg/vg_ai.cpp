#include "vg_sim.h"
#include "vg_arena.h"
#include "vg_bot.h"
#include "vg_modes.h"
#include "vg_wpnsys.h"
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
//   4. defend           -- somebody is on the tail: outrun them if they are
//                          slow, force an overshoot if they are not
//   5. an empty rack    -- extend and reload rather than joust with nothing,
//                          and only while the player is near enough to matter
//   6. the class tactic -- where this hull wants to be, and the only step of
//                          the six that differs between the four
//
// PRESSING A WON POSITION cuts across step 6 rather than joining the list. It is
// not another thing to do, it is a reason not to stop doing the current one --
// see has_the_angle.
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

// HOW FAST THE PLAYER IS GOING, 0 at their own minimum and 1 at their maximum.
//
// Normalised against THEIR envelope rather than an absolute, because the question
// is never "how many units a second" -- it is whether this ship can still chase,
// dodge or run, and that is a question about how much of its own speed range it
// has left.
static float player_speed_norm(void) {
    const float span = vg.spec->speed_max - vg.spec->speed_min;
    if (span <= 1.0f) return 0.0f;
    const float n = (vg.speed - vg.spec->speed_min) / span;
    return n < 0.0f ? 0.0f : (n > 1.0f ? 1.0f : n);
}

// THE LATERAL AIM OFFSET, collapsed against somebody who has stopped.
//
// Every tactic aims at a point BESIDE the player rather than at them, so a
// pursuit curve produces a firing pass instead of a collision. The nose follows
// the flight path, so that offset is also the reason the nose is not on them --
// measured, an enemy holds its aim 76 degrees off the target on average, against
// a lock cone of 31, and has a lock, a round and a cool trigger all at once on
// 0.7% of frames. It is not that it cannot lock a parked ship quickly. It is that
// it never looks at one.
//
// The caution is real against a ship that is moving, because both of you are
// choosing where to be. Against one that has parked it is a courtesy: it cannot
// close the distance you leave, and it cannot make you hit it. So the offset goes
// away as their speed does, the nose comes onto them, and the lock is earnable.
static float aim_offset_for(float base) {
    return base * (1.0f - ENEMY_PRESS_SLOW_K * (1.0f - player_speed_norm()));
}

// The break range, shortened against somebody who has stopped. See the note at
// ENEMY_PRESS_SLOW_K: a parked ship cannot be collided with, cannot chase, and
// every break it is given is a free reset to aim with.
static float break_range_for(float base) {
    return base * (1.0f - ENEMY_PRESS_SLOW_K * (1.0f - player_speed_norm()));
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
    // NERVE SCALES THE RANGE THEY LOSE THEIR PATIENCE AT, and it is divided in
    // rather than multiplied: a bold pilot (nerve > 1) lets the player get
    // CLOSER before breaking, a nervy one leaves sooner. The extend that follows
    // is scaled the other way, so the same number makes one pilot commit and
    // another keep its distance.
    if (s->break_t > 0 || range < break_range_for(ENEMY_BREAK_RANGE / s->nerve)) {
        // Break off and extend. Holding the current heading here instead --
        // which pursuit had already pointed at the player -- flew them
        // straight into the player. Commit to a direction across AND away
        // from the line of sight, long enough to actually open the range.
        if (s->break_t <= 0) {
            s->break_dir = break_across(vnorm(to), s->up, -0.55f);
            s->break_t   = vg_frand(ENEMY_BREAK_TIME_MIN, ENEMY_BREAK_TIME_MAX)
                         / s->nerve;
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
        Vec3 aim  = vmul(s->offset_dir, aim_offset_for(ENEMY_OFFSET));
        *desired  = vsub(aim, s->pos);
        // Close fast, then bleed off to a speed they can actually fight at --
        // the same decision the player has to make.
        s->target_speed = (range > close_r)
                          ? smax * 0.85f
                          : smin + (smax - smin) * 0.35f;
    }
}

// IS THE PLAYER ON MY TAIL AND TRACKING ME?
//
// Three things have to be true at once, and dropping any of them gives a jumpy
// pilot that answers merely being near somebody. They must be CLOSE, they must
// be BEHIND, and they must be POINTED AT ME -- a player crossing astern on the
// way somewhere else is not an attack, and answering it would throw away a pass
// for nothing.
//
// "Pointed at me" is the same test the player's own lock uses: they are the
// origin and they look down +z, so the bearing to this ship is how near their
// nose it sits.
static bool on_my_six(const Ship* s, Vec3 to, float range) {
    if (range > ENEMY_SIX_RANGE || range < 1.0f) return false;
    if (vdot(s->fwd, vnorm(to)) > ENEMY_SIX_COS) return false;   // in front: not a tail
    return vdot(vnorm(s->pos), v3(0, 0, 1)) > ENEMY_SIX_AIM_COS;
}

// WHAT TO DO ABOUT IT, and there are two answers because there are two attacks.
//
// Against a SLOW attacker, leave. Whoever is parked has the best turn rate in
// the game and no ability to follow, so opening the range beats them outright
// and makes them spend the throttle they were saving. That is the answer to
// sitting at zero throttle on a tail, and it is the game's own trade rather than
// a rule invented to punish it.
//
// Against a FAST one, turn across them and bleed speed. They cannot corner with
// a ship that has slowed, so the overshoot is forced -- and a pilot who has
// overshot is in front, which is the whole point of making them do it.
//
// COMMITTED for a second or two. Re-deciding every frame would give a ship that
// jitters between running and turning and does neither.
static void tactic_defend(Ship* s, Vec3 to, float smin, float smax, Vec3* desired) {
    if (s->defend_t <= 0) {
        const float span = vg.spec->speed_max - vg.spec->speed_min;
        const float psn  = (span > 1.0f) ? (vg.speed - vg.spec->speed_min) / span : 0.0f;
        s->defend_run = (psn < ENEMY_SIX_SLOW);
        s->defend_dir = s->defend_run ? vnorm(vmul(to, -1.0f))
                                      : break_across(vnorm(to), s->up, 0.15f);
        s->defend_t   = vg_frand(ENEMY_DEFEND_MIN, ENEMY_DEFEND_MAX);
    }
    *desired = s->defend_dir;
    // Run flat out; break slow. A turn is only tighter if the speed goes.
    s->target_speed = s->defend_run ? smax : smin * 1.15f;
}

// HAVE I WON THIS MERGE?
//
// The mirror of on_my_six, asked about the other pilot, and deliberately
// STRICTER: a ship should need a clearly won position before it abandons its
// class's plan, where it needs only a suspicion of a threat to answer one.
//
// Three things again, and the middle one is the point. They must be CLOSE, they
// must be BEHIND THE PLAYER -- who is the origin looking down +z, so a ship in
// their rear hemisphere has a negative z bearing -- and their own nose must be on
// the player. That last test is what stops a ship merely trailing along behind
// from believing it has an attack.
static bool has_the_angle(const Ship* s, Vec3 to, float range) {
    if (range > ENEMY_ANGLE_RANGE || range < 1.0f) return false;
    if (vdot(vnorm(s->pos), v3(0, 0, 1)) > -ENEMY_ANGLE_BEHIND) return false;
    return vdot(s->fwd, vnorm(to)) > ENEMY_ANGLE_NOSE;
}

// WHERE THIS PILOT IS POINTING, as against where the ship is facing.
//
// A slow wander of the aim direction, re-rolled on the character's own hold
// time, whose width is the character's `aim`. Everything that AIMS reads the
// result -- the lock and the launch -- and nothing that FLIES does, because being
// a poor shot and being unable to fly an aeroplane are different failings and
// the game already had a trait for the second one.
//
// PERPENDICULAR, AND RE-NORMALISED. The offset is built across the nose rather
// than along it, so it is a pointing error and not a range error, and the sum is
// renormalised so the aim stays a direction.
//
// HELD RATHER THAN RESAMPLED PER FRAME, and that is the whole feel of it. A
// per-frame jitter averages out over the length of a lock and costs a pilot
// almost nothing; an error that SITS for a second and a bit is one they have to
// fly out of. That is why the hold time is a trait and not a constant.
static void update_aim(Ship* s, float dt) {
    s->aim_t -= dt;
    if (s->aim_t <= 0.0f) {
        const float w = s->pilot->aim;
        if (w > 1e-4f) {
            // Across the nose: two perpendicular axes, each drawn independently,
            // so the error has no preferred side.
            Vec3 up   = s->up;
            Vec3 side = vcross(up, s->fwd);
            if (vlen2(side) < 1e-4f) side = vcross(v3(1, 0, 0), s->fwd);
            side = vnorm(side);
            up   = vnorm(vcross(s->fwd, side));
            s->aim_off = vadd(vmul(side, vg_frand(-w, w)),
                              vmul(up,   vg_frand(-w, w)));
        } else {
            s->aim_off = v3(0, 0, 0);
        }
        s->aim_t = s->pilot->aim_hold * vg_frand(0.7f, 1.3f);
    }
    s->aim_dir = vnorm(vadd(s->fwd, s->aim_off));
}

// WHICH LAYER OWNS THE SHAPE OF THE FIGHT, per class.
//
// The network is a function of ONE FRAME. Breaking off, extending and coming
// back in is a plan that runs for ten seconds, and no policy without memory can
// hold one -- which is why asking it to own survival turned the classes that
// have a plan into tail-chasers. The hand-written tactics DO have memory: it is
// break_t, reset_t and press_t, and those timers are the plan.
//
// So the two classes whose identity is a shape rather than a reflex keep their
// tactic above the network, and the network flies inside it -- still every
// moment-to-moment decision, just no longer choosing whether the fight is a
// merge or a break. Measured over six seeds, damage per run:
//
//     AEGIS    1241 net / 980 tactic     LANCE     322 net / 194 tactic
//     CHARIOT   540 net / 652 tactic     BALLISTA  132 net / 207 tactic
//
// A reflex class is better served by the reflex, and a shape class by the shape.
// This is not a tuning constant -- it is which of the two has the memory the
// class needs, and the tactic enum already says which class is which.
static bool net_owns_phase(ShipTactic t) {
    switch (t) {
        case TACTIC_SLASH:      // CHARIOT -- the pass has to be set up
        case TACTIC_STANDOFF:   // BALLISTA -- the whole class is holding a range
            return false;
        case TACTIC_FIGHTER:
        case TACTIC_GEOMETRY:
        default:
            return true;
    }
}

// THE ENEMY'S LOCK, WHICH IS THE PLAYER'S LOCK.
//
// A deliberate mirror of vg_update_lock, and it should stay one: every rule here
// is read off the ship being flown rather than chosen for the AI, so a class
// change moves both pilots at once and neither can end up exempt.
//
//   ACQUIRE in lock_cos, HOLD in lock_hold_cos -- the same two-cone split, and
//   for the same reason. Putting the nose on somebody is the aim; keeping a
//   target you already have is only asking whether you have lost them.
//
//   LOCK TIME SCALES WITH SPEED, through the same LOCK_SPEED_PENALTY. This is
//   what makes the AI's existing habit of slowing down to shoot cost it
//   something real rather than merely satisfying a threshold.
//
// "MIRROR" IS NOW ONLY TRUE OF THE CONE TEST BELOW. The speed normalisation, the
// lock time it feeds, the latch and the whole of the banking are the SAME CALLS
// the player's seat makes -- see vg_wpnsys.h -- which is the only form of this
// claim a reader can check. What is left here is the part that genuinely cannot
// cross over, and it is one branch.
//
// THE VIEWPORT IDIOM CANNOT CROSS OVER. lock_hold_cos below -1 means "as long as
// it is on screen", and an enemy has no screen; ENEMY_LOCK_HOLD_WIDE is that rule
// in the only terms it has. See cfg_combat.h.
static void enemy_update_lock(Ship* s, const ShipSpec* sp, Vec3 to, float range,
                              float sn, float dt) {
    bool ok = (range <= sp->lock_range && range >= 1.0f);
    if (ok) {
        const float c = vdot(s->aim_dir, vnorm(to));
        if (!s->locked) {
            ok = (c > vg_lock_cos_at(sp, range, false));
        } else if (sp->lock_hold_cos < -1.0f) {
            ok = (c > ENEMY_LOCK_HOLD_WIDE);
        } else {
            ok = (c > vg_lock_cos_at(sp, range, true));
        }
    }

    if (!ok) {
        // A lock that breaks costs the nose to get back, exactly as the player's
        // does. That is what puts the choice between manoeuvring and shooting
        // into an enemy's flying too, instead of only into the player's.
        s->lock_t  = 0.0f;
        s->locked  = false;
        vg_wpn_bank_drop(*s);    // the bank goes with the lock
        return;
    }

    vg_wpn_lock_hold(*s, dt, vg_wpn_lock_need(sp, sn));

    // BANKING. Not "exactly as the player's seat does it" any more -- the SAME
    // CALL the player's seat makes, which is the only version of that claim a
    // reader can check. See vg_wpnsys.h.
    vg_wpn_bank_step(*s, sp, dt);
}

// NOTHING IN THE RACK, AND THE PLAYER IS CLOSE ENOUGH FOR THAT TO MATTER.
//
// Across and away, flat out, and no cleverness: this is not a manoeuvre, it is
// leaving. The reload runs on its own clock down in the firing code, so coming
// back to the fight is simply this test going false.
static void tactic_dry(Ship* s, Vec3 to, float smax, Vec3* desired) {
    if (s->break_t <= 0) {
        s->break_dir = break_across(vnorm(to), s->up, -0.85f);
        s->break_t   = vg_frand(0.8f, 1.4f);
    }
    *desired = s->break_dir;
    s->target_speed = smax * ENEMY_DRY_SPEED_K;
}

// STANDOFF -- the one that fights a different fight rather than the same fight
// with different numbers.
//
// It cannot fly backwards, so holding a range means never pointing straight at
// the thing it is holding away from. But it also has to keep its nose on the
// player to be allowed to shoot. Those two pull against each other, and the
// answer is to aim PAST the player: a point out to the side, far enough that the
// closing rate falls away, near enough that the bearing stays inside the firing
// cone. That is a wide arc, and it is what a gun platform circling its target
// actually looks like.
//
// Inside the flee range it stops arguing and runs. That is the whole counter to
// the class -- get inside and it has nothing -- so it has to be visible from the
// cockpit: the ship turns its back and leaves.
static void tactic_standoff(Ship* s, const ShipSpec* sp, Vec3 to, float range,
                            float smin, float smax, Vec3* desired) {
    const float hold = sp->lock_range * STANDOFF_HOLD_K;
    // NOT shortened against a slow target, unlike the merge breaks. Those exist
    // as caution about the other ship and a parked one is not worth being cautious
    // about. This one is the CLASS: a standoff hull wants its range whoever it is
    // facing, and handing it a reason to close because the target slowed down
    // would trade the identity away for a press it does not need. It shoots from
    // out there; that is the whole point of it.
    const float flee = ENEMY_BREAK_RANGE * STANDOFF_FLEE_K;

    // DID THE LAST RUN ACHIEVE ANYTHING? Judged when it ends rather than while it
    // is happening, because a run has to be given its whole length first.
    //
    // A standoff hull is the slowest thing in the game and it flees from ships
    // that are faster, so running is not always available. A pilot who has just
    // discovered that turns and fights instead of repeating it, which is the
    // difference between a plan and a loop.
    if (s->break_t <= 0.0f && s->flee_r0 > 0.0f) {
        if (range < s->flee_r0 + STANDOFF_FLEE_GAIN) s->flee_cd = STANDOFF_FLEE_GIVEUP;
        s->flee_r0 = 0.0f;
    }

    if (s->break_t > 0 || (range < flee && s->flee_cd <= 0.0f)) {
        if (s->break_t <= 0) {
            s->break_dir = vnorm(vmul(to, -1.0f));   // straight out, no cleverness
            s->break_t   = vg_frand(1.2f, 2.2f);
            s->flee_r0   = range;                    // what the run will be judged on
        }
        *desired = s->break_dir;
        s->target_speed = smax;
        return;
    }

    // The side it arcs around. offset_dir is already a stable random unit that is
    // re-rolled on a break, so the arc does not jitter frame to frame.
    Vec3 los  = vnorm(to);
    Vec3 side = vsub(s->offset_dir, vmul(los, vdot(s->offset_dir, los)));
    if (vlen2(side) < 1e-4f) side = vcross(los, s->up);
    side = vnorm(side);

    if (range > hold * 1.15f) {
        // Still outside the band: close, and aim nearly straight in.
        *desired = vadd(to, vmul(side, range * STANDOFF_ARC * 0.35f));
        s->target_speed = smax * 0.85f;
    } else {
        // In the band. Slow enough to be allowed to fire, arcing wide enough
        // that it does not walk straight into the merge it is trying to avoid.
        *desired = vadd(to, vmul(side, range * STANDOFF_ARC));
        s->target_speed = smin * 1.15f;
    }
}

// SLASH -- the same shape as the fighter, flown harder and further.
static void tactic_slash(Ship* s, const ShipSpec* sp, Vec3 to, float range,
                         float close_r, float smin, float smax, Vec3* desired) {
    (void)sp;
    if (s->break_t > 0
        || range < break_range_for(ENEMY_BREAK_RANGE * SLASH_BREAK_K / s->nerve)) {
        if (s->break_t <= 0) {
            s->break_dir = break_across(vnorm(to), s->up, -0.75f);
            s->break_t   = vg_frand(ENEMY_BREAK_TIME_MIN * SLASH_EXTEND_K,
                                    ENEMY_BREAK_TIME_MAX * SLASH_EXTEND_K)
                         / s->nerve;
            s->offset_dir = vg_rand_unit();
        }
        *desired = s->break_dir;
        s->target_speed = smax;
    } else {
        // A wide offset, so the pass CROSSES instead of converging. A slash that
        // converges is a joust, and a joust is what the fighter already does.
        Vec3 aim  = vmul(s->offset_dir, aim_offset_for(ENEMY_OFFSET * SLASH_OFFSET_K));
        *desired  = vsub(aim, s->pos);
        s->target_speed = (range > close_r) ? smax
                        : smin + (smax - smin) * SLASH_SPEED_K;
    }
}

// GEOMETRY -- the fighter, but it wants to be lined up more than it wants to be
// safe.
static void tactic_geometry(Ship* s, const ShipSpec* sp, Vec3 to, float range,
                            float close_r, float smin, float smax, Vec3* desired) {
    (void)sp;
    if (s->break_t > 0
        || range < break_range_for(ENEMY_BREAK_RANGE * GEOM_BREAK_K / s->nerve)) {
        if (s->break_t <= 0) {
            s->break_dir = break_across(vnorm(to), s->up, -0.55f);
            s->break_t   = vg_frand(ENEMY_BREAK_TIME_MIN, ENEMY_BREAK_TIME_MAX)
                         / s->nerve;
            s->offset_dir = vg_rand_unit();
        }
        *desired = s->break_dir;
        s->target_speed = smax;
    } else {
        Vec3 aim  = vmul(s->offset_dir, aim_offset_for(ENEMY_OFFSET * GEOM_OFFSET_K));
        *desired  = vsub(aim, s->pos);
        s->target_speed = (range > close_r) ? smax * 0.85f
                        : smin + (smax - smin) * GEOM_SPEED_K;
    }
}

// WHAT THE PILOTS ARE ACTUALLY DOING, counted per class.
//
// This existed three times tonight as a throwaway probe and was deleted three
// times, which is the definition of something that should be permanent. It is
// how the biggest finding of the week was made: an AEGIS flown by the network
// spends 67% of its frames in the network and 0% in evade, tail, extend, press
// or its class tactic, where the same hull on the tactics spreads across all of
// them. No amount of playing tells you that; one column of numbers does.
//
// Unconditional and cheap, like the missile counters next door: a handful of
// increments on a path that already does far more work than this.
// FLAT, so the declaration in vg_prof.h cannot disagree with the definition
// about a dimension. It did, and a two-dimensional extern mangles differently.
// (int) on the first: multiplying two different enum types is deprecated in C++20.
uint32_t g_ai_steer[(int)SHIP_CLASSES * STEER_KINDS] = { 0 };
uint32_t g_ai_frames[SHIP_CLASSES]  = { 0 };
uint32_t g_ai_locked[SHIP_CLASSES]  = { 0 };
uint32_t g_ai_armed[SHIP_CLASSES]   = { 0 };   // lock + round + cool trigger at once
float    g_ai_aim[SHIP_CLASSES]     = { 0 };   // summed cos(aim, target)
float    g_ai_range[SHIP_CLASSES]   = { 0 };   // summed range, world units
// ...and the same aim, split by what the pilot was DOING at the time. A mean over
// the whole fight is dominated by the frames where not aiming is correct: a ship
// with an empty rack should be leaving, not pointing.
float    g_ai_aimk[(int)SHIP_CLASSES * STEER_KINDS] = { 0 };


// Which row of the class table this hull is, or -1 if it is not one of them.
static int class_of(const ShipSpec* sp) {
    const int i = (int)(sp - &vg_ship_class[0]);
    return (i >= 0 && i < SHIP_CLASSES) ? i : -1;
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

    update_aim(s, dt);

    float  inc_range = 1e9f;
    const Missile* inc = incoming_for(index, &inc_range);

    // HAVE THEY NOTICED YET?
    //
    // A threat is a missile tracking them or somebody on their tail. The clock
    // starts on the frame one APPEARS, not on every frame one exists -- so a
    // pilot pays their reaction once per attack and is not permanently stunned
    // by a long chase. Until it expires they carry on doing whatever they were
    // doing, which is exactly what being slow to notice looks like from outside.
    //
    // WALL AVOIDANCE AND THE SUICIDE RUN ARE EXEMPT, and they should be: one is
    // not a reaction to anybody and the other is a decision already taken. The
    // gate is on ANSWERING an attack, which is the only thing a person can be
    // late to.
    const bool threat_now =
        (inc && inc_range < ENEMY_EVADE_RANGE) ||
        on_my_six(s, vsub(v3(0, 0, 0), s->pos), vlen(s->pos));
    if (threat_now && !s->threat_seen) s->react_t = s->pilot->reaction;
    s->threat_seen = threat_now;
    if (s->react_t > 0.0f) s->react_t -= dt;
    const bool reacted = (s->react_t <= 0.0f);

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

    // IS THIS FIGHT GOING ANYWHERE? A hit either way resets the clock; otherwise
    // it runs. The player's hull is read once, on the first ship, so four
    // opponents do not each see the same hit as their own.
    static float s_prev_player_hp = 1.0f;
    if (index == 0) {
        if (vg.health < s_prev_player_hp - 1e-4f) {
            for (int i = 0; i < MAX_ENEMIES; i++) vg.enemy[i].stale_t = 0.0f;
        }
        s_prev_player_hp = vg.health;
    }
    if (s->hull < s->prev_hull - 1e-4f) s->stale_t = 0.0f;
    else                                s->stale_t += dt;
    s->prev_hull = s->hull;
    if (s->reset_t > 0.0f) s->reset_t -= dt;

    s->steer_by = STEER_NONE;
    if (eclear < ARENA_ENEMY_MARGIN) {
        s->steer_by = STEER_WALL;
        desired = vg_arena_dir_to_view(vg_arena_inward(elocal));
        s->target_speed = (smin + smax) * 0.5f;
        s->evade_t = 0;
        s->break_t = 0;

    } else if (s->kamikaze_on) {
        s->steer_by = STEER_RAM;
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

    // TOO CLOSE, AND CONTACT KILLS THE PLAYER OUTRIGHT. See ENEMY_MERGE_FLOOR:
    // this is the floor the network has always had and the tactics never did,
    // and without it an aggressive pilot converts its own aggression into the
    // player's death by touching them. Under the wall and under a suicide run,
    // because one is equally fatal and the other is a decision already taken.
    } else if (vlen2(s->pos) < ENEMY_MERGE_FLOOR * ENEMY_MERGE_FLOOR) {
        s->steer_by = STEER_EVADE;
        desired     = break_across(vnorm(vsub(v3(0, 0, 0), s->pos)), s->up, -0.75f);
        s->target_speed = smax;
        s->attack_t = 0.0f;      // the pass is over; it got what it was going to get
        s->break_t  = 0.0f;

    // NOTHING IS WORKING, SO STOP DOING IT. Committed for a couple of seconds:
    // away and across, at speed, with the approach offset re-rolled so the
    // re-merge comes from a side the last one did not.
    //
    // ABOVE the network and below the wall and the missile, because it is a
    // decision about the FIGHT rather than about this second -- but staying alive
    // still outranks changing the subject.
    } else if (s->reset_t > 0.0f || s->stale_t > ENEMY_STALE_TIME) {
        if (s->reset_t <= 0.0f) {
            s->reset_dir  = break_across(vnorm(vsub(v3(0, 0, 0), s->pos)), s->up, -0.7f);
            s->reset_t    = vg_frand(ENEMY_RESET_MIN, ENEMY_RESET_MAX);
            s->offset_dir = vg_rand_unit();
            s->stale_t    = 0.0f;
            s->press_t    = 0.0f;   // a held position is part of what was not working
        }
        s->steer_by     = STEER_RESET;
        desired         = s->reset_dir;
        s->target_speed = smax;

    // THE NETWORK, ASKED EARLY, when it is allowed to own more than positioning.
    // It declines a class it never learned and a boundary, so the layers below
    // still fly everything it will not.
    } else if (vg_net_owns_survival && net_owns_phase(sp->tactic) &&
               vg_bot_fly_enemy(index, s, &desired, &s->target_speed, dt)) {
        s->steer_by = STEER_NET;

    } else if (reacted && inc && inc_range < ENEMY_EVADE_RANGE) {
        s->steer_by = STEER_EVADE;
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

    } else if (s->defend_t > 0
               || (reacted &&
                   on_my_six(s, vsub(v3(0, 0, 0), s->pos), vlen(s->pos)))) {
        // ABOVE THE TACTIC, BELOW THE MISSILE. A round already in the air is the
        // more urgent of the two, and a pilot being tracked still has time to
        // answer properly -- but neither can wait for the class's positioning
        // plan, which assumes the fight is still being flown forwards.
        s->evade_t = 0;
        s->steer_by = STEER_TAIL;
        tactic_defend(s, vsub(v3(0, 0, 0), s->pos), smin, smax, &desired);

    } else {
        s->evade_t = 0;

        Vec3  to    = vsub(v3(0, 0, 0), s->pos);   // player is the origin
        float range = vlen(to);

        // PRESSING, decided before the tactic and spent inside it.
        //
        // Every tactic breaks off on a RANGE test, and range cannot tell a merge
        // that went well from one that went badly -- so the pass that ended with
        // this ship behind the player, nose on, was abandoned exactly like the
        // one that ended in front of them. That is why a fight read as a series
        // of jousts with no consequence to losing one.
        //
        // Held for ENEMY_PRESS_TIME once taken, because a hard turn breaks the
        // angle for a frame at a time and re-deciding every frame would drop the
        // pursuit constantly. Ended by ENEMY_PRESS_MAX whatever happens: a pilot
        // who keeps just barely holding the angle would otherwise never let go,
        // and an opponent that cannot be shaken is not a fight, it is a fact.
        //
        // ROUNDS IN THE RACK ARE PART OF IT. Sitting on somebody's tail with an
        // empty magazine is the same mistake as jousting with one.
        //
        // press_run is the ceiling and it is why there are two timers. press_t is
        // the COMMITMENT, refreshed while the angle holds so that a hard turn
        // breaking it for a frame does not drop the pursuit; press_run is how long
        // this one pursuit has lasted, and once it passes ENEMY_PRESS_MAX the
        // refresh stops and the commitment simply expires. It is cleared when the
        // pursuit ends, so re-acquiring later is a NEW pursuit with a full budget
        // rather than a continuation that has already spent it.
        if (s->press_t > 0.0f) {
            s->press_t   -= dt;
            s->press_run += dt;
        } else {
            s->press_run = 0.0f;
        }
        if (s->rounds > 0 && s->press_run < ENEMY_PRESS_MAX * s->pilot->press &&
            has_the_angle(s, to, range)) {
            s->press_t = ENEMY_PRESS_TIME * s->pilot->press;
        }
        const bool pressing = (s->press_t > 0.0f);
        // The break is what pressing suppresses, so cancelling the timer is the
        // whole mechanism: every tactic's first test is `break_t > 0`.
        if (pressing) s->break_t = 0;

        // AN EMPTY RACK OUTRANKS THE CLASS PLAN, and sits under defend: something
        // shooting at this ship now is worth more than a weapon it will have
        // shortly. It outranks pressing too -- see the note above about a tail
        // held with nothing to fire -- and has_the_angle already refuses to arm
        // on an empty rack, so this only has to run first.
        const bool dry = (s->rounds <= 0 && s->reload_t > 0.0f &&
                          range < ENEMY_DRY_RANGE);
        if (dry) {
            s->steer_by = STEER_DRY;
            tactic_dry(s, to, smax, &desired);
        } else
        // THE TRAINED PILOT, IF THERE IS ONE AND IT WILL TAKE THE FIGHT.
        //
        // It sits exactly where a class tactic sits, and that is the whole design
        // of this function paying off: everything above -- the wall, the suicide
        // run, the missile, the tail -- is survival that every hull does the same
        // way, and everything here is POSITIONING. A network is another opinion
        // about positioning.
        //
        // It declines near the boundary and when there is nothing to fight, and
        // then the class tactic flies as it always did. There is no frame where
        // nobody is steering.
        {
            // THE NETWORK FIRST, because asking it is what decides the mode: one
            // forward pass gives both where to point this frame and what the ship
            // is trying to do over the next few seconds. Reading the mode before
            // this had run would always read the previous frame's answer, and the
            // whole strategy layer would sit in a branch that never executes --
            // which is exactly what it did until this was measured.
            //
            // The firing gates below are untouched either way, so a network-flown
            // enemy still has to be slow enough and hold a lock long enough, in its
            // own class's cone. It gets a different opinion about where to be, not
            // a different game.
            const bool net_flew =
                vg_bot_fly_enemy(index, s, &desired, &s->target_speed, dt);
        // WHAT THE POLICY THINKS THIS SHIP IS DOING, if it has an opinion.
        //
        // This is the strategy layer and it sits ABOVE the class tactic rather
        // than beside it: the tactic answers "how does this hull fight", the mode
        // answers "what am I trying to do for the next few seconds". A CHARIOT
        // that has decided to EXTEND leaves like a CHARIOT, and a BALLISTA that
        // has decided to EXTEND leaves like a BALLISTA -- the mode picks the
        // verb and the class still supplies the manner.
        //
        // PRESS is deliberately the class tactic itself rather than a fifth
        // behaviour. Closing and taking the shot IS what every tactic here does;
        // giving it a separate implementation would only mean two of them to keep
        // in step.
        //
        // -1 means the network has no opinion -- an untrained class, older
        // weights, a frame it declined -- and then this whole block is skipped and
        // the class flies exactly as it did before modes existed.
        const int mode = vg_bot_enemy_mode(index);
        bool by_mode = true;
        switch (mode) {
            case VG_MODE_BREAK:
                // Across whatever is coming, or across the player when nothing is.
                // The same manoeuvre the evade path flies, chosen a second early
                // and on purpose rather than as a reaction.
                s->steer_by = STEER_EVADE;
                if (s->evade_t <= 0) {
                    const Vec3 away = inc ? inc->dir : vnorm(vsub(v3(0, 0, 0), s->pos));
                    s->evade_dir = vnorm(vadd(break_across(away, s->up, 0.0f),
                                              vmul(vg_rand_unit(), 0.35f)));
                    s->evade_t   = vg_frand(0.9f, 1.6f);
                }
                desired = s->evade_dir;
                s->target_speed = smin * 1.15f;
                break;
            case VG_MODE_EXTEND:
                // Away and across at speed. tactic_dry is already exactly this --
                // it is what a ship does when it has nothing to shoot with -- and
                // leaving on purpose is the same manoeuvre for a better reason.
                s->steer_by = STEER_DRY;
                tactic_dry(s, to, smax, &desired);
                break;
            case VG_MODE_HOLD:
                // Keep the range and keep the nose. Not a retreat and not a
                // commitment: the seconds a lock is earned in.
                s->steer_by = STEER_PRESS;
                tactic_standoff(s, sp, to, range, smin, smax, &desired);
                break;
            default:
                by_mode = false;   // PRESS, or no opinion at all
                break;
        }
        // THE AIMED SHOT, and it is the one thing the tactics never do.
        //
        // Every one of them flies a curve past the player. That is right against
        // somebody who is moving -- a straight run at a ship that can shoot back
        // is how you get shot -- but against one that has parked it means the nose
        // is never on them and the lock is never earned. See ENEMY_PRESS_SLOW_AT.
        //
        // So: stop flying the pattern, slow to where the lock comes quickly, and
        // point. It stays subject to every firing gate, and it stops the moment
        // they move again or the lock is in hand and the class can go back to
        // fighting properly.
        // HELD, and dropping out on `locked` was the whole of why this did not
        // bite. The moment the lock was earned the ship went back to flying its
        // curve, which pointed the nose away, which broke the lock, which brought
        // it back here -- an oscillation that never held a firing solution long
        // enough to spend more than a round or two of the rack.
        //
        // A pilot who has a parked target in their sights does not immediately
        // look somewhere else. It holds until the rack is dry or they move.
        // A FIRING PASS AT SOMETHING THAT IS MOVING.
        //
        // The slow-target press below is the easy half: a parked ship cannot
        // punish an approach, so pointing at it costs nothing. This is the other
        // half, and it costs something on purpose -- lining up on a ship that can
        // shoot back means being pointed at by it too. That is what a pass IS.
        //
        // Committed, because a run decided fresh every frame is a ship that
        // half-points at somebody forever. It ends on its own clock, or the break
        // at ENEMY_BREAK_RANGE ends it -- and that is what turns a converging
        // chase into line up, shoot, break, come back.
        if (s->attack_cd > 0.0f) s->attack_cd -= dt;
        if (s->attack_t  > 0.0f) s->attack_t  -= dt;
        // NOT A STANDOFF HULL, for the third time tonight and for the same reason
        // each time. A firing pass is a decision to close, and its lock range is
        // 4200 -- so "in the window" is almost always true and it would spend the
        // fight running at people. Measured, its damage in ordinary flying went
        // 0.55 -> 1.94: the sniper stops sniping. It already has an aimed shot for
        // a target that has parked, which is the only time it should leave.
        if (sp->tactic != TACTIC_STANDOFF
            && s->attack_t <= 0.0f && s->attack_cd <= 0.0f && reacted
            && s->rounds > 0 && !s->locked
            && range < sp->lock_range * ENEMY_ATTACK_RANGE_K
            && range > ENEMY_ATTACK_MIN) {
            s->attack_t  = vg_frand(ENEMY_ATTACK_TIME_MIN, ENEMY_ATTACK_TIME_MAX)
                         * s->pilot->nerve;
            s->attack_cd = ENEMY_ATTACK_COOLDOWN;
        }

        // THE PASS ENDS AT THE BREAK RANGE, and leaving this out made it a ram.
        //
        // A run sets by_mode, which skips the class tactic -- and the break lives
        // INSIDE the tactic. So the nose stayed on the player for the whole
        // committed window with nothing left to call it off, and a manoeuvre meant
        // to be line up, shoot, break became line up, shoot, arrive. Played, it
        // read as the enemy laying a trap.
        //
        // Cancelling the run here rather than breaking directly hands the frame
        // back to the tactic, whose own break test is immediately true at this
        // range. One break, owned by one piece of code.
        if (s->attack_t > 0.0f && range < ENEMY_BREAK_RANGE) s->attack_t = 0.0f;

        const float press_at = (sp->tactic == TACTIC_STANDOFF)
                             ? ENEMY_PRESS_SLOW_AT : ENEMY_PRESS_SLOW_CLOSE;
        if (!by_mode && s->attack_t > 0.0f && s->rounds > 0) {
            s->steer_by     = STEER_PRESS;
            desired         = vsub(v3(0, 0, 0), s->pos);
            // Slow for the same reason the parked press is slow: LOCK_SPEED_PENALTY
            // is 1.8, so a ship at full throttle needs nearly three times as long
            // to earn the lock it came here for.
            s->target_speed = smin * 1.25f;
            by_mode         = true;
        } else if (!by_mode && player_speed_norm() < press_at
            && s->rounds > 0 && range < sp->lock_range) {
            s->steer_by = STEER_PRESS;
            desired     = vsub(v3(0, 0, 0), s->pos);
            // TWO PHASES, because holding a firing solution at fifteen hundred
            // units is not pressure -- it is a threat nobody has to answer.
            //
            // Outside the working range, CLOSE, at speed: the lock is worthless
            // until the round has a short enough flight for a parked ship to still
            // be there when it arrives. Inside it, slow down, because the lock
            // time is scaled by how fast the shooter is going -- LOCK_SPEED_PENALTY
            // is 1.8, and a ship at full throttle needs nearly three times as long
            // to earn the same lock. The player already knows this. It is why they
            // stopped.
            // SLOW THE WHOLE TIME, and closing at speed first was measured and
            // thrown away: it arrives sooner and cannot shoot when it does,
            // because LOCK_SPEED_PENALTY is 1.8 and a ship at full throttle needs
            // nearly three times as long to earn the same lock. Damage against a
            // parked target FELL on every class. At this speed the nose is already
            // on them, so it drifts in while it works the lock, which is both the
            // approach and the aim.
            s->target_speed = smin * 1.15f;
            by_mode         = true;
        }

        // PRESS is the network's own flying: closing and taking the shot is what
        // it was fitted to do, so there is nothing to add. No opinion at all falls
        // back to the class tactic, exactly as before there were modes.
        if (!by_mode && net_flew) s->steer_by = STEER_NET;
        else if (!by_mode)
        switch (sp->tactic) {
            case TACTIC_STANDOFF:
                tactic_standoff(s, sp, to, range, smin, smax, &desired);
                break;
            case TACTIC_SLASH:
                tactic_slash(s, sp, to, range, close_r, smin, smax, &desired);
                break;
            case TACTIC_GEOMETRY:
                tactic_geometry(s, sp, to, range, close_r, smin, smax, &desired);
                break;
            case TACTIC_FIGHTER:
            default:
                tactic_fighter(s, sp, to, range, close_r, smin, smax, &desired);
                break;
        }
        }
        if (s->steer_by == STEER_NONE) s->steer_by = STEER_TACTIC;

        // Holding the angle means holding the NOSE on them, and the tactic has
        // just aimed at a point beside the player so the pass does not converge.
        // Overriding it here rather than threading a flag through all four keeps
        // the tactics about positioning, which is the one thing they are for.
        if (pressing && !dry) {
            // Last word, so it is the label too.
            s->steer_by = STEER_PRESS;
            desired = to;
            // Slow enough to be allowed to shoot -- the firing gate applies to a
            // won position exactly as it does to a merge -- and slow is also what
            // lets them stay inside the player's turn.
            s->target_speed = smin + (smax - smin) * 0.30f;
        }
    }

    {   // THE CENSUS. Taken after the whole chain has decided, so steer_by is
        // final rather than whatever an earlier branch happened to leave.
        const int c = class_of(sp);
        if (c >= 0) {
            g_ai_frames[c]++;
            if (s->steer_by < STEER_KINDS) g_ai_steer[c * STEER_KINDS + s->steer_by]++;
            if (s->locked) g_ai_locked[c]++;
            if (s->locked && s->rounds > 0 && s->fire_cd <= 0.0f) g_ai_armed[c]++;
            const float rr = vlen(s->pos);
            g_ai_range[c] += rr;
            if (rr > 1.0f) {
                const float cs = vdot(s->aim_dir, vmul(s->pos, -1.0f / rr));
                g_ai_aim[c] += cs;
                if (s->steer_by < STEER_KINDS)
                    g_ai_aimk[c * STEER_KINDS + s->steer_by] += cs;
            }
        }
    }

    if (s->flee_cd  > 0) s->flee_cd  -= dt;
    if (s->evade_t  > 0) s->evade_t  -= dt;
    if (s->break_t  > 0) s->break_t  -= dt;
    if (s->defend_t > 0) s->defend_t -= dt;

    // Same trade the player gets: bleeding speed buys turn rate. Without this
    // their evasive break is too lazy to ever defeat a seeker.
    float snorm = (s->speed - smin) / (smax - smin);
    if (snorm < 0) snorm = 0; else if (snorm > 1) snorm = 1;
    float erate = sp->turn_rate * s->skill
                * (1.0f + sp->agility_slow_bonus * (1.0f - snorm)
                        - sp->agility_fast_malus * snorm);

    // CORNERED. Applied after every layer has had its say, because it is not a
    // different plan -- it is the same plan, pressed. Whatever the ship decided to
    // do, it does it while coming at the player rather than away from them.
    //
    // SKIPPED FOR THE THINGS THAT OUTRANK A FIGHT. Not into a wall, not while
    // breaking from a missile, and not during a suicide run, which is already
    // this taken to its end.
    if (s->hull < sp->hull * ENEMY_CORNERED_HULL &&
        s->steer_by != STEER_WALL && s->steer_by != STEER_EVADE &&
        s->steer_by != STEER_RAM) {
        const Vec3  to = vsub(v3(0, 0, 0), s->pos);
        const float r  = vlen(to);
        if (r > 1.0f) {
            const float k = ENEMY_CORNERED_PULL;
            desired = vnorm(vadd(vmul(desired, 1.0f - k), vmul(vmul(to, 1.0f / r), k)));
            const float want = smin + (smax - smin) * ENEMY_CORNERED_SPEED;
            if (want > s->target_speed) s->target_speed = want;
            s->steer_by = STEER_CORNER;
        }
    }

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
    // AN ENEMY CARRIES A RACK NOW, AND THE RACK IS THE CLASS.
    //
    // This used to be one cooldown for everybody: the whole magazine and the
    // reload averaged into a SUSTAINED rate, and a round sent on that interval for
    // ever. Every gate above was per-class and this was not, so the one thing a
    // class does with a trigger was the one thing the AI could not do.
    //
    // It flattened CHARIOT worst, which is what a playtest reported. Twelve rounds
    // at 0.16 is the whole rack in under two seconds and then ten seconds holding
    // nothing -- the identity of the ship. Averaged, it came out at a round every
    // 2.1s, against AEGIS's 2.8: the glass cannon and the reference fighter, near
    // enough the same on the trigger. BALLISTA lost the other half of it, firing
    // singles at 9.6s where it should send three in five seconds and then be
    // empty.
    //
    // AND IT TOOK THE ANSWER WITH IT. A burst is a deal offered to the player --
    // live through the pass and the next ten seconds are free -- and a drizzle
    // offers nothing to read, survive or exploit.
    //
    // TACTIC_SLASH ALREADY ASSUMED THIS. Its comment reads "one fast pass, the
    // rack emptied, a long extend", and the pass and the extend were both built:
    // break_t carries a CHARIOT away after a firing pass that never emptied
    // anything. The manoeuvre was waiting for the weapon.
    s->fire_cd -= dt;

    // THE SAME REARM RULE THE PLAYER GETS. The contact is the player, tested in
    // this ship's own frame: inside RADAR_RANGE and in its forward half. An AEGIS
    // that has turned its back rearms no faster than the player's would.
    {
        const Vec3  rel  = vsub(v3(0, 0, 0), s->pos);
        const float fwd  = vdot(s->fwd, rel);
        const float side = vlen(rel);
        const float lat  = sqrtf(fmaxf(0.0f, side * side - fwd * fwd));
        vg_wpn_reload_step(*s, sp, dt, vg_wpn_on_radar(lat, fwd));
    }

    // WHETHER THIS SHOT OPENS A BURST, read before the trigger is reset.
    //
    // fire_cd keeps counting down past zero while the gates hold the shot, so a
    // ship that has been waiting for its nose to come on target is deeply
    // negative and one that is mid-burst is barely under. More than one gap of
    // slack means the trigger was released and pulled again.
    const bool fresh_burst = (s->fire_cd < -sp->fire_gap);

    Vec3  to      = vsub(v3(0, 0, 0), s->pos);
    float range   = vlen(to);
    const float fire_sn = vg_wpn_speed_norm(sp, s->speed);

    // EARNED BEFORE IT IS SPENT. Run every frame, including the frames the ship
    // cannot shoot on -- a lock is built by holding the nose on somebody, and
    // pausing the clock whenever the trigger happened to be cold would make it
    // mean something different from the player's.
    enemy_update_lock(s, sp, to, range, fire_sn, dt);

    // The discipline, and it is deliberately the LAST gate: a pilot who is
    // holding fire because a round of theirs is still out there has done
    // everything else right -- earned the lock, slowed down, closed the range --
    // and is choosing to wait. That is what the ships with a cap are for.
    //
    // A TRAINED PILOT ANSWERS THIS ITSELF. The cap is a rule standing in for a
    // judgement, and a policy fitted to somebody who held a lock on 69.5% of
    // frames and fired on 0.4% of them has learned the judgement. Where it has an
    // opinion the cap steps aside; where it has none -- an untrained class, or a
    // frame it declined -- the rule is still there.
    // WHETHER TO SHOOT IS THE POLICY'S. HOW MANY MAY BE IN THE AIR IS THE CLASS'S.
    //
    // These used to be one decision: a network with an opinion switched the cap
    // off entirely, on the argument that a policy cloned from someone with good
    // trigger discipline had learned the discipline. It had not. An AEGIS carries
    // six rounds at half a second apart, so with nothing holding it back the whole
    // rack goes up in three seconds -- which is what a flurry out of nowhere is,
    // and it is not the ship the class table describes.
    //
    // The cap is an attribute of the hull, like the magazine and the reload, and
    // an attribute does not stop applying because something clever is flying. The
    // policy still decides whether this frame is a shot; it no longer decides how
    // many of its own rounds may be in the air while it takes one.
    // ONE SET OF RULES, AND THE PLAYER'S IS IT.
    //
    // Four conditions used to live here that the player has never been subject
    // to: a cap on how many of its own rounds could be in the air, a maximum
    // range short of its own lock, a minimum range, and a ceiling on throttle.
    // None of them is in the class table. All of them were invisible from the
    // cockpit, which is the whole problem -- an opponent held to rules the player
    // cannot see is not a harder opponent, it is an inconsistent one, and every
    // future balance pass would have had to be done twice.
    //
    // What is left is exactly what a player has: rounds in the rack, a trigger
    // that has cooled, and a lock. Everything ELSE that should restrain a class
    // belongs in the class table, where it binds both seats and is visible in the
    // ship's own numbers.
    //
    // `judged` stays, and is not one of the four. It is the policy declining to
    // shoot -- a pilot's finger, not a rule -- and the player's finger is under
    // exactly the same lack of obligation.
    const int judged = vg_bot_enemy_fire(index);

    // A stacking class with nothing banked has nothing to fire. The player's
    // trigger asks the same function.
    const bool banked = vg_wpn_has_shot(*s, sp);

    if (s->rounds > 0 && s->fire_cd <= 0 && s->locked && banked && (judged != 0)) {
        // ALONG THE AIM. The rail is still on the nose -- that is where the ship
        // is -- but the round leaves on the direction this pilot thinks is the
        // target, which is the same error that made the lock slow to earn.
        // HOW MANY LEAVE ON THIS TRIGGER. One, unless the class banks -- and then
        // everything banked, which is the player's rule and therefore this one.
        const int salvo = vg_wpn_salvo(*s, sp);
        for (int k = 0; k < salvo; k++) {
            // ALONG THE AIM. The rail is still on the nose -- that is where the
            // ship is -- but the round leaves on the direction this pilot thinks
            // is the target.
            if (!vg_launch_missile(false, vadd(s->pos, vmul(s->fwd, 4.0f)),
                                   s->aim_dir, -1, index, sp))
                break;
            s->rounds--;
        }
        // What the press cost. The player's trigger runs this same function, so
        // an enemy cannot keep a contact through a salvo the player would lose.
        vg_wpn_spend(*s, sp);

        // THE CLASS'S OWN TRIGGER SPEED, and ENEMY_FIRE_GAP_K is gone from it.
        // That constant existed to slow an average down to something survivable;
        // the burst is meant to be fast, and slowing it is what removed the class.
        // What makes it fair is that it RUNS OUT -- the handicap moved from the
        // trigger to the magazine, where the player's is.
        //
        // The jitter stays and is small. Four ships firing on the same cadence
        // read as one weapon, but a burst that wobbles is not a burst.
        s->fire_cd = sp->fire_gap * vg_frand(0.92f, 1.10f);

        if (s->rounds <= 0) {
            // DRY. The reload is the class's own, jittered so that four empty
            // racks do not come back together, and it is the whole of the
            // handicap now: a CHARIOT that spends twelve rounds in two seconds
            // is genuinely holding nothing for the next ten, exactly as the
            // player's would be.
            s->reload_t = sp->reload * vg_frand(0.95f, 1.15f);
        }

        // Called on the launch, not on the lock: the line and the missile leave
        // together, so the radio is a warning rather than a commentary.
        //
        // ON THE FIRST ROUND OF A BURST ONLY. Twelve launches is twelve calls, and
        // a CHARIOT emptying its rack would say the same line twelve times in two
        // seconds -- which is the announcement becoming the weapon's sound.
        //
        // A BURST, NOT A FULL RACK. Testing for a full magazine instead looks
        // equivalent and is not: a pass that spends half the rounds and breaks
        // away leaves the rack part full, so every later pass would be silent
        // until something emptied it. The warning belongs to the trigger being
        // pulled, which is the thing the player has to answer.
        if (fresh_burst) vg_comms_say(s, VOICE_FIRE);
    }
}
