#include "vg_sim.h"
#include "vg_sfx.h"
#include "vg_arena.h"
#include <math.h>
#include "vg_cockpit.h"

// Seeker guidance. The whole missile duel rests on one rule -- a missile can only
// pull so many degrees per second -- and on the seeker cone that turns falling
// behind that limit into a permanent miss rather than an endless chase.

bool vg_launch_missile(bool from_player, Vec3 pos, Vec3 dir, int target,
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
    m->spec        = spec;
    m->pos         = pos;
    m->dir         = vnorm(dir);
    m->life        = spec->msl_life;
    m->age         = 0;
    m->target      = target;
    m->have_last   = false;
    trail_clear(m->trail);
    return true;
}

// Where is this missile's target right now, and how fast is it moving in view
// space? The player is the origin and, in its own frame, motionless -- which is
// why an enemy missile needs no lead prediction at all.
static bool missile_target(const Missile* m, Vec3* pos, Vec3* vel) {
    if (m->target < 0) {
        *pos = v3(0, 0, 0);
        *vel = v3(0, 0, 0);
        return true;
    }
    if (m->target >= MAX_ENEMIES) return false;
    const Ship* s = &vg.enemy[m->target];
    if (!s->alive) return false;
    *pos = s->pos;
    // View-space velocity: its own motion, minus the player's forward travel.
    *vel = vsub(vmul(s->fwd, s->speed), v3(0, 0, vg.speed));
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

static void detonate(Missile* m, bool hit) {
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
    return w->msl_damage * (w->msl_graze_floor + (1.0f - w->msl_graze_floor) * t);
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
        if (m->life <= 0) { detonate(m, false); continue; }

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
                } else {
                    // Lead pursuit: aim where the target will be, which is what
                    // bends the flight path into the arc you actually see.
                    Vec3 aim = tpos;
                    if (m->age > m->spec->msl_arm_time) {
                        float t_int = range / m->spec->msl_speed;
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
                    detonate(m, hit);
                    continue;
                }
            }
            m->last_rel  = to;
            m->have_last = true;
        }

        m->pos = vadd(m->pos, vmul(m->dir, m->spec->msl_speed * dt));

        trail_sample(m->trail, dt, m->pos, TRAIL_SAMPLE_DT);

        // A missile that runs out of world detonates against it, which makes
        // leading one into a wall a legitimate way to defeat it.
        if (vg_arena_clearance(vg_arena_local_of(m->pos)) < 0.0f) {
            detonate(m, false);
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
            if (m->from_player) report(MSL_MISSED);
            m->alive = false;
        }
    }
}
