#include "vg_sim.h"
#include "vg_arena.h"
#include <math.h>

// Seeker guidance. The whole missile duel rests on one rule -- a missile can only
// pull so many degrees per second -- and on the seeker cone that turns falling
// behind that limit into a permanent miss rather than an endless chase.

void vg_launch_missile(bool from_player, Vec3 pos, Vec3 dir, int target) {
    Missile* m = nullptr;
    for (int i = 0; i < MAX_MISSILES; i++) if (!vg.msl[i].alive) { m = &vg.msl[i]; break; }
    if (!m) return;

    m->alive       = true;
    m->from_player = from_player;
    m->locked      = true;
    m->pos         = pos;
    m->dir         = vnorm(dir);
    m->life        = MISSILE_LIFE;
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

static void detonate(Missile* m, bool hit) {
    vg_spawn_debris(m->pos, hit ? 14.0f : 5.0f, hit ? 8 : 3);
    m->alive = false;
}

static void hit_enemy(int index) {
    Ship* s = &vg.enemy[index];
    if (!s->alive) return;
    s->hp--;
    s->hit_flash = 0.2f;
    if (s->hp <= 0) {
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
                        float t_int = range / MISSILE_SPEED;
                        if (t_int > 1.2f) t_int = 1.2f;
                        aim = vadd(tpos, vmul(tvel, t_int));
                    }
                    m->dir = vg_turn_toward(m->dir, vsub(aim, m->pos),
                                            MISSILE_TURN_RATE * dt);
                }
            }

            // Proximity fuse: once inside fuse range and the range starts opening
            // again, this is the closest we will ever get.
            if (range < MISSILE_HIT_RADIUS * 2.5f && range > m->last_range) {
                bool hit = range < MISSILE_HIT_RADIUS;
                if (hit) {
                    if (m->target < 0) vg_damage_player(DMG_MISSILE);
                    else               hit_enemy(m->target);
                }
                detonate(m, hit);
                continue;
            }
            m->last_range = range;
        }

        m->pos = vadd(m->pos, vmul(m->dir, MISSILE_SPEED * dt));

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

        if (vlen2(m->pos) > CULL_RADIUS * CULL_RADIUS) m->alive = false;
    }
}
