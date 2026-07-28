#include "vg_sim.h"
#include "vg_arena.h"
#include <math.h>

// Seeker guidance. The whole missile duel rests on one rule -- a missile can only
// pull so many degrees per second -- and on the seeker cone that turns falling
// behind that limit into a permanent miss rather than an endless chase.

void vg_launch_missile(bool from_player, Vec3 pos, Vec3 dir, int target,
                       const ShipSpec* spec) {
    Missile* m = nullptr;
    for (int i = 0; i < MAX_MISSILES; i++) if (!vg.msl[i].alive) { m = &vg.msl[i]; break; }
    if (!m) return;

    m->alive       = true;
    m->from_player = from_player;
    m->locked      = true;
    m->spec        = spec;
    m->pos         = pos;
    m->dir         = vnorm(dir);
    m->life        = spec->msl_life;
    m->age         = 0;
    m->target      = target;
    m->last_range  = 1e9f;
    m->trail_acc   = 0;
    m->trail_n     = 0;
    m->trail_head  = 0;
}

static inline void trail_push(Missile* m) {
    m->trail_head = (uint8_t)((m->trail_head + 1) % MISSILE_TRAIL);
    m->trail[m->trail_head] = m->pos;
    if (m->trail_n < MISSILE_TRAIL) m->trail_n++;
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
static void report(MslEvent e) {
    // Only a STRICTLY better outcome is protected -- a kill must not be
    // downgraded by a second round missing the wreckage. An equal outcome
    // refreshes the banner, so two misses in quick succession read as two.
    if (vg.msl_event_t > 0.0f && vg.msl_event > e) return;
    vg.msl_event   = e;
    vg.msl_event_t = 1.1f;
}

static void detonate(Missile* m, bool hit) {
    vg_spawn_debris(m->pos, hit ? 14.0f : 5.0f, hit ? 8 : 3);
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
    float t = 1.0f - range / MISSILE_HIT_RADIUS;   // 1 at the centre, 0 at the rim
    if (t < 0.0f) t = 0.0f; else if (t > 1.0f) t = 1.0f;
    return w->msl_damage * (w->msl_graze_floor + (1.0f - w->msl_graze_floor) * t);
}

static void hit_enemy(int index, float dmg) {
    Ship* s = &vg.enemy[index];
    if (!s->alive) return;
    s->hull -= dmg;
    s->hit_flash = 0.2f;
    if (s->hull <= 0.0f) {
        vg_spawn_debris(s->pos, 22.0f, 14);
        s->alive = false;
        vg.kills++;
        vg.score += 500;
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

        if (m->locked && have_target) {
            Vec3  to    = vsub(tpos, m->pos);
            float range = vlen(to);

            if (range > 1e-3f) {
                Vec3 los = vmul(to, 1.0f / range);

                // Seeker cone. Once the bearing leaves the cone the lock is gone
                // for good -- the missile keeps its heading and sails past. This
                // is the failure mode a hard break is meant to force, and it is
                // emergent rather than scripted.
                if (vdot(m->dir, los) < MISSILE_SEEKER_COS) {
                    m->locked = false;
                } else {
                    // Lead pursuit: aim where the target will be, which is what
                    // bends the flight path into the arc you actually see.
                    Vec3 aim = tpos;
                    if (m->age > MISSILE_ARM_TIME) {
                        float t_int = range / m->spec->msl_speed;
                        if (t_int > 1.2f) t_int = 1.2f;
                        aim = vadd(tpos, vmul(tvel, t_int));
                    }
                    m->dir = vg_turn_toward(m->dir, vsub(aim, m->pos),
                                            m->spec->msl_turn * dt);
                }
            }

            // Proximity fuse: once inside fuse range and the range starts opening
            // again, this is the closest we will ever get.
            if (range < MISSILE_HIT_RADIUS * 2.5f && range > m->last_range) {
                // Score off the CLOSEST approach, not the current range. The fuse
                // fires on the frame after the range starts opening again, so
                // using `range` would charge every detonation a frame's worth of
                // separation it never actually had -- which at 12 units per frame
                // against an 18-unit radius is most of the falloff curve.
                bool hit = m->last_range < MISSILE_HIT_RADIUS;
                if (hit) {
                    float dmg = impact_damage(m, m->last_range);
                    if (m->target < 0) {
                        vg_damage_player(dmg);
                    } else {
                        bool was_alive = vg.enemy[m->target].alive;
                        hit_enemy(m->target, dmg);
                        if (m->from_player)
                            report((was_alive && !vg.enemy[m->target].alive)
                                   ? MSL_DESTROYED : MSL_HIT);
                    }
                }
                detonate(m, hit);
                continue;
            }
            m->last_range = range;
        }

        m->pos = vadd(m->pos, vmul(m->dir, m->spec->msl_speed * dt));

        m->trail_acc += dt;
        if (m->trail_acc >= TRAIL_SAMPLE_DT) {
            m->trail_acc = 0;
            trail_push(m);
        }

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
        if (vlen2(m->pos) > CULL_RADIUS * CULL_RADIUS) {
            if (m->from_player) report(MSL_MISSED);
            m->alive = false;
        }
    }
}
