#include "vg_sim.h"
#include "vg_arena.h"
#include "vg_bot.h"
#include "vg_modes.h"
#include "vg_wpnsys.h"
#include "vg_net.h"      // vg_net_available, for the hunt
#include "vg_prof.h"     // vg_bout_time, for the fight census
#include <math.h>

// Enemy fighter behaviour, in strict priority order:
//
//   1. wall avoidance   -- outranks even running from a missile, because flying
//                          into the boundary to dodge is just a slower death
//   2. missile evasion  -- break ACROSS the seeker, never away from it
//   3. defend           -- somebody is on the tail: outrun them if they are
//                          slow, force an overshoot if they are not
//   4. an empty rack    -- extend and reload rather than joust with nothing,
//                          and only while the player is near enough to matter
//   5. the class tactic -- where this hull wants to be, and the only step of
//                          the five that differs between the four
//
// PRESSING A WON POSITION cuts across step 5 rather than joining the list. It is
// not another thing to do, it is a reason not to stop doing the current one --
// see has_the_angle.
//
// Enemies live under the same physics the player does: bleeding speed buys turn
// rate, and flat out they cannot shoot.

// Closest player missile currently tracking this enemy.
// THE AGGRESSION TABLE, read. See AGGRESSION in cfg_combat.h. One column per
// class, in the class table's order; a hull's tactic names its column too.
static_assert(SHIP_CLASSES == 4, "the aggression table has one column per class");
static int agg_col(const ShipSpec* sp) {
    const int c = (int)(sp - &vg_ship_class[0]);
    return (c >= 0 && c < SHIP_CLASSES) ? c : 0;
}
static int agg_col(ShipTactic t) {
    switch (t) {
        case TACTIC_GEOMETRY: return 1;
        case TACTIC_SLASH:    return 2;
        case TACTIC_STANDOFF: return 3;
        case TACTIC_FIGHTER:
        default:              return 0;
    }
}
static float agg(AggKnob k, int c) {
    return AGG_CALM[k][c] + (AGG_HOT[k][c] - AGG_CALM[k][c]) * AGGRESSION;
}

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

// ARE WE ABOUT TO HIT THEM? See ENEMY_AVOID_LOOK.
//
// Straight lines for both ships, in the player's frame: the player is the origin
// flying down +z, so the closing motion is this ship's velocity less theirs.
// Neither is going to fly straight, and that is the point of asking a second
// ahead rather than a second too late -- a turn started now is cheap.
//
// The way out is toward the side it is already passing on: the miss vector, with
// the part along our own nose taken off so it is a turn and not a reversal. Dead
// on, there is no side, and any one will do.
static bool on_collision_course(const Ship* s, Vec3* away) {
    const Vec3  v  = vsub(vmul(s->fwd, s->speed), v3(0, 0, vg.speed));
    const float vv = vdot(v, v);
    if (vv < 1.0f) return false;
    const float t = -vdot(s->pos, v) / vv;       // seconds to the closest approach
    if (t <= 0.0f || t > ENEMY_AVOID_LOOK) return false;
    const Vec3  miss = vadd(s->pos, vmul(v, t));
    const float d    = vlen(miss);
    if (d > ENEMY_AVOID_MISS) return false;
    Vec3 side = (d > 1.0f) ? vmul(miss, 1.0f / d) : vcross(s->fwd, s->up);
    side = vsub(side, vmul(s->fwd, vdot(side, s->fwd)));
    if (vlen2(side) < 1e-4f) side = vcross(s->fwd, s->up);
    *away = vnorm(vadd(vnorm(side), vmul(s->fwd, 0.25f)));
    return true;
}

// ...and once it has decided to get out of the way, it stays out of the way for
// ENEMY_AVOID_HOLD. Re-aimed every frame the course is still bad, released when
// the hold runs down with the course clear.
static bool avoiding(Ship* s, float dt) {
    Vec3 dir;
    if (on_collision_course(s, &dir)) {
        s->avoid_dir = dir;
        s->avoid_t   = ENEMY_AVOID_HOLD;
    } else if (s->avoid_t <= 0.0f) {
        return false;
    }
    s->avoid_t -= dt;
    return true;
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
// Steps 1 and 2 above -- the wall and the missile on the tail --
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
    // UNLESS THE NETWORK IS TOLD IT OWNS THEM ALL. The table above was measured on
    // a network with no plan; the author's direction since is that every hull is
    // flown by the network inside its class's style, so this is the switch that
    // asks whether a network with a plan can fly the two classes it was kept out
    // of. Off unless a harness turns it on: see vg_net_all_phases.
    if (vg_net_all_phases) return true;
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
//
// EXCEPT FOR AN AR-AAM RACK, WHOSE CLOCK DOES NOT RUN WHILE IT IS LEAVING. That
// weapon rearms only with the contact in the forward half -- vg_wpn_reload_step,
// the player's rule as well -- and across-and-away puts the player behind it. So
// an empty AEGIS flew off to wait for a reload that had stopped: 43-47% of its
// frames were spent here, and more than half its bouts ran to the time limit.
//
// So that rack CROSSES instead of leaving: the contact is held about 72 degrees
// off the nose, nearly on the beam, which is a hard shot for the player and still
// in front. The side is kept for break_t, like any break; the angle is aimed off
// the current line of sight every frame, or the ship would drift onto its own six.
static void tactic_dry(Ship* s, Vec3 to, float smax, Vec3* desired,
                       bool keep_contact = false) {
    if (keep_contact) {
        if (s->break_t <= 0) {
            s->break_dir = break_across(vnorm(to), s->up, 0.0f);
            s->break_t   = vg_frand(0.8f, 1.4f);
        }
        const Vec3 los = vnorm(to);
        Vec3 side = vsub(s->break_dir, vmul(los, vdot(s->break_dir, los)));
        if (vlen2(side) < 1e-4f) side = vcross(los, s->up);
        side = vnorm(side);
        *desired = vadd(vmul(los, 0.31f), vmul(side, 0.95f));   // cos 72, sin 72
        s->target_speed = smax * ENEMY_DRY_SPEED_K;
        return;
    }
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
    // CLOSER WITH THE BREAK ON: see STANDOFF3_HOLD_K.
    const float hold = sp->lock_range * (vg_enemy_break ? agg(AGG_BAND, agg_col(sp)) : STANDOFF_HOLD_K);
    // NOT shortened against a slow target, unlike the merge breaks. Those exist
    // as caution about the other ship and a parked one is not worth being cautious
    // about. This one is the CLASS: a standoff hull wants its range whoever it is
    // facing, and handing it a reason to close because the target slowed down
    // would trade the identity away for a press it does not need. It shoots from
    // out there; that is the whole point of it.
    // ...and with the break on, it leaves only when it is crowded: STANDOFF3_FLEE_K.
    const float flee = ENEMY_BREAK_RANGE * (vg_enemy_break ? agg(AGG_FLEE, agg_col(sp)) : STANDOFF_FLEE_K);

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
// RE-ENGAGEMENT, counted for the training harness. A pilot who pulls out of a dead
// circle and turns back in earns a NEW lock; one who keeps circling earns none and
// sits without a lock for a long time. So: how many locks were taken after at least
// AI_REENGAGE_GAP without one -- a new pass, not a lock flickering at the edge of
// the cone, which measured 8 to 25 a minute -- and how many frames came after more
// than AI_DEAD_TIME without one. Counting only: nothing here changes a decision.
uint32_t g_ai_lockon[SHIP_CLASSES]  = { 0 };
uint32_t g_ai_dead[SHIP_CLASSES]    = { 0 };
#define AI_DEAD_TIME     4.0f
#define AI_REENGAGE_GAP  2.0f
static bool  s_census_locked[MAX_ENEMIES] = { false };
static float s_census_nolock[MAX_ENEMIES] = { 0.0f };
float    g_ai_aim[SHIP_CLASSES]     = { 0 };   // summed cos(aim, target)
float    g_ai_range[SHIP_CLASSES]   = { 0 };   // summed range, world units
// ...and the same aim, split by what the pilot was DOING at the time. A mean over
// the whole fight is dominated by the frames where not aiming is correct: a ship
// with an empty rack should be leaving, not pointing.
float    g_ai_aimk[(int)SHIP_CLASSES * STEER_KINDS] = { 0 };

// THE FIGHT CENSUS: the author's verdict on the 10-25 playtests, as numbers. The
// opponents circle, or leave and never come back; a player who sits at zero
// throttle turning hard is never punished; and pressure is "the need for me to
// personally disengage knowing that i might get hit if i keep engaging". Counting
// only, like everything above: nothing here changes a decision. Per class of the
// opponent, over its alive frames. Checked against the author's own verdict on the
// four playtested opponents -- see captures/ai_eval/PLAN.md, 2026-10-25.
//
// CIRCLING: both ships pulling hard -- each at CEN_PULL or more of its OWN maximum
// turn at its current speed, smoothed over CEN_TAU -- with the range holding (|rdot|
// under CEN_RDOT_K of the opponent's top speed), inside its lock range, and NEITHER
// side holding a lock. Counted only inside a run that has held for CEN_CIRCLE_T, and
// then from the run's first frame, so a merge that crosses the test for a moment is
// not a circle. CEN_PULL is where the author's own smoothed pull sits on 55% of
// their AEGIS frames; 0.5 missed most of a soft seat's turning.
// A PASS is the thing the author asked for: the range opens past CEN_EXTEND of the
// lock range (an extend), comes back under CEN_RETURN of it with the opponent's aim
// inside its lock cone (a return), and the opponent fires within CEN_PASS_T of the
// return. Counted apart, so "leaves and never comes back" is extends without returns.
// TAIL: the opponent's nose on the seat from the seat's rear half, and the rounds it
// fires from there. The author calls that being attacked ("it does know to attack you
// if it's on your tail"); the one opponent that never got there ("doesn't capitalize
// ... when it's behind me") was the one with no pressure, though it fired as often as
// any. So the TAIL is the pressure, and THREAT below is not: it ranks that opponent
// highest, because its missiles track the seat for a while and miss.
// THREAT: the opponent holds a lock on the seat, or a missile it fired is still
// tracking the seat -- the seat's own OBS_MSL_IN test, given to the shooter.
// NOSE: the seat inside the opponent's lock cone and lock range, from anywhere.
// ATTACK: the seat's nose on the opponent inside the SEAT's lock cone and range.
// SEAT: how hard the seat pulls and how much throttle it carries, and SPIN, the
// author's "remain at 0 throttle just to spin around": under CEN_SPIN_THR of
// throttle while pulling CEN_SPIN_PULL or more.
uint32_t g_cen_circle[SHIP_CLASSES]     = { 0 };
uint32_t g_cen_extend[SHIP_CLASSES]     = { 0 };
uint32_t g_cen_return[SHIP_CLASSES]     = { 0 };
uint32_t g_cen_pass[SHIP_CLASSES]       = { 0 };
uint32_t g_cen_shots[SHIP_CLASSES]      = { 0 };
uint32_t g_cen_tail[SHIP_CLASSES]       = { 0 };
uint32_t g_cen_tail_shots[SHIP_CLASSES] = { 0 };
uint32_t g_cen_threat[SHIP_CLASSES]     = { 0 };
uint32_t g_cen_inbound[SHIP_CLASSES]    = { 0 };
uint32_t g_cen_nose[SHIP_CLASSES]       = { 0 };
uint32_t g_cen_attack[SHIP_CLASSES]     = { 0 };
uint32_t g_cen_spin[SHIP_CLASSES]       = { 0 };
float    g_cen_pull_sum[SHIP_CLASSES]      = { 0 };   // the opponent's own, smoothed
float    g_cen_seat_pull_sum[SHIP_CLASSES] = { 0 };
float    g_cen_seat_thr_sum[SHIP_CLASSES]  = { 0 };
#define CEN_PULL      0.4f
#define CEN_RDOT_K    0.25f    // of the opponent's speed_max, a second
#define CEN_CIRCLE_T  2.0f
#define CEN_TAU       0.5f     // smoothing of both pulls and the range rate, seconds
#define CEN_EXTEND    0.8f
#define CEN_RETURN    0.6f
#define CEN_PASS_T    8.0f
#define CEN_SPIN_THR  0.15f
#define CEN_SPIN_PULL 0.5f
static float    s_cen_pull[MAX_ENEMIES];   // this pilot's last turn, from the turn step
static float    s_cen_pe[MAX_ENEMIES], s_cen_ps[MAX_ENEMIES], s_cen_rdot[MAX_ENEMIES];
static float    s_cen_r[MAX_ENEMIES], s_cen_run[MAX_ENEMIES], s_cen_ret_t[MAX_ENEMIES];
static float    s_cen_bout[MAX_ENEMIES];   // the bout clock last frame: a drop is a new bout
static uint32_t s_cen_runf[MAX_ENEMIES];
static int      s_cen_state[MAX_ENEMIES], s_cen_rounds[MAX_ENEMIES];
// THE BREAK, counted (Gameplay's break-and-attack, in vg_ai.cpp below). Agreed with
// Gameplay so both mark the same frames: a TURN-IN is the frame brk_phase goes from
// BRK_EXTEND to BRK_RUN; a BREAK SHOT is a round fired on the run; a BREAK HIT is a
// hit on the seat by a round LAUNCHED on the run -- what the author singled out: "the
// break away attempt did landed good hits when the opponent suddenly turned around".
// THE EXPOSED SHOT (the break's second kind): rounds fired while expo_t > 0, the
// opponent taking a shot at a seat that has left itself open. Counted apart, and
// never as a break shot. Hits are matched in vg_census_round_end to the shooter's
// recent run and exposed windows by the round's age.
// ILLUMINATION (a semi-active hull lighting its own round): frames it holds the lock
// for its round (ILLUM_HOLD), the times it let go for a round judged likely to hit
// it (ILLUM_LET_GO), and hits on the seat while it was still holding.
uint32_t g_cen_turnin[SHIP_CLASSES]      = { 0 };
uint32_t g_cen_break_shots[SHIP_CLASSES] = { 0 };
uint32_t g_cen_break_hits[SHIP_CLASSES]  = { 0 };
uint32_t g_cen_expo_shots[SHIP_CLASSES]  = { 0 };
uint32_t g_cen_expo_hits[SHIP_CLASSES]   = { 0 };
uint32_t g_cen_illum_hold[SHIP_CLASSES]  = { 0 };
uint32_t g_cen_illum_letgo[SHIP_CLASSES] = { 0 };
uint32_t g_cen_illum_hits[SHIP_CLASSES]  = { 0 };
#define CEN_RUNS       4        // windows kept per opponent, for rounds still flying
static uint8_t s_cen_brk[MAX_ENEMIES];
static bool    s_cen_expo[MAX_ENEMIES];
static int     s_cen_runk[MAX_ENEMIES], s_cen_expk[MAX_ENEMIES];
static float   s_cen_run_a[MAX_ENEMIES][CEN_RUNS], s_cen_run_b[MAX_ENEMIES][CEN_RUNS];
static float   s_cen_exp_a[MAX_ENEMIES][CEN_RUNS], s_cen_exp_b[MAX_ENEMIES][CEN_RUNS];
// AGGRESSION (playtest 3: "aggression needs to be met at least 10 fold"), the same
// definitions as Gameplay's:
// A SOLUTION SHOT is a round fired with all four of:
//   LOCK on the seat (this frame or the last: the census runs before the shooter
//     fires);
//   REACH: range <= reach - away * life, away being the seat's speed directly away
//     from the shooter (floored at 0);
//   FASTER: the seat slower than the round's launch speed;
//   NOT CROSSING: the seat's speed across the line of sight under half the round's
//     launch speed.
// Gameplay's term, from 6,000+ rounds against both author-like seats: the misses
// that end in the wall are a fast seat crossing the line of sight (AUC 0.91 for its
// speed, 0.85 for crossing; 0.59 for range). An AEGIS round leaves slower than a
// seat at full throttle and cannot hold it in its cone. Solution shots hit 54-93%
// of the time, the others 5-54%. The launch speed even for the rounds that speed
// up: the conservative form. Its hits are told apart by launch time (SOLUTION HITS).
// Independent of anything the AI uses to decide when to fire: this measures.
// ROUND ENDS, per shooter class, from detonate() (vg_census_round_end): ended, by the
// wall, and hits on the seat.
// FORCED EVADE: frames where one of its locked rounds would reach the seat within
// CEN_EVADE_T at the present closing speed -- the seat has to do something now. A
// little longer than the AI's own evade trigger (ENEMY_EVADE_RANGE at missile speed,
// ~1.6 s): a person answers slower.
uint32_t g_cen_sol_shots[SHIP_CLASSES] = { 0 };
uint32_t g_cen_ended[SHIP_CLASSES]     = { 0 };
uint32_t g_cen_wall_end[SHIP_CLASSES]  = { 0 };
uint32_t g_cen_seat_hits[SHIP_CLASSES] = { 0 };
uint32_t g_cen_evade[SHIP_CLASSES]     = { 0 };
uint32_t g_cen_sol_hits[SHIP_CLASSES]  = { 0 };
uint32_t g_cen_rams_net[SHIP_CLASSES]  = { 0 };   // collisions counted against the network
uint32_t g_cen_rams_seat[SHIP_CLASSES] = { 0 };   // ...and the ones the seat drove into
#define CEN_RAM_COS 0.866f      // cos 30 degrees: heading at the seat
// PRESSURE, the two research measures (2026-10-28), to be checked against the
// author's verdicts before anything is scored with them:
// MUTUAL THREAT: frames where the seat has the opponent in its own lock cone and
//   range (ATTACK) AND the opponent has the seat in its lock cone and range (NOSE)
//   or one of its locked rounds is tracking the seat. Engaging costs the seat
//   something. MUTUAL WIDE: the same with the seat's cone widened to 60 degrees.
// FORCED REACTION: a launch (the rack going down) after which the seat's heading
//   turns more than 90 degrees within CEN_FREACT_T. Any direction: breaking into it
//   is a reaction too. Counted once a launch; LAUNCHES counts the launches.
uint32_t g_cen_mutual[SHIP_CLASSES]   = { 0 };
uint32_t g_cen_mutual_w[SHIP_CLASSES] = { 0 };
uint32_t g_cen_freact[SHIP_CLASSES]   = { 0 };
uint32_t g_cen_launch[SHIP_CLASSES]   = { 0 };
#define CEN_WIDE_COS  0.5f      // cos 60 degrees
#define CEN_FREACT_T  1.0f
static float s_cen_fr_t[MAX_ENEMIES];      // seconds since its last launch, < 0: none
static Vec3  s_cen_fr_dir[MAX_ENEMIES];    // the seat's heading then, arena frame
static bool  s_cen_fr_done[MAX_ENEMIES];
// THE OPPONENT'S OWN VIEW, for comparing how it shoots with how the author shoots
// (host --dump-enemy): opponent 0's observation each frame it has a target, in the
// recordings' layout, with an "action" of its pull, 0, its throttle and a launch flag.
// Host only; null on the board.
void (*vg_census_enemy_tap)(const VgObs* o, const float* act) = nullptr;
// The seat's heading in the arena frame (the sim is centred on the seat).
static Vec3 cen_seat_heading(void) {
    return vsub(vg_arena_local_of(v3(0, 0, 1)), vg_arena_local_of(v3(0, 0, 0)));
}
#define CEN_EVADE_T  2.0f
#define CEN_SOLS     8          // solution launches kept per opponent, for rounds in flight
#define CEN_SOL_EDGE 0.05f      // seconds: a hit's launch time against a kept launch
static bool  s_cen_lock_was[MAX_ENEMIES];
static int   s_cen_solk[MAX_ENEMIES];
static float s_cen_sol_t[MAX_ENEMIES][CEN_SOLS];
// The distance a round covers in its life: vg_msl_reach in vg_ship.cpp, which is
// file-local there.
static float cen_reach(const ShipSpec* sp) {
    if (sp->msl_accel <= 0.0f) return sp->msl_speed * sp->msl_life;
    const float ramp = (sp->msl_speed_max - sp->msl_speed) / sp->msl_accel;
    if (ramp >= sp->msl_life)
        return sp->msl_speed * sp->msl_life + 0.5f * sp->msl_accel * sp->msl_life * sp->msl_life;
    return sp->msl_speed * ramp + 0.5f * sp->msl_accel * ramp * ramp
         + (sp->msl_life - ramp) * sp->msl_speed_max;
}

static void census_fight(const Ship* s, const ShipSpec* sp, int c, int i, float dt) {
    const float r  = vlen(s->pos);
    const float bt = vg_bout_time();
    // A NEW BOUT, or the first frame of this slot: start from what is there now, so
    // the jump to a fresh spawn is not read as a range rate.
    if (bt < s_cen_bout[i] || s_cen_r[i] <= 0.0f) {
        s_cen_r[i]      = r;
        s_cen_rdot[i]   = 0.0f;
        s_cen_pe[i]     = s_cen_pull[i];
        s_cen_ps[i]     = vg.seat_pull;
        s_cen_run[i]    = 0.0f;
        s_cen_runf[i]   = 0;
        s_cen_state[i]  = 0;
        s_cen_rounds[i] = s->rounds;
        s_cen_lock_was[i] = s->locked;
        s_cen_brk[i]  = s->brk_phase;
        s_cen_expo[i] = s->expo_t > 0.0f;
        for (int k = 0; k < CEN_RUNS; k++) {
            s_cen_run_a[i][k] = s_cen_run_b[i][k] = -1.0f;
            s_cen_exp_a[i][k] = s_cen_exp_b[i][k] = -1.0f;
        }
        for (int k = 0; k < CEN_SOLS; k++) s_cen_sol_t[i][k] = -1.0f;
        s_cen_fr_t[i] = -1.0f;
    }
    s_cen_bout[i] = bt;
    const float k = (dt > 0.0f) ? fminf(1.0f, dt / CEN_TAU) : 0.0f;
    if (dt > 0.0f) s_cen_rdot[i] += ((r - s_cen_r[i]) / dt - s_cen_rdot[i]) * k;
    s_cen_r[i] = r;
    s_cen_pe[i] += (s_cen_pull[i] - s_cen_pe[i]) * k;
    s_cen_ps[i] += (vg.seat_pull  - s_cen_ps[i]) * k;

    g_cen_pull_sum[c]      += s_cen_pe[i];
    g_cen_seat_pull_sum[c] += vg.seat_pull;
    g_cen_seat_thr_sum[c]  += vg.throttle;
    if (vg.throttle < CEN_SPIN_THR && vg.seat_pull >= CEN_SPIN_PULL) g_cen_spin[c]++;

    // A SHOT is the rack going down: one round a missile, whatever fired it.
    const int fired = (s->rounds < s_cen_rounds[i]) ? s_cen_rounds[i] - s->rounds : 0;
    g_cen_shots[c] += (uint32_t)fired;
    s_cen_rounds[i] = s->rounds;
    if (fired && r > 1.0f && (s->locked || s_cen_lock_was[i])) {
        // The seat flies +z; the line of sight runs from the shooter to the seat (-pos).
        // CROSSING is |v_seat - away * los| with away floored at 0, as Gameplay
        // measured it: a seat coming at the shooter counts its whole speed, because a
        // round meeting it head-on has as little time to turn as one it is crossing.
        const float away  = fmaxf(0.0f, vg.speed * (-s->pos.z / r));
        const float cross = sqrtf(fmaxf(0.0f, vg.speed * vg.speed - away * away));
        if (r <= cen_reach(sp) - away * sp->msl_life
            && vg.speed < sp->msl_speed && cross < 0.5f * sp->msl_speed) {
            g_cen_sol_shots[c] += (uint32_t)fired;
            s_cen_solk[i] = (s_cen_solk[i] + 1) % CEN_SOLS;
            s_cen_sol_t[i][s_cen_solk[i]] = bt;
        }
    }
    s_cen_lock_was[i] = s->locked;

    bool nose = false;
    if (r > 1.0f && r <= sp->lock_range)
        nose = vdot(s->aim_dir, vmul(s->pos, -1.0f / r)) > vg_lock_cos_at(sp, r, false);
    if (nose) g_cen_nose[c]++;
    // The seat's nose is +z, so its rear half is z < 0.
    if (nose && s->pos.z < 0.0f) {
        g_cen_tail[c]++;
        g_cen_tail_shots[c] += (uint32_t)fired;
    }
    const bool att = r > 1.0f && vg.spec && r <= vg.spec->lock_range
                  && s->pos.z / r > vg_lock_cos_at(vg.spec, r, false);
    if (att) g_cen_attack[c]++;

    bool inbound = false, evade = false;
    for (int m = 0; m < MAX_MISSILES && !evade; m++) {
        const Missile* ms = &vg.msl[m];
        if (!ms->alive || ms->from_player || !ms->locked || ms->shooter != i) continue;
        inbound = true;
        // The seat is at the origin and flies +z at vg.speed.
        const float d = vlen(ms->pos);
        if (d > 1.0f) {
            const Vec3  rel     = vsub(vmul(ms->dir, ms->speed), v3(0, 0, vg.speed));
            const float closing = -vdot(rel, vmul(ms->pos, 1.0f / d));
            if (closing > 0.0f && d <= closing * CEN_EVADE_T) evade = true;
        }
    }
    if (inbound) g_cen_inbound[c]++;
    if (evade) g_cen_evade[c]++;
    if (inbound || s->locked) g_cen_threat[c]++;

    if (nose || inbound) {
        if (att) g_cen_mutual[c]++;
        if (r > 1.0f && vg.spec && r <= vg.spec->lock_range && s->pos.z / r > CEN_WIDE_COS)
            g_cen_mutual_w[c]++;
    }
    if (fired) {
        g_cen_launch[c]++;
        s_cen_fr_t[i]    = 0.0f;
        s_cen_fr_dir[i]  = cen_seat_heading();
        s_cen_fr_done[i] = false;
    } else if (s_cen_fr_t[i] >= 0.0f) {
        s_cen_fr_t[i] += dt;
        if (!s_cen_fr_done[i] && vdot(cen_seat_heading(), s_cen_fr_dir[i]) < 0.0f) {
            g_cen_freact[c]++;
            s_cen_fr_done[i] = true;
        }
        if (s_cen_fr_t[i] > CEN_FREACT_T) s_cen_fr_t[i] = -1.0f;
    }
    if (vg_census_enemy_tap && i == 0) {
        VgObs o;
        vg_bot_observe_enemy(i, &o);
        if (o.has_target) {
            const float act[4] = { s_cen_pull[i], 0.0f, o.v[OBS_OWN_THROTTLE],
                                   fired ? 1.0f : 0.0f };
            vg_census_enemy_tap(&o, act);
        }
    }

    const bool circ = s_cen_ps[i] >= CEN_PULL && s_cen_pe[i] >= CEN_PULL
                   && fabsf(s_cen_rdot[i]) <= CEN_RDOT_K * sp->speed_max
                   && r <= sp->lock_range && !vg_wpn.locked && !s->locked;
    if (circ) {
        s_cen_run[i] += dt;
        s_cen_runf[i]++;
        if (s_cen_run[i] >= CEN_CIRCLE_T) {
            g_cen_circle[c] += s_cen_runf[i];
            s_cen_runf[i] = 0;
        }
    } else {
        s_cen_run[i]  = 0.0f;
        s_cen_runf[i] = 0;
    }

    // THE BREAK: open a window on entering the run (a turn-in when it came from the
    // extend) or an exposed shot, close it on leaving, and count the rounds fired.
    // A ROUND COUNTED HERE WAS FIRED LAST FRAME, after this census had run, so the
    // state it was fired in is last frame's or this one's: either counts.
    const uint8_t ph = s->brk_phase, ph_was = s_cen_brk[i];
    const bool    ex = s->expo_t > 0.0f, ex_was = s_cen_expo[i];
    if (ph == BRK_RUN && ph_was != BRK_RUN) {
        if (ph_was == BRK_EXTEND) g_cen_turnin[c]++;
        s_cen_runk[i] = (s_cen_runk[i] + 1) % CEN_RUNS;
        s_cen_run_a[i][s_cen_runk[i]] = bt;
        s_cen_run_b[i][s_cen_runk[i]] = 1e30f;
    } else if (ph != BRK_RUN && ph_was == BRK_RUN) {
        s_cen_run_b[i][s_cen_runk[i]] = bt;
    }
    if (ex && !ex_was) {
        s_cen_expk[i] = (s_cen_expk[i] + 1) % CEN_RUNS;
        s_cen_exp_a[i][s_cen_expk[i]] = bt;
        s_cen_exp_b[i][s_cen_expk[i]] = 1e30f;
    } else if (!ex && ex_was) {
        s_cen_exp_b[i][s_cen_expk[i]] = bt;
    }
    s_cen_brk[i]  = ph;
    s_cen_expo[i] = ex;
    if (ex || ex_was)
        g_cen_expo_shots[c]  += (uint32_t)fired;
    else if (ph == BRK_RUN || ph_was == BRK_RUN)
        g_cen_break_shots[c] += (uint32_t)fired;
    if (s->illum == ILLUM_HOLD)        g_cen_illum_hold[c]++;
    else if (s->illum == ILLUM_LET_GO) g_cen_illum_letgo[c]++;

    switch (s_cen_state[i]) {
        case 0:
            if (r > CEN_EXTEND * sp->lock_range) { s_cen_state[i] = 1; g_cen_extend[c]++; }
            break;
        case 1:
            if (r < CEN_RETURN * sp->lock_range && nose) {
                s_cen_state[i] = 2;
                s_cen_ret_t[i] = 0.0f;
                g_cen_return[c]++;
            }
            break;
        default:
            s_cen_ret_t[i] += dt;
            if (fired) { g_cen_pass[c]++; s_cen_state[i] = 0; }
            else if (s_cen_ret_t[i] > CEN_PASS_T) s_cen_state[i] = 0;
            break;
    }
}


// ---------------------------------------------------------------------------
// OUT OF A CIRCLE, AWAY, AND BACK IN. See ENEMY_CIRCLE_TIME.
// ---------------------------------------------------------------------------
// The extend, the run's speed, the run's end after its shot, the extend's two
// ceilings, the circle hold and the cooldown. See BRK3_EXTEND_FIGHTER.
struct BrkClass { float extend_k, speed_k, after, ext_max, ext_hard, hold, cd; };

// THE PER-HULL EXCEPTIONS, apart from the table above so they can go on their own.
struct BrkHull { bool evade_early; float winning; };

static BrkHull brk_hull(ShipTactic t) {
    switch (t) {
        case TACTIC_GEOMETRY: return { BRK_EVADE_EARLY_GEOMETRY != 0, 0.0f };
        case TACTIC_SLASH:    return { false, BRK_WINNING_SLASH };
        default:              return { false, 0.0f };
    }
}

static BrkClass brk_class(ShipTactic t) {
    switch (t) {
        // THE RESET'S KNOBS ARE THE AGGRESSION TABLE'S: see AGGRESSION.
        case TACTIC_GEOMETRY: return { agg(AGG_EXTEND, 1), BRK_SPEED_GEOMETRY, BRK_AFTER_GEOMETRY,
                                       agg(AGG_EXT_MAX, 1), agg(AGG_EXT_HARD, 1),
                                       agg(AGG_HOLD, 1), agg(AGG_BREAK_CD, 1) };
        case TACTIC_SLASH:    return { agg(AGG_EXTEND, 2), BRK_SPEED_SLASH, BRK_AFTER_SLASH,
                                       agg(AGG_EXT_MAX, 2), agg(AGG_EXT_HARD, 2),
                                       agg(AGG_HOLD, 2), agg(AGG_BREAK_CD, 2) };
        case TACTIC_STANDOFF: return { agg(AGG_EXTEND, 3), BRK_SPEED_STANDOFF, BRK_AFTER_STANDOFF,
                                       agg(AGG_EXT_MAX, 3), agg(AGG_EXT_HARD, 3),
                                       agg(AGG_HOLD, 3), agg(AGG_BREAK_CD, 3) };
        case TACTIC_FIGHTER:
        default:              return { agg(AGG_EXTEND, 0), BRK_SPEED_FIGHTER, BRK_AFTER_FIGHTER,
                                       agg(AGG_EXT_MAX, 0), agg(AGG_EXT_HARD, 0),
                                       agg(AGG_HOLD, 0), agg(AGG_BREAK_CD, 0) };
    }
}

// WATCHED EVERY FRAME, whoever is flying -- the tactics or the network -- because
// the circle is a fact about the two ships, not about the layer that steered.
// The same test the census counts: both pulling hard, the range not moving, inside
// this ship's lock range, and nobody locked. circle_t is the unbroken run of it.
static void circle_watch(Ship* s, const ShipSpec* sp, float dt) {
    if (dt <= 0.0f) return;
    const float r = vlen(s->pos);
    if (s->prev_range <= 0.0f) {
        // A NEW BOUT starts from what is true now, not from zero.
        s->pull_s      = s->turn_frac;
        s->seat_pull_s = vg.seat_pull;
        s->rdot_s      = 0.0f;
    } else {
        float k = dt / ENEMY_CIRCLE_SMOOTH;
        if (k > 1.0f) k = 1.0f;
        s->rdot_s      += ((r - s->prev_range) / dt - s->rdot_s) * k;
        s->pull_s      += (s->turn_frac - s->pull_s) * k;
        s->seat_pull_s += (vg.seat_pull - s->seat_pull_s) * k;
    }
    s->prev_range = r;
    const bool circling = s->seat_pull_s >= ENEMY_CIRCLE_PULL
                       && s->pull_s >= ENEMY_CIRCLE_PULL
                       && fabsf(s->rdot_s) <= ENEMY_CIRCLE_RDOT_K * sp->speed_max
                       && r <= sp->lock_range
                       && !vg_wpn.locked && !s->locked;
    s->circle_t = circling ? s->circle_t + dt : 0.0f;
    if (s->brk_cd > 0.0f) s->brk_cd -= dt;
    if (s->brk_phase != BRK_NONE) s->brk_t += dt;
    if (s->brk_shot_t >= 0.0f) s->brk_shot_t += dt;
    // HOW LONG SINCE THE PLAYER WAS LAST HIT, for a hull that does not break a circle
    // it is winning. Any hit counts: there is one opponent in a duel, and two ships
    // landing hits between them is no less a fight going somewhere.
    if (s->seen_health >= 0.0f && vg.health < s->seen_health - 1e-4f) s->seat_hit_t = 0.0f;
    else                                                                 s->seat_hit_t += dt;
    s->seen_health = vg.health;
}

// AWAY, AND ALONG THE TUBE. Straight away from the player is usually straight at
// a wall: the tube is 1900 in radius and the extend wants a lock range or more.
// Along the loop there is always room, so the extend follows it, on whichever side
// of the loop points away from the player -- chosen once, so the ship does not
// dither between the two -- with a share of plain "away" so the range opens from
// the first second. The wall still outranks it.
static Vec3 extend_dir(Ship* s, Vec3 los) {
    const Vec3 away = vmul(los, -1.0f);
    if (vg_arena.kind != ARENA_TORUS) return away;
    const Vec3  l = vg_arena_local_of(s->pos);
    const float h = sqrtf(l.x * l.x + l.z * l.z);
    if (h < 1.0f) return away;
    Vec3 t = vg_arena_dir_to_view(v3(-l.z / h, 0.0f, l.x / h));
    if (s->brk_sign == 0.0f) s->brk_sign = (vdot(t, away) >= 0.0f) ? 1.0f : -1.0f;
    t = vmul(t, s->brk_sign);
    return vnorm(vadd(vmul(t, 0.6f), vmul(away, 0.8f)));
}

bool vg_enemy_break = true;

// THE BREAK. Returns true while it is steering.
//
// A ROUND COMING IN IS ANSWERED BY RUNNING, then by the shot, then by the break.
// The extend is flat out, which outruns it; the run takes its shot first, and only
// a round inside ENEMY_BRK_LAST_DITCH makes it break before then; after the shot
// it answers at the ordinary range. On those frames this declines, the layers
// below answer the missile, and the break picks up where it was.
//
// THE RUN leads the player by up to ENEMY_BRK_LEAD_MAX of their motion, but never
// by more than half its own lock cone, so the lead does not cost it the lock it
// came for. It closes fast, and inside its lock range it slows to its class's
// speed, because the lock is earned slowly at full throttle -- LOCK_SPEED_PENALTY.
// A STANDOFF hull does not lead: an eight-degree cone and a round it keeps lighting
// all the way in want the nose on the player, not ahead of them.
//
// IT ENDS on the class's own terms after the first shot, at the break range --
// the merge still belongs to the tactic and the floors above -- or on its ceiling.
// An AR-AAM rack refills while the player is in front, so an AEGIS that arrives
// empty keeps its nose on them and shoots when the round comes back.
static bool breaking(Ship* s, const ShipSpec* sp, bool reacted, float inc_range,
                     float inc_speed, float smin, float smax, Vec3* desired) {
    if (!vg_enemy_break) return false;
    const Vec3  to = vsub(v3(0, 0, 0), s->pos);
    const float r  = vlen(to);
    if (r < 1.0f) return false;
    const Vec3 los = vmul(to, 1.0f / r);
    const BrkClass bc = brk_class(sp->tactic);
    const BrkHull  bh = brk_hull(sp->tactic);

    if (s->brk_phase == BRK_NONE) {
        if (s->circle_t < bc.hold || s->brk_cd > 0.0f) return false;
        // NOT THE SNIPER. A BALLISTA is the slowest thing in the game, so an extend
        // is nine seconds with its back to every round in the air -- measured, it
        // took more hits on the way out than in all the rest of the fight. Its own
        // standoff already holds the range and leaves when it is crowded.
        if (sp->tactic == TACTIC_STANDOFF && !BRK_STANDOFF_BREAK) return false;
        // PER HULL: not out of a circle that is landing hits. See BRK_WINNING_SLASH.
        if (bh.winning > 0.0f && s->seat_hit_t < bh.winning) return false;
        // AN AR-AAM RACK ONLY REFILLS WITH THE PLAYER IN FRONT, and an extend puts
        // them behind. Leaving with nothing in the rack comes back with nothing, so
        // an AEGIS goes when it has a round to come back with.
        if (sp->wpn == WPN_ARAAM && s->rounds <= 0 && !BRK_DRY_BREAK) return false;
        s->brk_phase = BRK_EXTEND;
        s->brk_t     = 0.0f;
        s->brk_sign  = 0.0f;
        s->circle_t  = 0.0f;
    }
    // RUN FROM WHAT YOU CAN OUTRUN, AND BREAK ACROSS WHAT YOU CANNOT. Flat out, a
    // ship faster than the round chasing it has already won; a BALLISTA is not, and
    // running away from a faster round is the one answer that always loses.
    const bool shot_taken = s->brk_phase == BRK_RUN && s->brk_shot_t >= 0.0f;
    const bool outrun     = inc_speed < smax;
    const float answer_at = (shot_taken || (s->brk_phase == BRK_EXTEND && !outrun)
                             || (s->brk_phase == BRK_RUN && bh.evade_early))
                          ? ENEMY_EVADE_RANGE : ENEMY_BRK_LAST_DITCH;
    if (reacted && inc_range < answer_at) return false;

    // THE SNIPER'S BREAK: its own standoff, then back to the fight. No run.
    if (sp->tactic == TACTIC_STANDOFF) {
        if (s->brk_t < agg(AGG_STANDOFF_T, agg_col(sp))) {
            tactic_standoff(s, sp, to, r, smin, smax, desired);
            s->steer_by = STEER_TACTIC;
            s->stale_t  = 0.0f;
            return true;
        }
        s->brk_phase = BRK_NONE;
        s->brk_cd    = bc.cd;
        s->circle_t  = 0.0f;
        return false;
    }
    if (s->brk_phase == BRK_EXTEND) {
        const float want    = sp->lock_range * bc.extend_k;
        // NOT INTO A LOCK. Turning back in while the player has this ship in their
        // sights hands them the first shot -- the player is slower, so they lock
        // sooner -- and the trace of every such run is the same: their round, then
        // ours, then ours missing. So it keeps running until they lose it, up to
        // the same harder ceiling.
        const bool  chased  = (inc_range < MSL_ALERT_RANGE && outrun) || vg_wpn.locked;
        const bool  opened  = r >= want || s->brk_t >= bc.ext_max;
        if (!(opened && !chased) && s->brk_t < bc.ext_hard) {
            *desired        = extend_dir(s, los);
            s->target_speed = smax;
            s->steer_by     = STEER_RESET;
            s->stale_t      = 0.0f;
            return true;
        }
        s->brk_phase  = BRK_RUN;
        s->brk_t      = 0.0f;
        s->brk_rounds = s->rounds;
        s->brk_shot_t = -1.0f;
    }

    // THE RUN.
    if (s->rounds < s->brk_rounds && s->brk_shot_t < 0.0f) s->brk_shot_t = 0.0f;
    s->brk_rounds = s->rounds;
    float brk_k = 1.0f;
    if (sp->tactic == TACTIC_GEOMETRY) brk_k = GEOM_BREAK_K;
    if (sp->tactic == TACTIC_SLASH)    brk_k = SLASH_BREAK_K;
    if (sp->tactic == TACTIC_STANDOFF) brk_k = STANDOFF_FLEE_K;
    const bool shot_done = s->brk_shot_t >= 0.0f &&
        (s->brk_shot_t > bc.after || (s->rounds <= 0 && sp->tactic == TACTIC_SLASH));
    // A LOCK IN HAND IS WORTH THE LAST FEW HUNDRED UNITS. Holding one and not yet
    // shot, the run goes on to half the break range; both collision floors above
    // still turn it away from contact.
    const float end_r = ENEMY_BREAK_RANGE * brk_k
                      * ((s->locked && s->brk_shot_t < 0.0f && s->rounds > 0) ? 0.5f : 1.0f);
    if (r < end_r || s->brk_t > ENEMY_BRK_RUN_MAX || shot_done) {
        s->brk_phase  = BRK_NONE;
        s->brk_cd     = bc.cd;
        s->brk_shot_t = -1.0f;
        s->circle_t   = 0.0f;
        return false;
    }

    Vec3 aim = to;
    if (sp->tactic != TACTIC_STANDOFF) {
        float t_go = r / (s->speed > 100.0f ? s->speed : 100.0f);
        if (t_go > ENEMY_BRK_LEAD_MAX) t_go = ENEMY_BRK_LEAD_MAX;
        aim = vadd(to, v3(0.0f, 0.0f, vg.speed * t_go));
        // Never more than half the cone off the player.
        const float half = cosf(0.5f * acosf(vg_lock_cos_at(sp, r, false)));
        const Vec3  a    = vnorm(aim);
        if (vdot(a, los) < half) {
            const Vec3 side = vnorm(vsub(a, vmul(los, vdot(a, los))));
            aim = vadd(vmul(los, half), vmul(side, sqrtf(1.0f - half * half)));
        }
    }
    *desired        = aim;
    s->target_speed = (r > sp->lock_range * 1.3f) ? smax
                    : smin + (smax - smin) * bc.speed_k;
    s->steer_by     = STEER_PRESS;
    s->stale_t      = 0.0f;
    return true;
}

// THE CHANCE THIS PILOT SEES IN A ROUND. See ENEMY_SHOT_MIN_FIGHTER.
//
// A round of class `wsp`, at `from` with speed `vm`, against a target at `tpos`
// moving at `tvel` (both relative, as missile_target sees them). Four numbers,
// each a ratio so one table serves every class and both directions -- this ship's
// own shot, and a round coming in at it:
//   the target's speed over the round's; its speed across the line of sight over
//   the round's; its speed along it, away from the round, over the round's; and
//   the time to impact at the present closing speed over the round's life.
// The weights are a logistic fitted to 31,000 rounds of headless fights, both
// sides' rounds against both author-like seats and the AI, per weapon class, with
// semi-active rounds whose launcher let go of them left out: for that class it is
// the chance IF it is lit (captures/brk/fit_phit.py, 2026-10-26).
static const float HIT_W[SHIP_CLASSES][5] = {
    //  bias    speed   across  away    time
    {  3.048f, -2.657f, -0.830f, 0.990f, -0.282f },   // AEGIS
    {  4.403f, -2.375f, -1.570f, 1.187f, -0.016f },   // LANCE
    {  3.574f, -2.050f, -2.439f, 0.929f, -0.578f },   // CHARIOT
    {  5.469f, -2.480f, -2.184f, 1.026f, -0.722f },   // BALLISTA
};

static float hit_chance(const ShipSpec* wsp, Vec3 from, float vm, Vec3 tpos, Vec3 tvel) {
    const int c = (int)(wsp - &vg_ship_class[0]);
    if (c < 0 || c >= SHIP_CLASSES) return 1.0f;
    const Vec3  d = vsub(tpos, from);
    const float r = vlen(d);
    if (r < 1.0f) return 1.0f;
    const Vec3  los  = vmul(d, 1.0f / r);
    if (vm < 1.0f) vm = 1.0f;
    const float vt2  = vdot(tvel, tvel);
    const float away = vdot(tvel, los);
    const float lat2 = vt2 - away * away;
    const float lat  = sqrtf(lat2 > 0.0f ? lat2 : 0.0f);
    const float clos = vm - away;
    const float tti  = r / (clos > 30.0f ? clos : 30.0f);
    const float* w = HIT_W[c];
    const float z = w[0] + w[1] * sqrtf(vt2) / vm + w[2] * lat / vm + w[3] * away / vm
                  + w[4] * tti / wsp->msl_life;
    return 1.0f / (1.0f + expf(-z));
}

// THIS SHIP'S OWN SHOT, at the player, judged against its class's minimum.
static bool shot_worth(const Ship* s, const ShipSpec* sp) {
    if (!vg_enemy_break) return true;
    const float need = agg(AGG_SHOT_MIN, agg_col(sp));   // see AGGRESSION
    return hit_chance(sp, s->pos, sp->msl_speed, v3(0, 0, 0), v3(0, 0, vg.speed)) >= need;
}

// A ROUND COMING IN AT THIS SHIP, as its pilot perceives it. See
// ENEMY_DANGER_TTI. Zero if there is none, if its seeker no longer holds this
// ship (it cannot come back: no class that fires at an enemy reacquires once it is
// clear of the cone), if it is opening, or if it is more than ENEMY_DANGER_TTI from
// impact. Otherwise hit_chance from the round's side, with this ship flying on as
// it is.
static float incoming_danger(const Ship* s, const Missile* m) {
    if (!m || !m->spec) return 0.0f;
    const Vec3  d = vsub(s->pos, m->pos);
    const float r = vlen(d);
    if (r < 1.0f) return 1.0f;
    const Vec3 los = vmul(d, 1.0f / r);
    if (vdot(m->dir, los) < m->spec->msl_seeker_cos) return 0.0f;
    const Vec3  tv   = vmul(s->fwd, s->speed);
    const float clos = m->speed * vdot(m->dir, los) - vdot(tv, los);
    if (clos <= 0.0f || r > clos * ENEMY_DANGER_TTI) return 0.0f;
    return hit_chance(m->spec, m->pos, m->speed, s->pos, tv);
}

// THE EXPOSED SHOT. See ENEMY_EXPO_MAX. Returns true while it is steering.
//
// It ends on the class's own terms after its first shot -- the burst, the whole
// rack for a slasher, the round lit all the way in for a semi-active hull -- or
// when the player turns their nose onto it with no lock in hand, or at the merge,
// or on its ceiling. Then it waits ENEMY_EXPO_COOLDOWN before it looks again.
static void expo_tick(Ship* s, float dt) {
    if (s->expo_t > 0.0f) {
        s->expo_t -= dt;
        if (s->expo_t <= 0.0f) s->expo_cd = agg(AGG_EXPO_CD, agg_col(s->spec));
    } else if (s->expo_cd > 0.0f) {
        s->expo_cd -= dt;
    }
    if (s->expo_shot_t >= 0.0f) s->expo_shot_t += dt;
}

static bool exposed_shot(Ship* s, const ShipSpec* sp, bool reacted, float inc_range,
                         const Missile* inc, float smin, float smax, Vec3* desired) {
    if (!vg_enemy_break) return false;
    const Vec3  to = vsub(v3(0, 0, 0), s->pos);
    const float r  = vlen(to);
    if (r < 1.0f) return false;
    // Where this ship sits on the player's canopy: inside their cone and range they
    // threaten it; anywhere else, they are exposed to it.
    const bool threat  = r <= vg.spec->lock_range
                      && s->pos.z / r >= vg_lock_cos_at(vg.spec, r, false);
    const bool exposed = !threat && r <= sp->lock_range && r > ENEMY_ATTACK_MIN;

    // NOT A LANCE. It has to bank its locks before it may shoot, and a few seconds of
    // pointing is not long enough to: measured, with this its kills went to nothing
    // in both modes and its draws went up. Its break run is where it shoots.
    if (sp->tactic == TACTIC_GEOMETRY) return false;
    if (s->expo_t <= 0.0f) {
        if (s->expo_cd > 0.0f || !exposed || !reacted || s->rounds <= 0) return false;
        s->expo_t      = ENEMY_EXPO_MAX;
        s->expo_shot_t = -1.0f;
        s->expo_rounds = s->rounds;
    }
    if (s->rounds < s->expo_rounds && s->expo_shot_t < 0.0f) s->expo_shot_t = 0.0f;
    s->expo_rounds = s->rounds;

    const BrkClass bc = brk_class(sp->tactic);
    const bool shot_done = s->expo_shot_t >= 0.0f &&
        (s->expo_shot_t > bc.after || (s->rounds <= 0 && sp->tactic == TACTIC_SLASH));
    const bool give_up = (threat && !s->locked) || r <= ENEMY_ATTACK_MIN
                      || (s->rounds <= 0 && s->expo_shot_t < 0.0f);
    if (shot_done || give_up) {
        s->expo_t      = 0.0f;
        s->expo_cd     = agg(AGG_EXPO_CD, agg_col(sp));
        s->expo_shot_t = -1.0f;
        return false;
    }
    // HOLDING THE ADVANTAGE, it answers only a round it judges likely to hit:
    // see ENEMY_EVADE_DANGER.
    if (reacted && inc_range < ENEMY_EVADE_RANGE
        && incoming_danger(s, inc) >= agg(AGG_EVADE_MIN, agg_col(sp))) return false;

    *desired        = to;
    // The turn back, as the hunt flies it: see AGG_TURN_COS.
    const bool turning = vdot(s->fwd, vmul(to, 1.0f / r)) < agg(AGG_TURN_COS, agg_col(sp));
    s->target_speed = smin + (smax - smin) * (turning ? agg(AGG_TURN_SPEED, agg_col(sp)) : bc.speed_k);
    s->steer_by     = STEER_PRESS;
    s->stale_t      = 0.0f;
    return true;
}

// BALLISTA LIGHTS ITS ROUND. See ENEMY_ILLUM_DANGER. Returns true while it is
// steering: the player held in its lock, at a speed it can turn at.
static bool lighting(Ship* s, const ShipSpec* sp, int index, const Missile* inc,
                     float smin, float smax, Vec3* desired) {
    s->illum = ILLUM_NONE;
    if (!vg_enemy_break || !vg_msl_semi_active(sp)) return false;
    bool lit = false;
    for (int i = 0; i < MAX_MISSILES && !lit; i++) {
        const Missile* m = &vg.msl[i];
        lit = m->alive && !m->from_player && m->shooter == index && m->locked;
    }
    if (!lit) return false;
    if (incoming_danger(s, inc) >= agg(AGG_ILLUM_DANGER, agg_col(sp))) {
        s->illum = ILLUM_LET_GO;
        return false;
    }
    s->illum        = ILLUM_HOLD;
    *desired        = vsub(v3(0, 0, 0), s->pos);
    s->target_speed = smin + (smax - smin) * ENEMY_ILLUM_SPEED;
    s->steer_by     = STEER_PRESS;
    s->stale_t      = 0.0f;
    return true;
}

// THE SALVO, banked. See AGG_SALVO. A banking rack fires when it holds that many
// rounds banked, or all it has -- or when the hunt is about to break away, so what
// is banked is not carried out of the fight.
static bool salvo_ready(const Ship* s, const ShipSpec* sp) {
    if (!vg_enemy_break || sp->wpn != WPN_SLAAM) return true;
    const int c    = agg_col(sp);
    const int want = (int)agg(AGG_SALVO, c);
    if (want <= 1) return true;
    if (s->stacks >= ((want < s->rounds) ? want : s->rounds)) return true;
    return vlen(s->pos) < agg(AGG_IN_MIN, c) * sp->lock_range;
}

// THE HUNT. See AGG_HUNT_RANGE. Returns true while it is steering.
//
// Whenever the player is in range it goes for them: lead pursuit, the aim kept
// inside half the lock cone so the nose stays where a lock is earned; flat out
// from outside AGG_LOCK_RANGE, because a player who turns bleeds speed and one
// who runs is matched; and at the class's locking speed inside it. A player
// coming the other way is a head-on shot, and it takes it -- the closing speeds
// add, and the collision floors above are what stop the pass, not the hunt. A
// pass that missed is followed by the next one at once: there is no reset here to
// wait out. It gives way only to a round coming in that it judges near-certain to
// hit, AGG_EVADE_MIN, and to an empty rack (AGG_HUNT_DRY) -- except an AR-AAM's,
// which rearms facing the player and so keeps hunting.
static bool hunting(Ship* s, const ShipSpec* sp, bool reacted, const Missile* inc,
                    float smin, float smax, float dt, Vec3* desired) {
    if (!vg_enemy_break) return false;
    const int   c  = agg_col(sp);
    const Vec3  to = vsub(v3(0, 0, 0), s->pos);
    const float r  = vlen(to);
    const float lo = (vg_enemy_net && vg_net_available()) ? agg(AGG_NET_HUNT_MIN, c)
                                                         : agg(AGG_HUNT_MIN, c);
    if (r < 1.0f || r > agg(AGG_HUNT_RANGE, c) * sp->lock_range || r < lo * sp->lock_range)
        return false;
    if (s->rounds <= 0 && sp->wpn != WPN_ARAAM && s->reload_t > agg(AGG_HUNT_DRY, c))
        return false;
    if (reacted && incoming_danger(s, inc) >= agg(AGG_EVADE_MIN, c)) return false;

    const Vec3 los = vmul(to, 1.0f / r);
    // THE CHARGE AND THE BREAK-AWAY. See AGG_IN_MIN. Closer than it, the hunt stops
    // closing and turns away -- by AGG_OUT_ANG from straight out, on the side the
    // nose is already on, so it is a turn and never a reversal -- at full speed,
    // until the range is back to AGG_OUT_R or AGG_OUT_MAX has gone; then in again.
    const float in_min = agg(AGG_IN_MIN, c) * sp->lock_range;
    if (in_min > 0.0f) {
        if (!s->hunt_out && r < in_min) { s->hunt_out = 1; s->hunt_t = 0.0f; }
        if (s->hunt_out) {
            s->hunt_t += dt;
            // Chased -- inside the player's own cone and range -- it turns back in
            // unless AGG_OUT_CHASED says to keep going.
            const bool chased = r <= vg.spec->lock_range
                             && s->pos.z / r >= vg_lock_cos_at(vg.spec, r, false);
            if (r >= agg(AGG_OUT_R, c) * sp->lock_range || s->hunt_t >= agg(AGG_OUT_MAX, c)
                || (chased && agg(AGG_OUT_CHASED, c) < 0.5f)) {
                s->hunt_out = 0;
            } else {
                Vec3 lat = vsub(s->fwd, vmul(los, vdot(s->fwd, los)));
                if (vlen2(lat) < 1e-4f) lat = vcross(los, s->up);
                lat = vnorm(lat);
                const float th = agg(AGG_OUT_ANG, c) * 0.0174533f;
                *desired        = vadd(vmul(los, -cosf(th)), vmul(lat, sinf(th)));
                s->target_speed = smax - (smax - smin) * (1.0f - agg(AGG_OUT_SPEED, c));
                s->steer_by     = STEER_RESET;
                s->stale_t      = 0.0f;
                return true;
            }
        }
    } else {
        s->hunt_out = 0;
    }
    float t_go = r / (s->speed > 100.0f ? s->speed : 100.0f);
    if (t_go > ENEMY_BRK_LEAD_MAX) t_go = ENEMY_BRK_LEAD_MAX;
    Vec3 aim = vadd(to, v3(0.0f, 0.0f, vg.speed * t_go));
    // BESIDE THEM: see AGG_OFFSET. The side is the ship's own stable one.
    Vec3 side = vsub(s->offset_dir, vmul(los, vdot(s->offset_dir, los)));
    if (vlen2(side) > 1e-4f) aim = vadd(aim, vmul(vnorm(side), agg(AGG_OFFSET, c)));
    const float half = cosf(0.5f * acosf(vg_lock_cos_at(sp, r, false)));
    const Vec3  a    = vnorm(aim);
    if (vdot(a, los) < half) {
        const Vec3 side = vnorm(vsub(a, vmul(los, vdot(a, los))));
        aim = vadd(vmul(los, half), vmul(side, sqrtf(1.0f - half * half)));
    }
    *desired        = aim;
    // THE TURN BACK. See AGG_TURN_COS: with the player this far off the nose the
    // speed comes off, so the nose comes round at the hull's best rate.
    const bool turning = vdot(s->fwd, los) < agg(AGG_TURN_COS, c);
    s->target_speed = smin + (smax - smin) * (turning ? agg(AGG_TURN_SPEED, c)
                    : (r > agg(AGG_LOCK_RANGE, c) * sp->lock_range) ? agg(AGG_CHASE_SPEED, c)
                                                                    : agg(AGG_LOCK_SPEED, c));
    s->steer_by     = STEER_PRESS;
    s->stale_t      = 0.0f;
    return true;
}

// Which row of the class table this hull is, or -1 if it is not one of them.
static int class_of(const ShipSpec* sp) {
    const int i = (int)(sp - &vg_ship_class[0]);
    return (i >= 0 && i < SHIP_CLASSES) ? i : -1;
}

// A COLLISION with opponent `index` (collide_player, before it dies): whose ram, for
// the score's ram rule. Conservative, as the main thread set it (2026-10-27): the
// NETWORK'S if it was closing on the seat with its heading within 30 degrees of it,
// or if the network was steering that frame; the SEAT'S only if the seat drove into
// it from behind or the side -- the opponent ahead of the seat's beam, heading
// elsewhere, and not the network's frame. Anything else counts against the network.
// Most collisions in Gameplay's log were the seat running into an opponent it chased
// toward the wall.
void vg_census_collision(int index) {
    if (index < 0 || index >= MAX_ENEMIES || !vg.enemy[index].spec) return;
    const Ship* s = &vg.enemy[index];
    const int c = class_of(s->spec);
    if (c < 0) return;
    const float r = vlen(s->pos);
    const Vec3  to_seat = (r > 1e-3f) ? vmul(s->pos, -1.0f / r) : v3(0, 0, 1);
    const bool  heading = vdot(s->fwd, to_seat) >= CEN_RAM_COS;
    const bool  net     = s->steer_by == STEER_NET;
    const bool  ahead   = s->pos.z > 0.0f;     // the seat's nose is +z
    if (heading || net || !ahead) g_cen_rams_net[c]++;
    else                          g_cen_rams_seat[c]++;
}

// CEN_EDGE on both ends of a window, for the same one-frame seam as the shots: a
// window opens and closes at a census, the round left a frame either side of it.
#define CEN_EDGE 0.1f
static bool cen_inside(const float* a, const float* b, float t) {
    for (int k = 0; k < CEN_RUNS; k++)
        if (a[k] >= 0.0f && t >= a[k] - CEN_EDGE && t <= b[k] + CEN_EDGE) return true;
    return false;
}

// One of opponent `shooter`'s rounds ended (detonate()): `hit` if it hit the seat,
// `why` the missile census's reason (3 is the wall), `age` how long it flew. For
// the fight census; counting only.
void vg_census_round_end(int shooter, float age, bool hit, int why) {
    if (shooter < 0 || shooter >= MAX_ENEMIES || !vg.enemy[shooter].spec) return;
    const int c = class_of(vg.enemy[shooter].spec);
    if (c < 0) return;
    g_cen_ended[c]++;
    if (why == 3) g_cen_wall_end[c]++;
    if (!hit) return;
    g_cen_seat_hits[c]++;
    // Its launch time, against the solution launches kept for this shooter.
    const float launched = vg_bout_time() - age;
    for (int k = 0; k < CEN_SOLS; k++) {
        if (s_cen_sol_t[shooter][k] >= 0.0f
            && fabsf(launched - s_cen_sol_t[shooter][k]) <= CEN_SOL_EDGE) {
            g_cen_sol_hits[c]++;
            break;
        }
    }
    // A hit by a round launched in one of its exposed-shot windows is an EXPOSED HIT,
    // else one launched on a run is a BREAK HIT. Apart from both: a hit while its
    // shooter still holds the light on it is an ILLUMINATED HIT.
    if (cen_inside(s_cen_exp_a[shooter], s_cen_exp_b[shooter], launched))
        g_cen_expo_hits[c]++;
    else if (cen_inside(s_cen_run_a[shooter], s_cen_run_b[shooter], launched))
        g_cen_break_hits[c]++;
    if (vg.enemy[shooter].illum == ILLUM_HOLD) g_cen_illum_hits[c]++;
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
    circle_watch(s, sp, dt);
    expo_tick(s, dt);
    bool broke = false;

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
    // WALL AVOIDANCE IS EXEMPT, and it should be: it is not a reaction to
    // anybody. The gate is on ANSWERING an attack, which is the only thing a
    // person can be late to.
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

    // ABOUT TO HIT THEM, a second ahead. See ENEMY_AVOID_LOOK: the floor below is
    // the same rule as a distance, and as a distance it fires too late to work.
    // Same rank and the same label, because it is the same decision made in time.
    } else if (avoiding(s, dt)) {
        s->steer_by = STEER_EVADE;
        desired     = s->avoid_dir;
        if (vdot(s->fwd, vsub(v3(0, 0, 0), s->pos)) < 0.0f) s->target_speed = smax;
        s->attack_t = 0.0f;
        s->break_t  = 0.0f;

    // TOO CLOSE, AND CONTACT KILLS THE PLAYER OUTRIGHT. See ENEMY_MERGE_FLOOR:
    // this is the floor the network has always had and the tactics never did,
    // and without it an aggressive pilot converts its own aggression into the
    // player's death by touching them. Under the wall, because it is equally
    // fatal.
    } else if (vlen2(s->pos) < ENEMY_MERGE_FLOOR * ENEMY_MERGE_FLOOR) {
        s->steer_by = STEER_EVADE;
        desired     = break_across(vnorm(vsub(v3(0, 0, 0), s->pos)), s->up, -0.75f);
        s->target_speed = smax;
        s->attack_t = 0.0f;      // the pass is over; it got what it was going to get
        s->break_t  = 0.0f;

    // BALLISTA LIGHTS ITS ROUND. See ENEMY_ILLUM_DANGER. Under both collision
    // floors and over everything else, until a round coming in is likely to hit.
    } else if (lighting(s, sp, index, inc, smin, smax, &desired)) {
        broke = true;

    // THE EXPOSED SHOT. See ENEMY_EXPO_MAX. Under both collision floors, over the
    // break: a shot at somebody who cannot answer it is worth more than the plan
    // to set one up.
    } else if (exposed_shot(s, sp, reacted, inc ? inc_range : 1e9f, inc, smin, smax, &desired)) {
        broke = true;

    // OUT OF A CIRCLE, AWAY, AND BACK IN. See ENEMY_CIRCLE_TIME. Under the wall
    // and both collision floors -- it must never be the reason two ships touch --
    // and above the reset, the network and every tactic, because it
    // is a decision about the shape of the fight, and with the network flying, the
    // rules own the extend and the turn in while the network flies the rest.
    } else if (breaking(s, sp, reacted, inc ? inc_range : 1e9f, inc ? inc->speed : 0.0f,
                        smin, smax, &desired)) {
        broke = true;

    // THE HUNT. See AGG_HUNT_RANGE. Under the floors, the light, the exposed shot
    // and the break; over the reset, the network, the missile evade, the tail
    // defence and every tactic -- an opponent that is in range and not about to
    // be hit is attacking.
    } else if (hunting(s, sp, reacted, inc, smin, smax, dt, &desired)) {
        broke = true;

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
            // An AR-AAM rack only refills with the contact in front -- see tactic_dry.
            tactic_dry(s, to, smax, &desired, sp->wpn == WPN_ARAAM);
        } else
        // THE TRAINED PILOT, IF THERE IS ONE AND IT WILL TAKE THE FIGHT.
        //
        // It sits exactly where a class tactic sits, and that is the whole design
        // of this function paying off: everything above -- the wall, the
        // missile, the tail -- is survival that every hull does the same
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
            if (index >= 0 && index < MAX_ENEMIES) {
                if (s->locked) {
                    if (!s_census_locked[index] && s_census_nolock[index] >= AI_REENGAGE_GAP)
                        g_ai_lockon[c]++;
                    s_census_nolock[index] = 0.0f;
                } else {
                    s_census_nolock[index] += dt;
                }
                if (s_census_nolock[index] > AI_DEAD_TIME) g_ai_dead[c]++;
                s_census_locked[index] = s->locked;
                census_fight(s, sp, c, index, dt);
            }
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
    // SKIPPED FOR THE THINGS THAT OUTRANK A FIGHT. Not into a wall, and not
    // while breaking from a missile.
    // AND NOT THE NETWORK, WHEN A HARNESS SAYS SO. The pull bends whatever flew this
    // frame 55% toward the player -- the network's heading included, which is the
    // opposite of the "manoeuvre to stay alive" the author asked of it. Whether the
    // network should be exempt is a measurement, so it is a switch first: see
    // vg_net_corner_exempt.
    if (s->hull < sp->hull * ENEMY_CORNERED_HULL &&
        s->steer_by != STEER_WALL && s->steer_by != STEER_EVADE && !broke &&
        !(vg_net_corner_exempt && s->steer_by == STEER_NET)) {
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
    {   // How much of its turn it used: the circle test reads it next frame.
        const float cap = erate * dt;
        const float a   = atan2f(vlen(vcross(before, s->fwd)), vdot(before, s->fwd));
        const float f   = (cap > 1e-6f) ? a / cap : 0.0f;
        s->turn_frac = (f > 1.0f) ? 1.0f : f;
    }
    // FOR THE CENSUS: how much of its own turn this pilot used. Counting only.
    if (index >= 0 && index < MAX_ENEMIES && erate * dt > 1e-6f) {
        const float turned = atan2f(vlen(vcross(before, s->fwd)), vdot(before, s->fwd));
        s_cen_pull[index] = fminf(1.0f, turned / (erate * dt));
    }

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

    // THE SHOT, JUDGED, last and only when everything else would fire: see
    // ENEMY_SHOT_MIN_FIGHTER. A network with its own opinion (judged > 0) keeps it.
    if (s->rounds > 0 && s->fire_cd <= 0 && s->locked && banked && (judged != 0)
        && (judged > 0 || (shot_worth(s, sp) && salvo_ready(s, sp)))) {
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
