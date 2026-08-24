#include "vg_sim.h"
#include "vg_arena.h"
#include "vg_bot.h"
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
    if (s->break_t > 0 || range < ENEMY_BREAK_RANGE / s->nerve) {
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
        Vec3 aim  = vmul(s->offset_dir, ENEMY_OFFSET);
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

// HOW MANY OF THIS PILOT'S OWN ROUNDS ARE STILL FLYING.
//
// Counted rather than remembered, so it needs no bookkeeping and cannot drift: a
// round that has hit, been dodged into uselessness or timed out simply is not
// alive any more, and the pilot is free again. It reads Missile::shooter, which
// exists for semi-active guidance -- the same field answers both questions.
static int rounds_in_flight(int index) {
    int n = 0;
    for (int i = 0; i < MAX_MISSILES; i++) {
        const Missile* m = &vg.msl[i];
        if (m->alive && !m->from_player && m->shooter == index) n++;
    }
    return n;
}

// ...and how many this class is willing to have out there at once. See
// cfg_combat.h: this is trigger DISCIPLINE, which is a different thing from the
// rate limit, and it is the one the rate limit cannot express.
static int inflight_cap(ShipTactic t) {
    switch (t) {
        case TACTIC_STANDOFF: return ENEMY_INFLIGHT_STANDOFF;
        case TACTIC_SLASH:    return ENEMY_INFLIGHT_SLASH;
        case TACTIC_GEOMETRY: return ENEMY_INFLIGHT_GEOMETRY;
        case TACTIC_FIGHTER:
        default:              return ENEMY_INFLIGHT_FIGHTER;
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
// THE VIEWPORT IDIOM CANNOT CROSS OVER. lock_hold_cos below -1 means "as long as
// it is on screen", and an enemy has no screen; ENEMY_LOCK_HOLD_WIDE is that rule
// in the only terms it has. See cfg_combat.h.
static void enemy_update_lock(Ship* s, const ShipSpec* sp, Vec3 to, float range,
                              float sn, float dt) {
    bool ok = (range <= sp->lock_range && range >= 1.0f);
    if (ok) {
        const float c = vdot(s->aim_dir, vnorm(to));
        if (!s->locked) {
            ok = (c > sp->lock_cos);
        } else if (sp->lock_hold_cos < -1.0f) {
            ok = (c > ENEMY_LOCK_HOLD_WIDE);
        } else {
            ok = (c > sp->lock_hold_cos);
        }
    }

    if (!ok) {
        // A lock that breaks costs the nose to get back, exactly as the player's
        // does. That is what puts the choice between manoeuvring and shooting
        // into an enemy's flying too, instead of only into the player's.
        s->lock_t  = 0.0f;
        s->locked  = false;
        return;
    }

    s->lock_t += dt;
    const float need = sp->lock_time * (1.0f + LOCK_SPEED_PENALTY * sn);
    if (s->lock_t >= need) s->locked = true;
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
    const float flee = ENEMY_BREAK_RANGE * STANDOFF_FLEE_K;

    if (s->break_t > 0 || range < flee) {
        if (s->break_t <= 0) {
            s->break_dir = vnorm(vmul(to, -1.0f));   // straight out, no cleverness
            s->break_t   = vg_frand(1.2f, 2.2f);
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
    if (s->break_t > 0 || range < ENEMY_BREAK_RANGE * SLASH_BREAK_K / s->nerve) {
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
        Vec3 aim  = vmul(s->offset_dir, ENEMY_OFFSET * SLASH_OFFSET_K);
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
    if (s->break_t > 0 || range < ENEMY_BREAK_RANGE * GEOM_BREAK_K / s->nerve) {
        if (s->break_t <= 0) {
            s->break_dir = break_across(vnorm(to), s->up, -0.55f);
            s->break_t   = vg_frand(ENEMY_BREAK_TIME_MIN, ENEMY_BREAK_TIME_MAX)
                         / s->nerve;
            s->offset_dir = vg_rand_unit();
        }
        *desired = s->break_dir;
        s->target_speed = smax;
    } else {
        Vec3 aim  = vmul(s->offset_dir, ENEMY_OFFSET * GEOM_OFFSET_K);
        *desired  = vsub(aim, s->pos);
        s->target_speed = (range > close_r) ? smax * 0.85f
                        : smin + (smax - smin) * GEOM_SPEED_K;
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

    } else if (reacted && inc && inc_range < ENEMY_EVADE_RANGE) {
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
        if (vg_bot_fly_enemy(index, s, &desired, &s->target_speed, dt)) {
            // The firing gates below are untouched, so a network-flown enemy
            // still has to be slow enough and hold a lock long enough, in its own
            // class's cone. It gets a different opinion about where to be, not a
            // different game.
        } else
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

        // Holding the angle means holding the NOSE on them, and the tactic has
        // just aimed at a point beside the player so the pass does not converge.
        // Overriding it here rather than threading a flag through all four keeps
        // the tactics about positioning, which is the one thing they are for.
        if (pressing && !dry) {
            desired = to;
            // Slow enough to be allowed to shoot -- the firing gate applies to a
            // won position exactly as it does to a merge -- and slow is also what
            // lets them stay inside the player's turn.
            s->target_speed = smin + (smax - smin) * 0.30f;
        }
    }

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

    if (s->reload_t > 0.0f) {
        s->reload_t -= dt;
        if (s->reload_t <= 0.0f) {
            s->reload_t = 0.0f;
            s->rounds   = sp->magazine;
        }
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
    float fire_sn = (s->speed - smin) / (smax - smin);
    if (fire_sn < 0.0f) fire_sn = 0.0f;
    if (fire_sn > 1.0f) fire_sn = 1.0f;

    // EARNED BEFORE IT IS SPENT. Run every frame, including the frames the ship
    // cannot shoot on -- a lock is built by holding the nose on somebody, and
    // pausing the clock whenever the trigger happened to be cold would make it
    // mean something different from the player's.
    enemy_update_lock(s, sp, to, range, fire_sn, dt);

    // The discipline, and it is deliberately the LAST gate: a pilot who is
    // holding fire because a round of theirs is still out there has done
    // everything else right -- earned the lock, slowed down, closed the range --
    // and is choosing to wait. That is what the ships with a cap are for.
    const int cap = inflight_cap(sp->tactic);

    if (s->rounds > 0 && s->fire_cd <= 0 && s->locked &&
        range < fire_r && range > 60.0f &&
        fire_sn < ENEMY_ENGAGE_SPEED &&
        (cap <= 0 || rounds_in_flight(index) < cap)) {
        // ALONG THE AIM. The rail is still on the nose -- that is where the ship
        // is -- but the round leaves on the direction this pilot thinks is the
        // target, which is the same error that made the lock slow to earn.
        vg_launch_missile(false, vadd(s->pos, vmul(s->fwd, 4.0f)), s->aim_dir, -1,
                          index, sp);
        s->rounds--;

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
