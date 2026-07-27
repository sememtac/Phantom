#include "vg_sim.h"
#include "vg_arena.h"
#include "vg_sky.h"
#include <Arduino.h>
#include <esp_random.h>
#include <math.h>
#include <string.h>

// State machine, world step, spawning, player weapons and collisions. Geometry,
// seeker guidance and enemy behaviour live in vg_models / vg_missile / vg_ai.

VgGame vg;

// ---------------------------------------------------------------------------
// Spawning
// ---------------------------------------------------------------------------

static void spawn_asteroid(void) {
    Asteroid* a = nullptr;
    for (int i = 0; i < MAX_ASTEROIDS; i++) if (!vg.ast[i].alive) { a = &vg.ast[i]; break; }
    if (!a) return;

    float z   = vg_frand(SPAWN_Z_MIN, SPAWN_Z_MAX);
    float ang = vg_frand(0.0f, 6.2831853f);
    float rad = sqrtf(vg_frand(0.02f, 1.0f)) * z * SPAWN_CONE;

    a->alive  = true;
    a->pos    = vg_arena_clamp_inside(v3(cosf(ang) * rad, sinf(ang) * rad, z),
                                      ARENA_SPAWN_MARGIN);
    a->vel    = v3(vg_frand(-4, 4), vg_frand(-4, 4), 0);
    a->radius = vg_frand(AST_R_MIN, AST_R_MAX);
    a->model  = (uint8_t)((uint32_t)(vg_frand01() * NUM_MODELS) % NUM_MODELS);
    for (int k = 0; k < 3; k++) {
        a->spin[k]      = vg_frand(0.0f, 6.2831853f);
        a->spin_rate[k] = vg_frand(-1.1f, 1.1f);
    }
}

void vg_spawn_debris(Vec3 at, float radius, int count) {
    for (int k = 0; k < count; k++) {
        Debris* d = nullptr;
        for (int i = 0; i < MAX_DEBRIS; i++) if (!vg.deb[i].alive) { d = &vg.deb[i]; break; }
        if (!d) return;
        Vec3 dir = vg_rand_unit();
        d->alive = true;
        d->pos   = vadd(at, vmul(dir, radius * 0.35f));
        d->seg   = vmul(vg_rand_unit(), radius * vg_frand(0.3f, 0.7f));
        d->vel   = vmul(dir, vg_frand(11.0f, 34.0f));
        d->life0 = vg_frand(0.40f, 1.00f);
        d->life  = d->life0;
    }
}

static void spawn_enemy(int i) {
    Ship* s = &vg.enemy[i];
    // Out ahead but off-axis, so a fight opens with a merge rather than with
    // someone already on someone's tail.
    Vec3 dir = vnorm(v3(vg_frand(-0.7f, 0.7f), vg_frand(-0.5f, 0.5f), 1.0f));
    s->alive        = true;
    s->pos          = vg_arena_clamp_inside(
                          vmul(dir, ENEMY_SPAWN_DIST * vg_frand(0.8f, 1.25f)),
                          ARENA_SPAWN_MARGIN);
    s->fwd          = vnorm(vsub(v3(0, 0, 0), s->pos));   // pointed at the player
    s->up           = v3(0, 1, 0);
    s->speed        = (ENEMY_SPEED_MIN + ENEMY_SPEED_MAX) * 0.5f;
    s->target_speed = s->speed;
    s->hp           = ENEMY_HP;
    s->fire_cd      = vg_frand(1.5f, 3.0f);
    s->evade_t      = 0;
    s->break_t      = 0;
    s->offset_dir   = vg_rand_unit();
    s->roll_vis     = 0;
    s->hit_flash    = 0;
}

// ---------------------------------------------------------------------------
// Damage
// ---------------------------------------------------------------------------

static bool s_player_hit = false;

bool vg_player_was_hit(void)  { return s_player_hit; }
void vg_clear_player_hit(void) { s_player_hit = false; }

void vg_damage_player(float amount) {
    // Brief post-hit invulnerability, so one bad moment cannot cascade into three
    // hits before you have had a chance to react.
    if (vg.state == VG_HIT) return;
    vg.health -= amount;
    if (vg.health < 0.0f) vg.health = 0.0f;
    vg.combat_t  = 0.0f;              // repair clock restarts
    vg.hit_flash = 0.6f;
    vg.shake     = 1.0f;
    s_player_hit = true;
}

// ---------------------------------------------------------------------------
// Player weapons
// ---------------------------------------------------------------------------

// Acquire and hold a lock on whichever live enemy is nearest the nose, provided
// it stays inside the cone long enough.
static void update_lock(float dt) {
    int   best   = -1;
    float best_c = PLAYER_LOCK_COS;

    for (int i = 0; i < MAX_ENEMIES; i++) {
        const Ship* s = &vg.enemy[i];
        if (!s->alive) continue;
        float range = vlen(s->pos);
        if (range > PLAYER_LOCK_RANGE || range < 1.0f) continue;
        float c = vdot(vnorm(s->pos), v3(0, 0, 1));   // player looks down +z
        if (c > best_c) { best_c = c; best = i; }
    }

    // Lock time scales with speed. This is what puts a fast ship out of effective
    // engagement range, and because it is re-evaluated every frame, accelerating
    // away also drops a lock you had already earned.
    float sn = (vg.speed - SPEED_MIN) / (SPEED_MAX - SPEED_MIN);
    if (sn < 0.0f) sn = 0.0f;
    if (sn > 1.0f) sn = 1.0f;
    vg.lock_need = PLAYER_LOCK_TIME * (1.0f + LOCK_SPEED_PENALTY * sn);

    if (best < 0) {
        vg.lock_target = -1;
        vg.lock_t      = 0;
        vg.locked      = false;
        return;
    }

    if (best != vg.lock_target) {
        vg.lock_target = best;
        vg.lock_t      = 0;
    }
    vg.lock_t += dt;
    vg.locked = (vg.lock_t >= vg.lock_need);
}

static void player_fire(void) {
    if (vg.missiles <= 0 || vg.fire_gap > 0) return;
    if (!vg.locked || vg.lock_target < 0) return;

    const Ship* s = &vg.enemy[vg.lock_target];
    if (!s->alive) return;

    // Alternate wing hardpoints so successive launches read as a pair.
    static int rail = 0;
    rail ^= 1;
    Vec3 origin = v3(rail ? 5.0f : -5.0f, -2.5f, 5.0f);

    vg_launch_missile(true, origin, vnorm(vsub(s->pos, origin)), vg.lock_target);
    vg.missiles--;
    vg.fire_gap = PLAYER_FIRE_GAP;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void vg_game_init(void) {
    memset(&vg, 0, sizeof(vg));
    vg_rng_seed(esp_random());

    vg_build_models();
    vg_build_starfield();
    vg_build_motes();

    // The attract loop flies the torus: a tunnel gives the title card far more
    // depth and motion than the inside of a sphere, where everything sits at a
    // uniform distance.
    vg_arena_init(ARENA_TORUS);
    vg_sky_init();
    vg_sky_generate((SkyKind)(esp_random() % (uint32_t)SKY_KINDS), esp_random());

    vg.state       = VG_ATTRACT;
    vg.health      = 1.0f;
    vg.throttle    = 0.45f;
    vg.speed       = SPEED_MIN;
    vg.difficulty  = 1.0f;
    vg.missiles    = PLAYER_MISSILES;
    vg.lock_target = -1;
}

void vg_game_start(void) {
    for (int i = 0; i < MAX_ENEMIES;   i++) vg.enemy[i].alive = false;
    for (int i = 0; i < MAX_MISSILES;  i++) vg.msl[i].alive   = false;
    for (int i = 0; i < MAX_ASTEROIDS; i++) vg.ast[i].alive   = false;
    for (int i = 0; i < MAX_DEBRIS;    i++) vg.deb[i].alive   = false;

    vg.state       = VG_PLAYING;
    vg.state_t     = 0;
    vg.score       = 0;
    vg.kills       = 0;
    vg.health      = 1.0f;
    vg.combat_t    = HEALTH_REGEN_DELAY;
    vg.difficulty  = 1.0f;
    vg.throttle    = 0.5f;
    vg.bank        = 0;
    vg.shake       = 0;
    vg.hit_flash   = 0;
    vg.missiles    = PLAYER_MISSILES;
    vg.reload_t    = PLAYER_RELOAD;
    vg.fire_gap    = 0;
    vg.lock_target = -1;
    vg.lock_t      = 0;
    vg.locked      = false;
    vg.spawn_t     = 0;

    // Torus only for now. A tunnel gives depth, a sense of place, and a line to
    // fly along; the inside of a sphere is uniform in every direction. The sphere
    // stays implemented in vg_arena.cpp for when there is a roster of maps.
    vg_arena_init(ARENA_TORUS);
    vg_sky_generate((SkyKind)(esp_random() % (uint32_t)SKY_KINDS), esp_random());
    vg.wall_clear = vg_arena_clearance(vg_arena_local_of(v3(0, 0, 0)));

    spawn_enemy(0);
    for (int i = 0; i < AST_TARGET_COUNT; i++) spawn_asteroid();

    vg_input_calibrate();
}

// ---------------------------------------------------------------------------
// World step
//
// Everything lives in VIEW SPACE: the player is nailed to the origin looking
// down +z, and the world rotates and translates around it. One counter-rotation
// per frame is applied to every star, mote, rock, ship, missile, trail point and
// fragment -- plus the arena and the backdrop.
// ---------------------------------------------------------------------------

static void world_step(float dt, float pitch_in, float yaw_in, float throttle_in) {
    float k = dt * THROTTLE_LERP;
    if (k > 1.0f) k = 1.0f;
    vg.throttle += (throttle_in - vg.throttle) * k;
    // An exponential lerp only ever approaches its target, so a slider held hard
    // against the stop settled at ~0.997 and top speed came out one unit short.
    // Snap the last sliver: reaching genuine maximum speed is the difference
    // between outrunning a missile and not.
    if (fabsf(throttle_in - vg.throttle) < 0.002f) vg.throttle = throttle_in;

    vg.speed = SPEED_MIN + (SPEED_MAX - SPEED_MIN) * vg.throttle;

    // Visual-only tracking of the same command, several times faster.
    float kv = dt * THROTTLE_VIS_LERP;
    if (kv > 1.0f) kv = 1.0f;
    vg.throttle_vis += (throttle_in - vg.throttle_vis) * kv;
    if (fabsf(throttle_in - vg.throttle_vis) < 0.002f) vg.throttle_vis = throttle_in;

    // Turn rate falls off with speed. Backing off tightens the turn; firewalling
    // it flattens you out. This is what makes the throttle a combat control.
    vg.agility = 1.0f + AGILITY_SLOW_BONUS * (1.0f - vg.throttle)
                      - AGILITY_FAST_MALUS * vg.throttle;

    float rate  = TURN_RATE * vg.agility;
    float yaw   = yaw_in   * rate * dt;
    float pitch = pitch_in * rate * dt;

    Mat3  R  = mat3_euler(-pitch, -yaw, 0.0f);
    float dz = vg.speed * dt;

    // The arena is static in the world, so it rides exactly the same transform.
    vg_arena_step(R, dz);
    vg.wall_clear = vg_arena_clearance(vg_arena_local_of(v3(0, 0, 0)));

    // The backdrop is at infinity, so only rotation moves it.
    vg_sky_step(pitch, yaw, vg.bank);

    float kb = dt * BANK_LERP;
    if (kb > 1.0f) kb = 1.0f;
    vg.bank += ((-yaw_in * BANK_MAX) - vg.bank) * kb;

    for (int i = 0; i < NUM_STARS; i++) vg.star[i] = mat3_apply(R, vg.star[i]);

    // Motes are ordinary static world points: rotate, translate, recycle once
    // they fall behind.
    for (int i = 0; i < NUM_MOTES; i++) {
        Vec3 m = mat3_apply(R, vg.mote[i]);
        m.z -= dz;
        if (m.z < MOTE_CULL_Z || vlen2(m) > 1700.0f * 1700.0f)
            m = vg_mote_spawn(MOTE_Z_MIN, MOTE_Z_MAX);
        vg.mote[i] = m;
    }

    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        Asteroid* a = &vg.ast[i];
        if (!a->alive) continue;
        a->pos = mat3_apply(R, a->pos);
        a->vel = mat3_apply(R, a->vel);
        a->pos = vadd(a->pos, vmul(a->vel, dt));
        a->pos.z -= dz;
        for (int s = 0; s < 3; s++) a->spin[s] += a->spin_rate[s] * dt;
        if (a->pos.z < CULL_Z_BEHIND || vlen2(a->pos) > CULL_RADIUS * CULL_RADIUS ||
            vg_arena_clearance(vg_arena_local_of(a->pos)) < -40.0f)
            a->alive = false;
    }

    for (int i = 0; i < MAX_ENEMIES; i++) {
        Ship* s = &vg.enemy[i];
        if (!s->alive) continue;
        s->pos = mat3_apply(R, s->pos);
        s->fwd = vnorm(mat3_apply(R, s->fwd));
        s->up  = vnorm(mat3_apply(R, s->up));
        s->pos = vadd(s->pos, vmul(s->fwd, s->speed * dt));
        s->pos.z -= dz;
        // Backstop: the AI steers away from the wall, but never let one escape
        // the world if it cuts a turn too fine.
        s->pos = vg_arena_clamp_inside(s->pos, ENEMY_HIT_RADIUS);
        if (vlen2(s->pos) > CULL_RADIUS * CULL_RADIUS) s->alive = false;
    }

    for (int i = 0; i < MAX_MISSILES; i++) {
        Missile* m = &vg.msl[i];
        if (!m->alive) continue;
        m->pos = mat3_apply(R, m->pos);
        m->dir = vnorm(mat3_apply(R, m->dir));
        m->pos.z -= dz;
        // The trail is world geometry too, so it has to ride the same transform
        // or it would smear behind the missile as you manoeuvre.
        for (int t = 0; t < m->trail_n; t++) {
            int idx = (m->trail_head - t + MISSILE_TRAIL * 2) % MISSILE_TRAIL;
            m->trail[idx] = mat3_apply(R, m->trail[idx]);
            m->trail[idx].z -= dz;
        }
    }

    for (int i = 0; i < MAX_DEBRIS; i++) {
        Debris* d = &vg.deb[i];
        if (!d->alive) continue;
        d->pos = mat3_apply(R, d->pos);
        d->vel = mat3_apply(R, d->vel);
        d->seg = mat3_apply(R, d->seg);
        d->pos = vadd(d->pos, vmul(d->vel, dt));
        d->pos.z -= dz;
        d->life -= dt;
        if (d->life <= 0) d->alive = false;
    }

    // Shake decays fast, and the offset is re-rolled each frame so it reads as
    // impact rather than as a smooth wobble.
    if (vg.shake > 0) {
        vg.shake -= dt * 2.6f;
        if (vg.shake < 0) vg.shake = 0;
        float amp = vg.shake * 13.0f;
        vg.shake_x = vg_frand(-amp, amp);
        vg.shake_y = vg_frand(-amp, amp);
    } else {
        vg.shake_x = vg.shake_y = 0;
    }

    if (vg.hit_flash > 0) vg.hit_flash -= dt;
}

// ---------------------------------------------------------------------------
// Threat and collisions
// ---------------------------------------------------------------------------

// Nearest live enemy missile tracking the player, for the threat warning.
static void update_threat(void) {
    vg.threat       = false;
    vg.threat_range = 1e9f;
    for (int i = 0; i < MAX_MISSILES; i++) {
        const Missile* m = &vg.msl[i];
        if (!m->alive || m->from_player || !m->locked) continue;
        float r = vlen(m->pos);
        if (r < vg.threat_range) {
            vg.threat_range = r;
            vg.threat_pos   = m->pos;
            vg.threat       = true;
        }
    }
}

static void collide_player(void) {
    // Boundary contact is fatal, so there is no bouncing the player back inside
    // any more -- the run is simply over.
    if (vg.wall_clear < SHIP_RADIUS) {
        vg_spawn_debris(v3(0, 0, 14), 26.0f, 16);
        vg_damage_player(DMG_WALL);
    }

    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        Asteroid* a = &vg.ast[i];
        if (!a->alive) continue;
        float r = a->radius + SHIP_RADIUS;
        if (vlen2(a->pos) < r * r) {
            vg_spawn_debris(a->pos, a->radius, 8);
            a->alive = false;
            vg_damage_player(DMG_ASTEROID);
        }
    }

    // Merging head-on has to cost something, or flying straight through them
    // becomes a free way to reverse the geometry.
    for (int i = 0; i < MAX_ENEMIES; i++) {
        Ship* s = &vg.enemy[i];
        if (!s->alive) continue;
        float r = ENEMY_HIT_RADIUS + SHIP_RADIUS;
        if (vlen2(s->pos) < r * r) {
            vg_spawn_debris(s->pos, 20.0f, 12);
            s->alive = false;
            vg_damage_player(DMG_RAM);
        }
    }
}

// ---------------------------------------------------------------------------
// Update
// ---------------------------------------------------------------------------

// Attract autopilot: a slow weave, plus a pull back toward the middle whenever
// the wall closes in. Without that second term it flies straight out through the
// side of the tunnel within a few seconds.
static void attract_autopilot(float t, float* pitch_in, float* yaw_in) {
    Vec3 want = v3(0.30f * sinf(t * 0.31f), 0.22f * sinf(t * 0.23f), 1.0f);

    Vec3  pl    = vg_arena_local_of(v3(0, 0, 0));
    float clear = vg_arena_clearance(pl);
    if (clear < ARENA_ATTRACT_MARGIN) {
        Vec3 inward = vg_arena_dir_to_view(vg_arena_inward(pl));
        want = vadd(want, vmul(inward,
                    2.2f * (ARENA_ATTRACT_MARGIN - clear) / ARENA_ATTRACT_MARGIN));
    }
    want = vnorm(want);

    // +yaw turns the nose right, +pitch drops it, hence the sign on y.
    float y =  want.x * 2.0f;
    float p = -want.y * 2.0f;
    if (y >  1.0f) y =  1.0f;
    if (y < -1.0f) y = -1.0f;
    if (p >  1.0f) p =  1.0f;
    if (p < -1.0f) p = -1.0f;
    *yaw_in = y;
    *pitch_in = p;
}

void vg_game_update(float dt, const VgInput* in) {
    vg.state_t += dt;

    vg.difficulty = 1.0f + (float)vg.kills * 0.35f;
    if (vg.difficulty > 4.0f) vg.difficulty = 4.0f;

    switch (vg.state) {

    case VG_ATTRACT: {
        float pitch_in, yaw_in;
        attract_autopilot(vg.state_t, &pitch_in, &yaw_in);
        world_step(dt, pitch_in, yaw_in, 0.42f);

        vg.spawn_t -= dt;
        if (vg.spawn_t <= 0) { spawn_asteroid(); vg.spawn_t = vg_frand(0.8f, 1.6f); }
        vg_update_missiles(dt);
        if (in->tap_edge) vg_game_start();
        break;
    }

    case VG_PLAYING:
    case VG_HIT: {
        const bool playing = (vg.state == VG_PLAYING);

        vg_clear_player_hit();

        world_step(dt, in->pitch, in->yaw, in->throttle);

        for (int i = 0; i < MAX_ENEMIES; i++) vg_update_enemy(&vg.enemy[i], i, dt);

        vg_update_missiles(dt);
        update_lock(dt);
        update_threat();

        // Being hunted counts as combat even before anything connects, so the
        // hull will not start knitting itself back together while a seeker is
        // still chasing you.
        vg.combat_t += dt;
        if (vg.threat && vg.threat_range < THREAT_COMBAT_RANGE) vg.combat_t = 0.0f;

        if (vg.combat_t > HEALTH_REGEN_DELAY &&
            vg.health >= HEALTH_REGEN_FLOOR && vg.health < 1.0f) {
            vg.health += HEALTH_REGEN_RATE * dt;
            if (vg.health > 1.0f) vg.health = 1.0f;
        }

        if (vg.fire_gap > 0) vg.fire_gap -= dt;
        if (vg.missiles < PLAYER_MISSILES) {
            vg.reload_t -= dt;
            if (vg.reload_t <= 0) { vg.missiles++; vg.reload_t = PLAYER_RELOAD; }
        }
        if (in->fire_edge) player_fire();

#if ENABLE_ASTEROIDS
        // Keep the field topped up as a speed cue.
        int alive_ast = 0;
        for (int i = 0; i < MAX_ASTEROIDS; i++) if (vg.ast[i].alive) alive_ast++;
        vg.spawn_t -= dt;
        if (alive_ast < AST_TARGET_COUNT && vg.spawn_t <= 0) {
            spawn_asteroid();
            vg.spawn_t = vg_frand(0.5f, 1.4f);
        }
#endif

        // Keep a fight going: replace losses, and add a wingman as it heats up.
        int alive_enemies = 0;
        for (int i = 0; i < MAX_ENEMIES; i++) if (vg.enemy[i].alive) alive_enemies++;
        int want = (vg.kills >= 3 && MAX_ENEMIES > 1) ? 2 : 1;
        if (alive_enemies < want) {
            for (int i = 0; i < MAX_ENEMIES; i++)
                if (!vg.enemy[i].alive) { spawn_enemy(i); break; }
        }

        if (playing) {
            collide_player();
            if (vg_player_was_hit()) {
                vg.state   = (vg.health > 0.0f) ? VG_HIT : VG_OVER;
                vg.state_t = 0;
            }
        } else if (vg.state_t > 1.2f) {
            vg.state   = VG_PLAYING;
            vg.state_t = 0;
        }
        break;
    }

    case VG_OVER: {
        world_step(dt, in->pitch * 0.3f, in->yaw * 0.3f, 0.35f);
        for (int i = 0; i < MAX_ENEMIES; i++) vg_update_enemy(&vg.enemy[i], i, dt);
        vg_update_missiles(dt);
        if (vg.state_t > 4.0f || (vg.state_t > 1.0f && in->tap_edge)) vg_game_start();
        break;
    }
    }
}
