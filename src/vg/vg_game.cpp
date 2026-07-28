#include "vg_sim.h"
#include "vg_arena.h"
#include "vg_sky.h"
#include "vg_tourney.h"
#include "vg_screens.h"
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

static void spawn_enemy(int i, ShipClass cls, float skill) {
    Ship* s = &vg.enemy[i];

    s->spec  = vg_spec(cls);
    s->skill = skill;

    // Out ahead but off-axis, so a fight opens with a merge rather than with
    // someone already on someone's tail.
    Vec3 dir = vnorm(v3(vg_frand(-0.7f, 0.7f), vg_frand(-0.5f, 0.5f), 1.0f));
    s->alive        = true;
    s->pos          = vg_arena_clamp_inside(
                          vmul(dir, ENEMY_SPAWN_DIST * vg_frand(0.8f, 1.25f)),
                          ARENA_SPAWN_MARGIN);
    s->fwd          = vnorm(vsub(v3(0, 0, 0), s->pos));   // pointed at the player
    s->up           = v3(0, 1, 0);
    s->speed        = (s->spec->speed_min + s->spec->speed_max) * 0.5f;
    s->target_speed = s->speed;
    s->hull         = s->spec->hull;
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
        if (range > vg.spec->lock_range || range < 1.0f) continue;
        float c = vdot(vnorm(s->pos), v3(0, 0, 1));   // player looks down +z
        if (c > best_c) { best_c = c; best = i; }
    }

    // Lock time scales with speed. This is what puts a fast ship out of effective
    // engagement range, and because it is re-evaluated every frame, accelerating
    // away also drops a lock you had already earned.
    float sn = (vg.speed - vg.spec->speed_min)
             / (vg.spec->speed_max - vg.spec->speed_min);
    if (sn < 0.0f) sn = 0.0f;
    if (sn > 1.0f) sn = 1.0f;
    vg.lock_need = vg.spec->lock_time * (1.0f + LOCK_SPEED_PENALTY * sn);

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

    vg_launch_missile(true, origin, vnorm(vsub(s->pos, origin)), vg.lock_target,
                      vg.spec);
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
    vg.credits     = CREDIT_START;
    vg.callsign[0] = 'A'; vg.callsign[1] = 'C'; vg.callsign[2] = 'E';
    vg.callsign[3] = 0;
    vg.trail_hue   = 0.52f;          // cyan by default: nothing else on screen is
    vg.ship        = SHIP_AEGIS;
    vg.spec        = vg_spec(vg.ship);
    vg.health_max  = vg.spec->hull;
    vg.health      = vg.health_max;
    vg.throttle    = 0.45f;
    vg.speed       = vg.spec->speed_min;
    vg.difficulty  = 1.0f;
    vg.missiles    = vg.spec->magazine;
    vg.lock_target = -1;
}

// Cycle the player's ship. Temporary: this is what the entry screen will do once
// callsign and trail colour join it, but flying all four is the only way to find
// out whether CHARIOT is actually fun, and that wants answering before any
// tournament code exists.
void vg_game_select_ship(ShipClass c) {
    vg.ship       = (c < SHIP_CLASSES) ? c : SHIP_AEGIS;
    vg.spec       = vg_spec(vg.ship);
    vg.health_max = vg.spec->hull;
    vg.health     = vg.health_max;
    vg.missiles   = vg.spec->magazine;
}

void vg_tournament_begin(ShipClass c) {
    vg.ship       = c;
    vg.spec       = vg_spec(c);
    // The only place hull is ever restored, until credits exist. Damage taken in
    // the round of 16 is still on the ship in the final.
    vg.health_max = vg.spec->hull;
    vg.health     = vg.health_max;
    vg.score      = 0;
    vg.kills      = 0;

    vg_tourney_begin(c);
}

void vg_match_start(void) {
    for (int i = 0; i < MAX_ENEMIES;   i++) vg.enemy[i].alive = false;
    for (int i = 0; i < MAX_MISSILES;  i++) vg.msl[i].alive   = false;
    for (int i = 0; i < MAX_ASTEROIDS; i++) vg.ast[i].alive   = false;
    for (int i = 0; i < MAX_DEBRIS;    i++) vg.deb[i].alive   = false;

    vg.state       = VG_PLAYING;
    vg.state_t     = 0;
    vg.spec        = vg_spec(vg.ship);
    vg.health_max  = vg.spec->hull;
    vg.difficulty  = 1.0f;
    vg.throttle    = 0.5f;
    vg.bank        = 0;
    vg.shake       = 0;
    vg.hit_flash   = 0;
    vg.missiles    = vg.spec->magazine;
    vg.reload_t    = vg.spec->reload;
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

    // One opponent, taken from the bracket. A match is strictly one on one --
    // the old "keep a fight going" respawn belonged to an endless survival mode
    // and would make a knockout round unwinnable.
    const Entrant* opp = vg_tourney_opponent();
    if (opp) spawn_enemy(0, opp->cls, ENEMY_SKILL * (0.75f + 0.35f * opp->rating));
    else     spawn_enemy(0, SHIP_AEGIS, ENEMY_SKILL);

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

    vg.speed = vg.spec->speed_min
             + (vg.spec->speed_max - vg.spec->speed_min) * vg.throttle;

    // Visual-only tracking of the same command, several times faster.
    float kv = dt * THROTTLE_VIS_LERP;
    if (kv > 1.0f) kv = 1.0f;
    vg.throttle_vis += (throttle_in - vg.throttle_vis) * kv;
    if (fabsf(throttle_in - vg.throttle_vis) < 0.002f) vg.throttle_vis = throttle_in;

    // Turn rate falls off with speed. Backing off tightens the turn; firewalling
    // it flattens you out. This is what makes the throttle a combat control.
    vg.agility = 1.0f + vg.spec->agility_slow_bonus * (1.0f - vg.throttle)
                      - vg.spec->agility_fast_malus * vg.throttle;

    float rate  = vg.spec->turn_rate * vg.agility;
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

// Purse for the round just won, plus a condition bonus scaled by hull remaining.
// Flying well therefore pays twice -- you earn more AND need less repair -- but
// the flat base dominates, so one scrappy match does not spiral the run.
//
// Called BEFORE vg_tourney_resolve, which is what advances the round counter.
static int s_last_purse = 0;

static void award_purse(void) {
    static const int BASE[TOURNEY_ROUNDS] = {
        CREDIT_R16, CREDIT_QF, CREDIT_SF, CREDIT_FINAL
    };
    const int r    = (vt.round < TOURNEY_ROUNDS) ? vt.round : TOURNEY_ROUNDS - 1;
    const int base = BASE[r];

    float cond = (vg.health_max > 0.0f) ? (vg.health / vg.health_max) : 0.0f;
    if (cond < 0.0f) cond = 0.0f;
    if (cond > 1.0f) cond = 1.0f;

    s_last_purse = base + (int)((float)base * CREDIT_CONDITION_K * cond + 0.5f);

    vg.credits += s_last_purse;
    if (vg.credits > CREDIT_CAP) vg.credits = CREDIT_CAP;
}

int vg_last_purse(void) { return s_last_purse; }

// Every menu state flies the same idle scene underneath, so the tournament map
// and the ship select sit over moving space rather than a dead background.
static void menu_world(float dt) {
    float pitch_in, yaw_in;
    attract_autopilot(vg.state_t, &pitch_in, &yaw_in);
    world_step(dt, pitch_in, yaw_in, 0.42f);

    vg.spawn_t -= dt;
    if (vg.spawn_t <= 0) { spawn_asteroid(); vg.spawn_t = vg_frand(0.8f, 1.6f); }
    vg_update_missiles(dt);
}

void vg_game_update(float dt, const VgInput* in) {
    vg.state_t += dt;

    vg.difficulty = 1.0f + (float)vg.kills * 0.35f;
    if (vg.difficulty > 4.0f) vg.difficulty = 4.0f;

    // A menu tap is a contact that lifts WITHOUT travelling. Resolved here
    // rather than in the input layer because the bracket needs one contact to
    // serve as both a pan drag and a button press, and only the consumer can
    // know which it turned out to be.
    static bool  s_held = false;
    static float s_press_x = 0, s_press_y = 0, s_travel = 0;
    bool  tap_up = false;
    float tap_x = 0, tap_y = 0;

    if (in->menu_edge) { s_press_x = in->menu_x; s_press_y = in->menu_y; s_travel = 0; }
    if (in->menu_held) s_travel += fabsf(in->menu_dx) + fabsf(in->menu_dy);
    if (s_held && !in->menu_held && s_travel < MENU_TAP_SLOP) {
        tap_up = true;
        tap_x  = s_press_x;
        tap_y  = s_press_y;
    }
    s_held = in->menu_held;

    switch (vg.state) {

    case VG_ATTRACT: {
        menu_world(dt);
        if (tap_up) {
            vg_entry_reset();
            vg.state   = VG_ENTRY;
            vg.state_t = 0;
        }
        break;
    }

    case VG_ENTRY: {
        menu_world(dt);
        if (vg_entry_update(in, tap_up, tap_x, tap_y)) {
            vg.state   = VG_SELECT;
            vg.state_t = 0;
        }
        break;
    }

    case VG_REPAIR: {
        menu_world(dt);
        if (vg_repair_update(in, tap_up, tap_x, tap_y)) {
            vg.state   = VG_BRACKET;
            vg.state_t = 0;
        }
        break;
    }

    case VG_SELECT: {
        menu_world(dt);
        // The +/- key cycles as well as the cards, since it is already wired and
        // is the fastest way to feel the difference between classes.
        if (in->alt_edge)
            vg_game_select_ship((ShipClass)((vg.ship + 1) % SHIP_CLASSES));
        if (tap_up) {
            int card = vg_select_card_at(tap_x, tap_y);
            if (card >= 0) {
                vg_game_select_ship((ShipClass)card);
            } else if (vg_select_confirm_at(tap_x, tap_y)) {
                vg_tournament_begin(vg.ship);
                vg_bracket_focus_player();
                vg.state   = VG_BRACKET;
                vg.state_t = 0;
            }
        }
        break;
    }

    case VG_BRACKET: {
        menu_world(dt);
        if (in->menu_held) vg_bracket_pan(in->menu_dx, in->menu_dy);
        if (tap_up) {
            if (vg_bracket_ready_at(tap_x, tap_y)) {
                vg_match_start();
            } else if (vg_bracket_repair_at(tap_x, tap_y)) {
                vg_repair_reset();
                vg.state   = VG_REPAIR;
                vg.state_t = 0;
            }
        }
        break;
    }

    case VG_PAUSE: {
        // No world step: paused means paused.
        if (in->alt_edge) { vg.state = VG_PLAYING; vg.state_t = 0; }
        if (tap_up) {
            if (vg_pause_resume_at(tap_x, tap_y)) {
                vg.state = VG_PLAYING;
                vg.state_t = 0;
            } else if (vg_pause_quit_at(tap_x, tap_y)) {
                vg.state = VG_ATTRACT;
                vg.state_t = 0;
            }
        }
        break;
    }

    case VG_ROUND_WON: {
        menu_world(dt);
        if (vg.state_t > 2.4f) {
            vg_tourney_resolve(true);
            if (vt.complete) {
                vg.state = VG_WON;
            } else {
                vg_bracket_focus_player();
                vg.state = VG_BRACKET;
            }
            vg.state_t = 0;
        }
        break;
    }

    case VG_WON: {
        menu_world(dt);
        if (vg.state_t > 1.5f && tap_up) { vg.state = VG_ATTRACT; vg.state_t = 0; }
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

        // No hull regeneration. Damage taken here is carried for the rest of the
        // tournament and only credits will undo it, which is what makes the
        // repair economy the difficulty curve rather than a side system.

        if (vg.fire_gap > 0) vg.fire_gap -= dt;
        if (vg.missiles < vg.spec->magazine) {
            vg.reload_t -= dt;
            if (vg.reload_t <= 0) { vg.missiles++; vg.reload_t = vg.spec->reload; }
        }
        if (in->fire_edge) player_fire();
        if (playing && in->alt_edge) { vg.state = VG_PAUSE; vg.state_t = 0; break; }

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

        if (playing) {
            collide_player();

            // Player death is resolved FIRST, which is what makes a mutual kill
            // a loss: you died, so you do not advance, regardless of whether the
            // opponent went down in the same frame.
            if (vg_player_was_hit()) {
                vg.state   = (vg.health > 0.0f) ? VG_HIT : VG_OVER;
                vg.state_t = 0;
                break;
            }

            bool opponent_alive = false;
            for (int i = 0; i < MAX_ENEMIES; i++)
                if (vg.enemy[i].alive) opponent_alive = true;
            if (!opponent_alive) {
                // Paid on the kill, not on the bracket advance, so the ROUND WON
                // card can show the purse -- and because vg_tourney_resolve is
                // what moves the round counter the payout reads off.
                award_purse();
                vg.state   = VG_ROUND_WON;
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
        // Knocked out is knocked out: back to the main menu, not a restart.
        if (vg.state_t > 1.2f && tap_up) { vg.state = VG_ATTRACT; vg.state_t = 0; }
        break;
    }
    }
}
