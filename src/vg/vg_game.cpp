#include "vg_sim.h"
#include "vg_arena.h"
#include "vg_sky.h"
#include "vg_tourney.h"
#include "vg_screens.h"
#include "vg_save.h"
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

// Fly a ship across the view for the launch cutscene. `u` runs 0..1 over the
// pass. It approaches from far out on one side and crosses close, which is the
// only way to read a wireframe silhouette -- held at distance it is four pixels
// and a trail, and held close it fills the screen before you can see its shape.
// A TRACKING shot, not a fly-past. The camera cannot be aimed -- the player is
// the origin, so there is nothing to aim -- but that cuts both ways: the ship's
// position is authored directly in view space, so holding it near the centre of
// frame IS the camera following it. What sells the movement is everything else,
// because the arena and the starfield keep sweeping behind a subject that does
// not.
//
// It is drawn many times its combat size. At ENEMY_SCALE a fighter is about
// five pixels across at any distance the near plane will allow, which is fine
// for a contact and useless for a subject.
static void cine_launch(const ShipSpec* spec, float hue, bool mirror) {
    Ship* c = &vg.cine;
    const float sx = mirror ? -1.0f : 1.0f;

    c->alive = true;
    c->spec  = spec;
    c->hue   = hue;
    c->scale = 128.0f;

    // Runs AWAY down the tunnel rather than across the view.
    //
    // Crossing laterally was wrong twice over. It read as a ship going sideways
    // past a corridor instead of flying along one, and it put the ship outside
    // the 62 degree field of view at both ends of the pass -- which is the
    // appearing and disappearing: launched at 68 degrees off-axis, invisible
    // until the camera had swung round to it, and gone again before the end.
    //
    // Departing keeps it in front by construction. It starts near, close to the
    // centre of frame, and recedes -- so it is always at a shallow angle and
    // never has to be chased into the corner of the screen.
    c->pos   = v3(sx * 250.0f, 80.0f, 540.0f);
    c->fwd   = vnorm(v3(sx * 0.26f, -0.04f, 1.0f));
    c->up    = v3(0, 1, 0);
    c->speed = 620.0f;

    c->roll_vis = sx * 0.5f;

    c->trail_n    = 0;
    c->trail_head = 0;
    c->trail_acc  = 0;
    vg.cine_on    = true;
}

// Move the whole shot somewhere else. The two fighters are launched from
// separate places, and cutting between them without relocating showed the same
// stretch of tunnel twice -- which reads as the pair starting on top of each
// other rather than a corridor apart.
//
// The camera is the origin and cannot be moved, so the world is moved instead:
// a long jump down the tube and a hard turn, which is indistinguishable from
// having travelled there.
static void cine_relocate(void) {
    const Mat3 R = mat3_euler(vg_frand(-0.55f, 0.55f), vg_frand(-1.4f, 1.4f), 0.0f);
    const float dz = vg_frand(2600.0f, 4400.0f);

    vg_arena_step(R, dz);
    vg.wall_clear = vg_arena_clearance(vg_arena_local_of(v3(0, 0, 0)));

    for (int i = 0; i < NUM_STARS; i++) vg.star[i] = mat3_apply(R, vg.star[i]);
    // Motes are near-field, so there is nothing to carry over -- re-seed them
    // rather than dragging the old dust to the new location.
    for (int i = 0; i < NUM_MOTES; i++) vg.mote[i] = vg_mote_spawn(MOTE_Z_MIN, MOTE_Z_MAX);

    // The backdrop is at infinity, so only the turn applies to it.
    vg_sky_step(vg_frand(-0.9f, 0.9f), vg_frand(-1.6f, 1.6f), vg.bank);
}

// Advance along its own track, and lay ribbon. Runs AFTER world_step, which has
// already turned both the ship and the background by this frame's camera pan.
static float s_cine_turn = 0.0f;   // lazy arc, set at launch

static void cine_fly(float dt) {
    Ship* c = &vg.cine;

    // A slow curving climb rather than a straight line. Nothing in this game
    // flies straight, and a ship holding a ruled course through a four second
    // shot reads as a model on a wire.
    const Mat3 T = mat3_euler(-0.13f * dt, s_cine_turn * dt, 0.0f);
    c->fwd = vnorm(mat3_apply(T, c->fwd));
    c->roll_vis += ((s_cine_turn > 0 ? 0.75f : -0.75f) - c->roll_vis) * dt * 0.8f;

    c->pos = vadd(c->pos, vmul(c->fwd, c->speed * dt));

    // Sampled twice as often as a combat trail: this one is crossing the frame
    // at 850 units a second, and at the normal rate the ribbon would be a row
    // of disconnected dashes rather than a streak.
    c->trail_acc += dt;
    if (c->trail_acc >= SHIP_TRAIL_DT * 0.5f) {
        c->trail_acc = 0;
        c->trail_head = (uint8_t)((c->trail_head + 1) % SHIP_TRAIL);
        // From the engine, not the hull centre -- at this model size the ribbon
        // would otherwise start somewhere inside the ship.
        c->trail[c->trail_head]   = vsub(c->pos, vmul(c->fwd, c->scale * 1.3f));
        c->trail_p[c->trail_head] = 255;    // full burn, so the colour carries
        if (c->trail_n < SHIP_TRAIL) c->trail_n++;
    }
}

static void cine_clear(void) {
    vg.cine_on          = false;
    vg.cine.trail_n     = 0;
    vg.cine.trail_head  = 0;
    vg.cine.trail_acc   = 0;
}

void vg_comms_say(const Ship* s, VoiceEvent ev) {
    if (!s) return;
    // VoiceEvent is ordered by weight -- taunt, fire, hurt, death -- so the
    // enum value doubles as the priority and a strict '>' means an equal event
    // still refreshes. Two hits in a row should read as two.
    if (vg.comms_t > 0.0f && vg.comms_pri > (uint8_t)ev) return;

    const uint32_t pick = (uint32_t)(vg_frand01() * 997.0f);

    // Once the name is yours, rivals sometimes stop taunting and start
    // recognising. Taunts only -- a pilot bleeding out does not pause to admire
    // your reputation, and their own last words matter more than your legend.
    if (ev == VOICE_TAUNT && vg.champion && (pick & 1u))
        vg.comms_line = vg_voice_champion_line(pick >> 1);
    else
        vg.comms_line = vg_voice_line(s->voice, ev, pick);
    vg.comms_tag[0] = s->tag[0];
    vg.comms_tag[1] = s->tag[1];
    vg.comms_tag[2] = s->tag[2];
    vg.comms_tag[3] = 0;
    vg.comms_pri = (uint8_t)ev;
    // A last transmission is left up far longer. It is the only line the player
    // cannot provoke a second time, and the round is already decided -- there is
    // nothing it can be competing with.
    vg.comms_t = (ev == VOICE_DEATH) ? KILL_SPEECH : 2.4f;
}

static void spawn_enemy(int i, ShipClass cls, float skill, float hue) {
    Ship* s = &vg.enemy[i];

    s->spec       = vg_spec(cls);
    s->skill      = skill;
    s->hue        = hue;
    s->scale      = ENEMY_SCALE;
    s->trail_acc  = 0;
    s->trail_n    = 0;
    s->trail_head = 0;

    // Anywhere along the tunnel, ahead or behind, rather than parked in front.
    // The z sign is a coin flip: half the time the match opens with an empty
    // scope and a contact somewhere back down the torus.
    Vec3 dir = vnorm(v3(vg_frand(-0.5f, 0.5f), vg_frand(-0.4f, 0.4f),
                        (vg_frand01() < 0.5f) ? 1.0f : -1.0f));
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
    //
    // VG_KILL is the same idea stretched: the round is decided and the loser is
    // mid-sentence, so a stray round still in the air must not be able to take
    // the win back after the fact.
    if (vg.state == VG_HIT || vg.state == VG_KILL) return;
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

    // Only spend the round if a missile actually left the rail. With every slot
    // in the air this silently charged the player for nothing at all, and since
    // no missile existed there was no outcome to report either -- a shot that
    // vanished in both directions.
    if (!vg_launch_missile(true, origin, vnorm(vsub(s->pos, origin)),
                           vg.lock_target, vg.spec))
        return;

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
    vg_sky_generate(SKY_MENU, esp_random());   // we boot straight into the menu

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

    // Last, so anything restored overwrites the defaults just set rather than
    // being overwritten by them.
    vg_save_load();
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

    // Identity is settled at this point -- callsign, colour and ship are locked
    // for the whole tournament, so this is the moment they are worth keeping.
    vg_save_store();
}

void vg_match_start(void) {
    for (int i = 0; i < MAX_ENEMIES;   i++) vg.enemy[i].alive = false;
    for (int i = 0; i < MAX_MISSILES;  i++) vg.msl[i].alive   = false;
    for (int i = 0; i < MAX_ASTEROIDS; i++) vg.ast[i].alive   = false;
    for (int i = 0; i < MAX_DEBRIS;    i++) vg.deb[i].alive   = false;

    vg.state       = VG_INTRO;
    vg.state_t     = 0;
    vg.roll        = 0;          // the menu leaves the world tumbling; fly level
    vg.bank        = 0;
    vg.cine_on     = false;
    vg.hud_boot    = 0;
    vg.msl_event   = MSL_NONE;
    vg.msl_event_t = 0;
    vg.trail_n     = 0;
    vg.trail_head  = 0;
    vg.trail_acc   = 0;
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
    // The venue. Generated here and then dissolved in across the cutscene, so
    // the match arrives somewhere rather than simply starting.
    vg_sky_generate((SkyKind)(esp_random() % (uint32_t)SKY_KINDS), esp_random());
    vg_sky_set_reveal(0.0f);
    vg.wall_clear = vg_arena_clearance(vg_arena_local_of(v3(0, 0, 0)));

    // One opponent, taken from the bracket. A match is strictly one on one --
    // the old "keep a fight going" respawn belonged to an endless survival mode
    // and would make a knockout round unwinnable.
    const Entrant* opp = vg_tourney_opponent();
    if (opp) {
        spawn_enemy(0, opp->cls,
                    ENEMY_SKILL * (0.75f + 0.35f * opp->rating), opp->hue);
        vg.enemy[0].voice = opp->voice;
        for (int i = 0; i < 4; i++) vg.enemy[0].tag[i] = opp->tag[i];
    } else {
        spawn_enemy(0, SHIP_AEGIS, ENEMY_SKILL, 0.02f);
    }

    // They open the match. The first thing you learn about an opponent should
    // be what kind of person they are, not what they are flying.
    vg.comms_line = nullptr;
    vg.comms_t    = 0;
    vg.comms_pri  = 0;
    vg.taunt_t    = 1.4f;

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

// `roll_in` is a rotation about the view axis for THIS frame, in radians. It
// goes into the world transform rather than into the camera, which is the whole
// difference between a tumble and a rotated picture: once it is in R, the next
// frame's pitch and yaw act in the rolled frame and the path actually
// corkscrews. Flight passes zero -- the player's roll stays cosmetic.
static void world_step(float dt, float pitch_in, float yaw_in, float roll_in,
                       float throttle_in) {
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

    vg.roll += roll_in;
    if (vg.roll >  6.28318531f) vg.roll -= 6.28318531f;
    if (vg.roll < -6.28318531f) vg.roll += 6.28318531f;

    Mat3  R  = mat3_euler(-pitch, -yaw, roll_in);
    float dz = vg.speed * dt;

    // The arena is static in the world, so it rides exactly the same transform.
    vg_arena_step(R, dz);
    vg.wall_clear = vg_arena_clearance(vg_arena_local_of(v3(0, 0, 0)));

    // The backdrop is at infinity, so only rotation moves it -- and it does NOT
    // ride R, so it has to be handed the total apparent roll by hand: the true
    // roll now baked into world positions, plus the cosmetic bank that the
    // projection will add on top. Miss either term and the nebula slides against
    // the starfield.
    vg_sky_step(pitch, yaw, vg.bank + vg.roll);

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

    // Trails are world geometry, so every stored point rides the same transform
    // the objects do -- otherwise a ribbon would smear sideways the moment you
    // manoeuvred instead of staying pinned to the track that was actually flown.
    vg.trail_acc += dt;
    for (int t = 0; t < vg.trail_n; t++) {
        int idx = (vg.trail_head - t + SHIP_TRAIL * 2) % SHIP_TRAIL;
        vg.trail[idx] = mat3_apply(R, vg.trail[idx]);
        vg.trail[idx].z -= dz;
    }
    if (vg.trail_acc >= SHIP_TRAIL_DT) {
        vg.trail_acc = 0;
        vg.trail_head = (uint8_t)((vg.trail_head + 1) % SHIP_TRAIL);
        // The player is nailed to the origin, so their track is seeded there and
        // is carried backwards by the transform above like everything else.
        vg.trail[vg.trail_head]   = v3(0, 0, 0);
        vg.trail_p[vg.trail_head] = (uint8_t)(vg.throttle * 255.0f);
        if (vg.trail_n < SHIP_TRAIL) vg.trail_n++;
    }

    // The cutscene ship rides the ROTATION but not the translation. During the
    // death sequence that holds the wreck at a fixed distance while the camera
    // turns around it, which is the whole shot -- applying dz as well would
    // simply leave it behind at the player's last cruising speed.
    if (vg.cine_on) {
        Ship* c = &vg.cine;
        c->pos = mat3_apply(R, c->pos);
        c->fwd = vnorm(mat3_apply(R, c->fwd));
        c->up  = vnorm(mat3_apply(R, c->up));
        // The ribbon has to ride the same rotation as the ship that laid it.
        // Missing this is why the cutscene ships appeared to emit nothing: the
        // camera pans at up to two radians a second during a pass, so within a
        // few frames the stored points were left pointing into an old view
        // frame -- swung behind the near plane and culled before they could be
        // drawn. The trail was always there; it was just no longer in front.
        for (int t = 0; t < c->trail_n; t++) {
            int idx = (c->trail_head - t + SHIP_TRAIL * 2) % SHIP_TRAIL;
            c->trail[idx] = mat3_apply(R, c->trail[idx]);
        }
    }

    for (int i = 0; i < MAX_ENEMIES; i++) {
        Ship* s = &vg.enemy[i];
        if (!s->alive) continue;
        s->pos = mat3_apply(R, s->pos);
        s->fwd = vnorm(mat3_apply(R, s->fwd));
        s->up  = vnorm(mat3_apply(R, s->up));
        s->pos = vadd(s->pos, vmul(s->fwd, s->speed * dt));
        s->pos.z -= dz;

        for (int t = 0; t < s->trail_n; t++) {
            int idx = (s->trail_head - t + SHIP_TRAIL * 2) % SHIP_TRAIL;
            s->trail[idx] = mat3_apply(R, s->trail[idx]);
            s->trail[idx].z -= dz;
        }
        s->trail_acc += dt;
        if (s->trail_acc >= SHIP_TRAIL_DT) {
            s->trail_acc = 0;
            s->trail_head = (uint8_t)((s->trail_head + 1) % SHIP_TRAIL);
            s->trail[s->trail_head] = s->pos;
            // Their throttle, read back out of their speed -- so an enemy
            // extending at full power streams exactly the way the player does.
            float tp = (s->speed - s->spec->speed_min)
                     / (s->spec->speed_max - s->spec->speed_min);
            if (tp < 0.0f) tp = 0.0f; else if (tp > 1.0f) tp = 1.0f;
            s->trail_p[s->trail_head] = (uint8_t)(tp * 255.0f);
            if (s->trail_n < SHIP_TRAIL) s->trail_n++;
        }
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
    if (vg.msl_event_t > 0) {
        vg.msl_event_t -= dt;
        if (vg.msl_event_t <= 0) {
            if (vg.msl_qn > 0) {
                vg.msl_event = vg.msl_queue[0];
                for (int i = 1; i < vg.msl_qn; i++) vg.msl_queue[i - 1] = vg.msl_queue[i];
                vg.msl_qn--;
                // Held briefly when more are stacked up, so a salvo reports
                // itself out promptly instead of trailing the fight.
                vg.msl_event_t = vg.msl_qn ? MSL_BANNER_FAST : MSL_BANNER;
            } else {
                vg.msl_event = MSL_NONE;
            }
        }
    }
    if (vg.hud_boot > 0) vg.hud_boot -= dt;
    if (vg.comms_t > 0) {
        vg.comms_t -= dt;
        if (vg.comms_t <= 0) { vg.comms_line = nullptr; vg.comms_pri = 0; }
    }
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
    // Barely a deviation at all -- just enough that the path is not a dead
    // straight line. Two terms per axis on periods with no common multiple, so
    // it drifts without ever visibly repeating.
    Vec3 want = v3(0.040f * sinf(t * 0.047f) + 0.019f * sinf(t * 0.019f),
                   0.034f * sinf(t * 0.038f) + 0.016f * sinf(t * 0.015f),
                   1.0f);

    // Ride the centreline of the tube. Clearance is at its maximum -- the tube
    // radius -- exactly on the central circle, so the shortfall from that IS
    // the off-centre distance, and steering inward in proportion to it settles
    // the camera onto the ring's axis and keeps it there.
    Vec3  pl     = vg_arena_local_of(v3(0, 0, 0));
    float clear  = vg_arena_clearance(pl);
    Vec3  inward = vg_arena_dir_to_view(vg_arena_inward(pl));

    float off = 1.0f - clear / ARENA_TORUS_RMIN;   // 0 dead centre, 1 at the wall
    if (off < 0.0f) off = 0.0f;
    if (off > 1.0f) off = 1.0f;

    // Position term alone is an undamped spring: it pulls toward the axis, sails
    // through it, and the camera ends up orbiting the centreline forever. That
    // slow circling was most of what read as the camera rocking.
    //
    // So damp it. We travel along +z in view space, so the inward direction's
    // own z component IS how fast we are already closing on the axis -- subtract
    // it and the approach settles instead of overshooting.
    float corr = off * off * 1.5f - inward.z * 1.1f;
    if (corr < -0.20f) corr = -0.20f;
    if (corr >  1.20f) corr =  1.20f;
    want = vadd(want, vmul(inward, corr));

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

    // Banked immediately. Losing a purse to a flat battery between the kill and
    // the next screen would be the single most annoying way to lose progress.
    vg_save_store();
}

int vg_last_purse(void) { return s_last_purse; }

// Returning to the title card has to take the finished run with it. Quitting
// from the pause menu and being knocked out both used to just set the state,
// leaving the loser's missiles and wreckage flying through the attract loop --
// and a missile whose seeker had broken draws in the dead-seeker grey, which is
// exactly the stray grey lines that were turning up on the menu.
// Out of combat the backdrop is always the menu one. Regenerating costs ~60ms,
// which is invisible at a screen change and is the price of not carrying a
// second full texture just to avoid it.
static void use_menu_sky(void) {
    vg_sky_generate(SKY_MENU, esp_random());
}

static void enter_attract(void) {
    use_menu_sky();
    for (int i = 0; i < MAX_ENEMIES;  i++) vg.enemy[i].alive = false;
    for (int i = 0; i < MAX_MISSILES; i++) vg.msl[i].alive   = false;
    for (int i = 0; i < MAX_DEBRIS;   i++) vg.deb[i].alive   = false;

    vg.trail_n     = 0;
    vg.trail_head  = 0;
    vg.trail_acc   = 0;
    vg.msl_event   = MSL_NONE;
    vg.msl_event_t = 0;
    vg.threat      = false;
    vg.lock_target = -1;
    vg.locked      = false;
    vg.hit_flash   = 0;
    vg.shake       = 0;

    vg.state   = VG_ATTRACT;
    vg.state_t = 0;
}

// Every menu state flies the same idle scene underneath, so the tournament map
// and the ship select sit over moving space rather than a dead background.
static void menu_world(float dt) {
    float pitch_in, yaw_in;
    attract_autopilot(vg.state_t, &pitch_in, &yaw_in);

    // Continuous roll rather than an oscillation. Something adrift does not rock
    // back to level -- it keeps going, slowly, and that is also what makes the
    // third axis unmistakable. About two minutes per revolution, breathing a
    // little so it never reads as a motor.
    const float roll_rate = 0.052f + 0.020f * sinf(vg.state_t * 0.023f);

    world_step(dt, pitch_in, yaw_in, roll_rate * dt, 0.30f);

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
#if VG_BENCH
        // Synthetic worst case: a full complement of fighters, all manoeuvring,
        // trailing and shooting, plus the player's own rack cycling. Reproduces
        // the primitive load of a heavy match without anyone touching the board.
        {
            int alive = 0;
            for (int i = 0; i < MAX_ENEMIES; i++) if (vg.enemy[i].alive) alive++;
            if (alive < MAX_ENEMIES) {
                for (int i = 0; i < MAX_ENEMIES; i++)
                    if (!vg.enemy[i].alive) {
                        spawn_enemy(i, (ShipClass)(i % SHIP_CLASSES), ENEMY_SKILL,
                                    0.1f + 0.45f * (float)i);
                        break;
                    }
            }
            for (int i = 0; i < MAX_ENEMIES; i++) vg_update_enemy(&vg.enemy[i], i, dt);
            update_lock(dt);
            if (vg.fire_gap > 0) vg.fire_gap -= dt;
            if (vg.missiles < vg.spec->magazine) {
                vg.reload_t -= dt;
                if (vg.reload_t <= 0) { vg.missiles++; vg.reload_t = vg.spec->reload; }
            }
            if (vg.locked) player_fire();
            vg.health = vg.health_max;      // never let the load generator "die"
        }
#endif
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

    case VG_INTRO: {
        const float t = vg.state_t;

        // The venue materialises during the opening drift, so the first shot is
        // of somewhere arriving rather than of somewhere already there.
        vg_sky_set_reveal(t / (INTRO_DRIFT * 0.85f));

        // Which shot we are in. Each cut re-anchors: the ship is launched fresh
        // from the opposite side on a new line, so the second setup reads as a
        // different camera in a different place rather than a repeat.
        static int s_shot = 0;
        const int  shot = (t > INTRO_OPP_START && t < INTRO_OPP_END) ? 2
                        : (t > INTRO_DRIFT     && t < INTRO_YOU_END) ? 1 : 0;

        if (shot != s_shot) {
            const bool was_first = (s_shot == 1);
            s_shot = shot;
            if (shot == 1) {
                s_cine_turn = 0.30f;
                cine_launch(vg.spec, vg.trail_hue, false);
            } else if (shot == 2) {
                // Only once, on the way in to the second setup.
                if (was_first || s_shot == 2) cine_relocate();
                s_cine_turn = -0.30f;
                cine_launch(vg.enemy[0].spec, vg.enemy[0].hue, true);
            }
            // Between shots the ribbon goes too, or the cut lands on a stranded
            // trail with nothing on the end of it.
            else cine_clear();
        }

        float pitch_in = 0.0f, yaw_in = 0.0f;
        if (vg.cine_on) {
            // Steer the view onto the ship. Gain is deliberately high so the
            // input saturates the moment it is more than about twenty degrees
            // off-axis -- from there the camera pans at its own maximum rate
            // and visibly lags through the closest approach, which is what an
            // operator swinging a long lens actually does.
            const Vec3 w = vnorm(vg.cine.pos);
            yaw_in   =  w.x * 3.2f;
            pitch_in = -w.y * 3.2f;
            if (yaw_in   >  1.0f) yaw_in   =  1.0f;
            if (yaw_in   < -1.0f) yaw_in   = -1.0f;
            if (pitch_in >  1.0f) pitch_in =  1.0f;
            if (pitch_in < -1.0f) pitch_in = -1.0f;
        } else {
            attract_autopilot(t, &pitch_in, &yaw_in);
        }

        // Throttle zero: the camera is anchored, not flying. world_step still
        // rotates the whole world by the pan, which is what carries the arena
        // and the starfield across behind the subject.
        world_step(dt, pitch_in, yaw_in, 0.0f, 0.0f);
        if (vg.cine_on) cine_fly(dt);

        if (t > INTRO_END || tap_up) {
            s_shot = 0;
            cine_clear();
            vg.state    = VG_PLAYING;
            vg.state_t  = 0;
            vg.hud_boot = HUD_BOOT_TIME;
            vg.roll     = 0;
            vg.bank     = 0;
            vg.taunt_t  = 1.6f;
            vg_input_calibrate();
        }
        break;
    }

    case VG_KILL: {
        // The world keeps running -- wreckage still tumbles, their last trail
        // still fades -- but nothing can touch the player. The only job of this
        // state is to let the dead pilot finish talking.
        world_step(dt, in->pitch, in->yaw, 0.0f, in->throttle);
        vg_update_missiles(dt);
        update_threat();
        if (vg.fire_gap > 0) vg.fire_gap -= dt;
        if (vg.state_t > KILL_BEAT) {
            award_purse();
            vg.state   = VG_ROUND_WON;
            vg.state_t = 0;
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
                enter_attract();
            }
        }
        break;
    }

    case VG_ROUND_WON: {
        menu_world(dt);
        if (vg.state_t > 2.4f) {
            vg_tourney_resolve(true);
            // Back out of combat, so back to the menu sky -- the bracket and
            // the repair screen are not a fight.
            use_menu_sky();
            if (vt.complete) {
                vg.champion = true;
                vg.state    = VG_WON;
                vg_save_store();      // the name sticks from here on
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
        // Returns on its own. The sequence ends by handing the player back to
        // the title card, where the crawl now says the rumour is about them --
        // so the payoff is not the win screen, it is the menu behind it having
        // quietly changed. A prompt would invite a tap that skips exactly that.
        //
        // Tapping is allowed only once the name is fully up, so an impatient
        // hand cannot cut the one moment the whole story was built toward.
        if (vg.state_t > WON_RETURN ||
            (vg.state_t > WON_NAME_IN + 2.6f && tap_up)) enter_attract();
        break;
    }

    case VG_PLAYING:
    case VG_HIT: {
        const bool playing = (vg.state == VG_PLAYING);

        vg_clear_player_hit();

        world_step(dt, in->pitch, in->yaw, 0.0f, in->throttle);

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

        // Unprompted chatter, on a long timer and only when the radio is idle.
        // Taunts are flavour; letting one interrupt a hit or a kill would turn
        // the most informative channel on the HUD into noise.
        vg.taunt_t -= dt;
        if (vg.taunt_t <= 0.0f) {
            vg.taunt_t = vg_frand(12.0f, 21.0f);
            if (vg.comms_t <= 0.0f) {
                for (int i = 0; i < MAX_ENEMIES; i++)
                    if (vg.enemy[i].alive) { vg_comms_say(&vg.enemy[i], VOICE_TAUNT); break; }
            }
        }

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
                if (vg.state == VG_OVER) {
                    // Your own ship, left drifting just ahead of the camera.
                    // There is no third-person view in a renderer where the
                    // player IS the origin, so the wreck is placed in front and
                    // the camera set tumbling around it -- which reads as being
                    // thrown clear, and gives the scene something to be about.
                    Ship* c = &vg.cine;
                    c->alive    = true;
                    c->spec     = vg.spec;
                    c->hue      = vg.trail_hue;
                    c->pos      = v3(vg_frand(-70.0f, 70.0f),
                                     vg_frand(-50.0f, 50.0f), 640.0f);
                    c->fwd      = vnorm(v3(0.35f, 0.12f, 1.0f));
                    c->up       = v3(0, 1, 0);
                    c->speed    = 0.0f;
                    c->scale    = 84.0f;
                    c->roll_vis = 0.4f;
                    c->trail_n  = 0;
                    c->hit_flash = 0.0f;
                    vg.cine_on  = true;
                    vg_spawn_debris(c->pos, 30.0f, 18);
                }
                break;
            }

            bool opponent_alive = false;
            for (int i = 0; i < MAX_ENEMIES; i++)
                if (vg.enemy[i].alive) opponent_alive = true;
            if (!opponent_alive) {
                // Not straight to the scorecard. They are still talking, and
                // cutting to a purse over the top of a dying pilot is the whole
                // difference between a tournament and a spreadsheet.
                vg.state   = VG_KILL;
                vg.state_t = 0;
            }
        } else if (vg.state_t > 1.2f) {
            vg.state   = VG_PLAYING;
            vg.state_t = 0;
        }
        break;
    }

    case VG_OVER: {
        // Dead men do not steer. Input is ignored entirely and the camera
        // tumbles on all three axes around the wreck it was thrown from --
        // slowly, and slowing further, like something that has stopped being a
        // ship and started being debris.
        const float decay = 1.0f / (1.0f + vg.state_t * 0.55f);
        world_step(dt,
                   0.16f * decay * sinf(vg.state_t * 0.63f),
                   0.21f * decay * sinf(vg.state_t * 0.41f + 1.1f),
                   0.30f * decay * dt,
                   0.0f);
        vg_update_missiles(dt);

        // Knocked out is knocked out: back to the main menu, not a restart.
        if (vg.state_t > 2.2f && tap_up) { cine_clear(); enter_attract(); }
        break;
    }
    }
}
