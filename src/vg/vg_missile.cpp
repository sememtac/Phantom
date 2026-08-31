#include "vg_sim.h"
#include "vg_sfx.h"
#include "vg_arena.h"
#include <math.h>
#include "vg_cockpit.h"

// Seeker guidance. The whole missile duel rests on one rule -- a missile can only
// pull so many degrees per second -- and on the seeker cone that turns falling
// behind that limit into a permanent miss rather than an endless chase.

// HOW OFTEN A ROUND ARRIVES, which nothing has ever counted.
//
// The whole damage table is written in hits: "AEGIS 6 clean hits, LANCE 4". That
// only says anything about a fight if you also know how many shots become hits,
// and that number has been assumed rather than measured for the life of the
// project. It moved a long way the day the lead solution and the fuse were
// fixed, and the first thing anybody noticed was that everything had become
// lethal.
//
// Indexed by who fired: [0] the enemy, [1] the player. Cumulative since boot,
// because a ratio wants a sample and a two-second window is not one.
uint32_t g_msl_fired[2] = { 0, 0 };
uint32_t g_msl_end[2]   = { 0, 0 };   // rounds that have finished, any way
uint32_t g_msl_hit[2]   = { 0, 0 };   // ...of which these arrived
// WHY A ROUND STOPPED, which the hit rate alone cannot say. A class that fires
// as often as another and does a fraction of the damage is failing somewhere
// specific, and these are the places it can fail.
uint32_t g_msl_why[2][5] = { { 0 } };  // hit, near miss, fuse, wall, left world
uint32_t g_msl_endlock[2] = { 0, 0 };  // ...still guided at the end
// WHERE A LOCK WENT. Illumination is the semi-active failure -- the launcher
// stopped looking -- and the cone is the round's own seeker losing the target.
uint32_t g_msl_lost[2][2] = { { 0 } };
uint32_t g_msl_lost_dead[2] = { 0, 0 };
// THE COAST, MEASURED. `dark` counts rounds that lost illumination at least
// once; `relit` counts the times one got it back before the clock ran out. A
// relit count near zero means the coast is buying nothing.
uint32_t g_msl_dark[2]  = { 0, 0 };
uint32_t g_msl_relit[2] = { 0, 0 };
uint32_t g_msl_dmg[2]   = { 0, 0 };   // hull points delivered

bool vg_launch_missile(bool from_player, Vec3 pos, Vec3 dir, int target, int shooter,
                       const ShipSpec* spec) {
    Missile* m = nullptr;
    for (int i = 0; i < MAX_MISSILES; i++) if (!vg.msl[i].alive) { m = &vg.msl[i]; break; }
    // Reported rather than swallowed. The caller has to know, because it is
    // about to charge the player a round for a missile that does not exist.
    if (!m) return false;

    m->alive       = true;
    m->from_player = from_player;
    m->locked      = true;
    m->lost_at     = -1.0f;
    m->dark_t      = 0.0f;
    m->travelled   = 0.0f;
    m->spec        = spec;
    m->pos         = pos;
    m->dir         = vnorm(dir);
    m->life        = spec->msl_life;
    m->age         = 0;
    m->target      = target;
    m->speed       = spec->msl_speed;
    m->shooter     = shooter;
    m->have_last   = false;
    g_msl_fired[from_player ? 1 : 0]++;
    trail_clear(m->trail);
    return true;
}

// Where is this missile's target right now, and how fast is it moving RELATIVE TO
// THE MISSILE? That last part is the whole of it, and it was wrong.
//
// The world flow cancels. Every step, an enemy gets mat3_apply(R, pos) and then
// pos.z -= dz -- and so does every missile, in the very next loop (vg_flight.cpp
// :253-257 and :295-297). The player's forward travel moves BOTH of them by the
// same amount, so it does not appear in the geometry between them at all. The
// only relative motion is what each one flies under its own power.
//
// Subtracting the player's speed anyway put up to 460 units/sec of phantom
// velocity into the lead solution, and the lead runs out to the 1.2s cap -- so the
// round aimed at a point up to 550 units from the target and flew there, confident
// and locked, because the target was still well inside the seeker cone. It read
// from the cockpit as a missile that simply would not track something in plain
// view. It hurt BALLISTA worst: a slow round saturates the cap on every shot, so
// it always paid the full error.
//
// THE PLAYER IS THE OTHER HALF OF THE SAME MISTAKE. The player sits at the origin
// and does not take the -dz; the missile hunting them does. So relative to that
// missile the player IS moving, at exactly the speed they are flying, up the +z
// they are flying along. Zero was never "no prediction needed", it was the same
// term missing with the opposite sign.
static bool missile_target(const Missile* m, Vec3* pos, Vec3* vel) {
    if (m->target < 0) {
        *pos = v3(0, 0, 0);
        *vel = v3(0, 0, vg.speed);
        return true;
    }
    if (m->target >= MAX_ENEMIES) return false;
    const Ship* s = &vg.enemy[m->target];
    if (!s->alive) return false;
    *pos = s->pos;
    *vel = vmul(s->fwd, s->speed);
    return true;
}

// Report what one of OUR rounds did. Only the player's, and only the strongest
// result: a kill should not be overwritten by a second missile missing the
// wreckage a frame later.
// Every round the player fires resolves into something the player is told.
// Nothing is ever dropped: if a banner is already up, this one waits behind it.
//
// The old version discarded any outcome weaker than the one showing, which is
// why shots sometimes vanished without a word -- fire three, kill with the
// second, and the third's miss was silently thrown away.
static void report(MslEvent e) {
    // One click per outcome, at the moment it is REPORTED rather than when it
    // happened -- the click belongs to the banner appearing, and a queued outcome
    // clicks when its turn comes.
    vg_sfx_play(SFX_MSL_EVENT, (e == MSL_DESTROYED) ? 0.7f : 1.0f);
    if (vg_cockpit.banner.t <= 0.0f) {
        vg_cockpit.banner.ev   = e;
        vg_cockpit.banner.t = MSL_BANNER;
        return;
    }
    if (vg_cockpit.banner.qn < (uint8_t)(sizeof(vg_cockpit.banner.queue) / sizeof(vg_cockpit.banner.queue[0])))
        vg_cockpit.banner.queue[vg_cockpit.banner.qn++] = e;
}

static void detonate(Missile* m, bool hit, int why) {
    const int who = m->from_player ? 1 : 0;
    g_msl_end[who]++;
    if (hit) g_msl_hit[who]++;
    if (why >= 0 && why < 5) g_msl_why[who][why]++;
    if (m->locked) g_msl_endlock[who]++;

    vg_spawn_debris(m->pos, hit ? 14.0f : 5.0f, hit ? 8 : 3);
    // A round that runs out of fuel still goes off. It used to leave nothing but
    // three shards, so a missile you had watched all the way in simply stopped
    // existing -- and a fuse that expires is exactly the moment a player is
    // still looking at it.
    vg_spawn_blast(m->pos, hit ? 16.0f : 7.0f, hit ? 3 : 1, 0, 1.0f);
    if (hit) vg_sfx_play(SFX_EXPLODE, 1.0f);
    if (m->from_player && !hit) report(MSL_MISSED);
    m->alive = false;
}

// Damage falls off with how close the fuse actually went off: full value dead
// centre, down to the warhead's graze floor out at the rim. This is what makes
// LANCE and CHARIOT different playstyles rather than different numbers -- a
// narrow high-yield warhead demands correct geometry, a wide low-yield one
// rewards volume. It reads off the ATTACKER's spec, because it is a distinction
// about shooting, and it applies symmetrically to the player's own aim.
static float impact_damage(const Missile* m, float range) {
    const ShipSpec* w = m->spec;
    float t = (w->msl_splash > 0.0f) ? 1.0f - range / w->msl_splash : 1.0f;
    if (t < 0.0f) t = 0.0f; else if (t > 1.0f) t = 1.0f;
    float dmg = w->msl_damage * (w->msl_graze_floor + (1.0f - w->msl_graze_floor) * t);

    // ...AND HOW FAR IT CAME TO GET HERE. See msl_reach_floor.
    //
    // Two different falloffs, and they are asking different questions. The one
    // above is about AIM -- how near the middle the fuse went off. This one is
    // about RANGE, and it exists because the two are otherwise independent: a
    // dead centre hit from four thousand units was worth exactly as much as one
    // from two hundred, so the safest place to shoot from was also the best.
    if (w->msl_reach_floor < 1.0f && w->lock_range > 1.0f) {
        float u = m->travelled / w->lock_range;
        if (u < 0.0f) u = 0.0f; else if (u > 1.0f) u = 1.0f;
        dmg *= 1.0f - (1.0f - w->msl_reach_floor) * u;
    }
    return dmg;
}

static void hit_enemy(int index, float dmg) {
    Ship* s = &vg.enemy[index];
    if (!s->alive) return;
    s->hull -= dmg;
    s->engaged = true;      // being shot at counts as meeting
    s->hit_flash = 0.2f;
    // Said before the ship is cleared, so a dying pilot still has a tag to
    // transmit under.
    vg_comms_say(s, (s->hull <= 0.0f) ? VOICE_DEATH : VOICE_HURT);
    if (s->hull <= 0.0f) {
        // THE ERUPTION LIVES HERE, not at the call site, because this is where
        // the ship actually dies. It was hung off the caller's kill test first
        // and that only covered the missile path, so a ship that went down any
        // other way still just stopped -- and the caller is not the authority on
        // a hull reaching zero anyway.
        vg_spawn_blast(s->pos, 46.0f, 9, 0, 1.9f);
        vg_spawn_shrapnel(s->pos, 30.0f, 54.0f, 34, 4.4f, 1.8f);
        s->alive = false;
    }
}

void vg_update_missiles(float dt) {
    for (int i = 0; i < MAX_MISSILES; i++) {
        Missile* m = &vg.msl[i];
        if (!m->alive) continue;

        m->age  += dt;
        m->life -= dt;
        if (m->life <= 0) { detonate(m, false, 2); continue; }

        Vec3 tpos, tvel;
        bool have_target = missile_target(m, &tpos, &tvel);

        // A seeker that can come back for another pass.
        //
        // The alternative was a wider cone, and the seeker-cone note in
        // cfg_combat.h is right to warn against it: a round that cannot be shaken
        // is a round that is not a decision. This keeps the dodge intact -- the
        // lock still breaks hard, the missile still sails past, the player still
        // gets the moment of having beaten it -- and only then does it turn round.
        //
        // Which is a different threat from an unbreakable one. You can beat it
        // twice, or three times; you just cannot beat it once and forget it.
        // SEMI-ACTIVE: IS ANYBODY STILL LOOKING AT THE TARGET?
        //
        // Asked before everything else, because it does not care what the round's
        // own seeker thinks -- a semi-active round has no opinion of its own. It
        // rides the LAUNCHER's lock, so the launcher letting go is the end of it:
        // the round keeps its heading and sails on, exactly as a broken seeker
        // lock does, and nothing brings it back.
        //
        // The two sides ask the same question of two different pilots. The player
        // must still hold a lock ON THIS TARGET -- switching to somebody else drops
        // the round as surely as losing them does, which is what stops one BALLISTA
        // rack being aimed at three ships at once. An enemy only ever shoots at the
        // player, so its own lock flag is the whole answer.
        //
        // A DEAD LAUNCHER STOPS ILLUMINATING, and that falls out for free: the
        // index goes stale, `alive` is false, and the rounds already in the air go
        // dumb. Killing the ship that is lighting you up is a real counter now,
        // and nobody had to write it.
        if (m->locked && m->spec->msl_saam) {
            bool lit;
            if (m->from_player) {
                lit = vg_wpn.locked && vg_wpn.target == m->target;
            } else {
                lit = (m->shooter >= 0 && m->shooter < MAX_ENEMIES
                       && vg.enemy[m->shooter].alive
                       && vg.enemy[m->shooter].locked);
            }
            // GOING DARK IS NOT THE SAME AS LOSING IT. The launcher that turns
            // away to miss a wall comes back, and a round that gave up on the
            // first unlit frame threw away a shot over a manoeuvre lasting less
            // than a second. The clock only runs while it is actually dark, and
            // resets the moment the light comes back -- so a pilot who keeps
            // glancing at the target keeps the round, and one who leaves does not.
            const int w = m->from_player ? 1 : 0;
            if (lit) {
                if (m->dark_t > 0.0f) g_msl_relit[w]++;
                m->dark_t = 0.0f;
            } else {
                if (m->dark_t <= 0.0f) g_msl_dark[w]++;
                m->dark_t += dt;
            }
            // ALL OR NOTHING, when the class asks for it. msl_coast of zero
            // finishes the round on the frame the light goes out -- there is no
            // grace, and losing them for an instant costs the shot.
            if (!lit && m->dark_t > m->spec->msl_coast) {
                m->locked  = false;
                m->lost_at = m->age;
                const int w = m->from_player ? 1 : 0;
                g_msl_lost[w][0]++;
                // TWO VERY DIFFERENT FAILURES SHARE THIS BRANCH. A launcher that
                // turned away threw the round away; a launcher that was SHOT did
                // not. Counting them together says the pilot is at fault when the
                // player may simply have killed it.
                const bool dead = m->from_player
                                ? false
                                : !(m->shooter >= 0 && m->shooter < MAX_ENEMIES
                                    && vg.enemy[m->shooter].alive);
                if (dead) g_msl_lost_dead[w]++;
            }
        }

        if (!m->locked && have_target && m->lost_at >= 0.0f
            && m->spec->msl_reacq_cos <= 1.0f
            && (m->age - m->lost_at) > m->spec->msl_reacq_delay) {
            Vec3  to    = vsub(tpos, m->pos);
            float range = vlen(to);
            if (range > 1e-3f
                && vdot(m->dir, vmul(to, 1.0f / range)) >= m->spec->msl_reacq_cos) {
                m->locked  = true;
                m->lost_at = -1.0f;
            }
        }

        // THE AIM IS THE ENGINE, and this is the whole of it.
        //
        // While the round is guided it accelerates; the frame it stops being
        // guided it stops accelerating and keeps whatever speed it had earned.
        // For a semi-active class "guided" means the launcher is still looking,
        // so a pilot who holds a target in view for twenty seconds is building a
        // faster and faster round, and a pilot who looks away is left with
        // whatever it had reached, flying straight.
        //
        // Nothing decays. A round that has been let go is not slowed down as
        // well -- losing the lock is already the whole punishment, and taking the
        // speed back would mean the pilot is charged twice for one mistake.
        // HOW STRAIGHT IT IS FLYING AT THEM, worked out before the engine rather
        // than after it, because for one class this IS the engine. Left at -2.0f
        // when there is nothing to measure against, which no cosine can reach and
        // which therefore reads as "no geometry" rather than as bad geometry.
        float align = -2.0f;
        if (m->locked && have_target) {
            const Vec3  d = vsub(tpos, m->pos);
            const float r = vlen(d);
            if (r > 1e-3f) align = vdot(m->dir, vmul(d, 1.0f / r));
        }

        if (m->locked && m->dark_t <= 0.0f && m->spec->msl_accel > 0.0f
            && (m->spec->msl_accel_cos < -1.0f
                || align >= m->spec->msl_accel_cos)) {
            m->speed += m->spec->msl_accel * dt;
            if (m->speed > m->spec->msl_speed_max) m->speed = m->spec->msl_speed_max;
        }

        if (m->locked && have_target) {
            Vec3  to    = vsub(tpos, m->pos);
            float range = vlen(to);

            if (range > 1e-3f) {
                Vec3 los = vmul(to, 1.0f / range);

                // Seeker cone. Once the bearing leaves the cone the lock is gone
                // for good -- the missile keeps its heading and sails past. This
                // is the failure mode a hard break is meant to force, and it is
                // emergent rather than scripted.
                if (vdot(m->dir, los) < m->spec->msl_seeker_cos) {
                    m->locked  = false;
                    m->lost_at = m->age;
                    g_msl_lost[m->from_player ? 1 : 0][1]++;
                } else {
                    // Lead pursuit: aim where the target will be, which is what
                    // bends the flight path into the arc you actually see.
                    Vec3 aim = tpos;
                    if (m->age > m->spec->msl_arm_time) {
                        float t_int = range / m->speed;
                        if (t_int > 1.2f) t_int = 1.2f;
                        aim = vadd(tpos, vmul(tvel, t_int));
                    }
                    m->dir = vg_turn_toward(m->dir, vsub(aim, m->pos),
                                            m->spec->msl_turn * dt);
                }
            }

            // PROXIMITY FUSE, over the STEP rather than at its ends.
            //
            // This used to compare two consecutive RANGES: arm inside 2.5x the
            // radius, fire on the frame the range started opening, and score off
            // whichever of the two samples was smaller. That works, but it can only
            // ever report a distance it happened to sample, and a round closing
            // head-on shuts the range by up to (msl_speed + 460) * 0.02 -- around
            // 16 units for AEGIS and 20 for a 520 u/s warhead. So the sampled
            // minimum overstates how close the round really passed, the fuse scores
            // a rim hit or a clean miss on a round that flew through the middle,
            // and the tighter the radius the worse it gets: a 14-unit fuse was
            // delivering about 10.
            //
            // The two bearings ARE the chord the round flew relative to the target
            // over this step, so the closest point on that segment to the origin is
            // the true miss distance, exactly. No velocity model and no arming
            // window: nothing here has to assume anything about how the world flows
            // past, which is the part that would otherwise have to be got right.
            if (m->have_last) {
                const Vec3  r0 = m->last_rel;              // bearing at step start
                const Vec3  d  = vsub(to, r0);             // ...and how it moved
                const float dd = vdot(d, d);
                float u = 1.0f;
                if (dd > 1e-6f) {
                    u = -vdot(r0, d) / dd;
                    if (u < 0.0f) u = 0.0f; else if (u > 1.0f) u = 1.0f;
                }
                const float d_min = vlen(vadd(r0, vmul(d, u)));
                // u < 1 means the nearest point lies INSIDE this step, so the round
                // is opening again: this is the closest it will ever be. Same moment
                // the old range test fired on, decided exactly instead of by sample.
                if (u < 1.0f && d_min < m->spec->msl_splash * 2.5f) {
                    bool hit = d_min < m->spec->msl_splash;
                    if (hit) {
                        float dmg = impact_damage(m, d_min);
                    g_msl_dmg[m->from_player ? 1 : 0] += (uint32_t)dmg;
                        if (m->target < 0) {
                            vg_damage_player(dmg);
                        } else {
                            bool was_alive = vg.enemy[m->target].alive;
                            hit_enemy(m->target, dmg);
                            const bool killed = was_alive && !vg.enemy[m->target].alive;
                            if (m->from_player)
                                report(killed ? MSL_DESTROYED : MSL_HIT);
                        }
                    }
                    detonate(m, hit, hit ? 0 : 1);
                    continue;
                }
            }
            m->last_rel  = to;
            m->have_last = true;
        }

        m->pos = vadd(m->pos, vmul(m->dir, m->speed * dt));

        m->travelled += m->speed * dt;
        trail_sample(m->trail, dt, m->pos, TRAIL_SAMPLE_DT);

        // A missile that runs out of world detonates against it, which makes
        // leading one into a wall a legitimate way to defeat it.
        if (vg_arena_clearance(vg_arena_local_of(m->pos)) < 0.0f) {
            detonate(m, false, 3);
            continue;
        }

        // Leaving the world silently was the missing MISSED. A round that breaks
        // lock flies ballistic and can clear CULL_RADIUS long before its life
        // expires -- especially a BALLISTA's, which burns for eighteen seconds --
        // and this path just switched it off without ever telling the player
        // what happened to it.
        // THE ENVELOPE FOLLOWS THE CLASS THAT FIRED IT. A flat 4200 is right for
        // asteroids, which are scenery, and wrong for a round whose whole identity
        // is reach: BALLISTA locks at 4200, so a flat cull deleted its shot at the
        // moment it arrived. 1.4x is the pursuit-curve allowance -- a seeker flies
        // an arc, not a chord -- and the floor keeps every other class exactly
        // where it was.
        float lim = m->spec->lock_range * 1.4f;
        if (lim < CULL_RADIUS) lim = CULL_RADIUS;
        if (vlen2(m->pos) > lim * lim) {
            // Counted here as well as in detonate: a round that leaves the world
            // never detonates, and a miss that is not counted flatters the rate.
            g_msl_end[m->from_player ? 1 : 0]++;
            g_msl_why[m->from_player ? 1 : 0][4]++;
            if (m->locked) g_msl_endlock[m->from_player ? 1 : 0]++;
            if (m->from_player) report(MSL_MISSED);
            m->alive = false;
        }
    }
}
