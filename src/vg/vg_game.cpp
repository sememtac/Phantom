#include "vg_sim.h"
#include "vg_shake.h"
#include "vg_arena.h"
#include "vg_sky.h"
#include "vg_tourney.h"
#include "vg_screens.h"
#include "vg_save.h"
#include "vg_cine.h"
#include "vg_ift.h"
#include "vg_course.h"
#include "vg_sfx.h"
#include <Arduino.h>
#include "vg_replay.h"
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

static void spawn_enemy(int i, ShipClass cls, float skill, float hue);

// Put the bracket's opponent into the world. Called twice: once at match setup
// so the cutscene has a ship to introduce, and again when the cutscene ends, by
// which point the viewpoint has drifted for twelve seconds and their position
// relative to the arena means nothing.
void vg_spawn_opponent(void) {
    const Entrant* opp = vg_tourney_opponent();
    if (opp) {
        spawn_enemy(0, opp->cls,
                    ENEMY_SKILL * (0.75f + 0.35f * opp->rating), opp->hue);
        vg.enemy[0].voice = opp->voice;
        for (int i = 0; i < 4; i++) vg.enemy[0].tag[i] = opp->tag[i];
    } else {
        spawn_enemy(0, SHIP_AEGIS, ENEMY_SKILL, 0.02f);
    }
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
    s->engaged      = false;
    s->kamikaze_will = (vg_frand01() < ENEMY_KAMIKAZE_CHANCE);
    s->kamikaze_on   = false;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void vg_game_init(void) {
    memset(&vg, 0, sizeof(vg));
    vg_rng_seed(vg_replay_rand());

    vg_build_models();
    vg_build_starfield();
    vg_build_motes();

    // The attract loop flies the torus: a tunnel gives the title card far more
    // depth and motion than the inside of a sphere, where everything sits at a
    // uniform distance.
    vg_arena_init(ARENA_TORUS);
    vg_sky_init();
    vg_sky_menu();   // we boot straight into the menu, and the menu has a sky

    // Set directly, and the only place left that does. This is not an arrival:
    // the sky was built four lines up, and vg_state_go would run enter_attract
    // and build a second one. That costs a draw from the seeded stream, which
    // is not a cosmetic difference -- it is every venue and every opponent in
    // the tournament shifting by one, and a recorded session no longer meaning
    // what it meant.
    vg.state       = VG_ATTRACT;
    vg.cam_zoom    = 1.0f;
    vg.credits     = CREDIT_START;
    vg.callsign[0] = 'A'; vg.callsign[1] = 'C'; vg.callsign[2] = 'E';
    vg.callsign[3] = 0;
    vg.trail_hue   = 0.52f;          // cyan by default: nothing else on screen is
    // Not full. A first run should be able to get louder as well as quieter, and
    // a mix that starts at the ceiling can only ever be turned down.
    vg.vol_music   = 0.70f;
    vg.vol_sfx     = 0.85f;
    vg.ship        = SHIP_AEGIS;
    vg.spec        = vg_spec(vg.ship);
    vg.health_max  = vg.spec->hull;
    vg.health      = vg.health_max;
    vg.throttle    = 0.45f;
    vg.speed       = vg.spec->speed_min;
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
    for (int i = 0; i < MAX_FIREBALLS; i++) vg.fire[i].alive  = false;

    vg.ring_alive  = false;      // no gate follows the player into a round
    // Every round is its own broadcast, so the announcer's one-shot flags clear
    // here rather than at boot. Clearing them at boot only would have introduced
    // the fighters once and then gone quiet for the rest of the tournament.
    vg.ift_fired   = 0;
    // Properly, queue included. This used to zero the timer and leave the
    // indices, which is the case vg_ift_queue's self-heal was written for.
    vg_ift_clear();
    vg.roll        = 0;          // the menu leaves the world tumbling; fly level
    vg.roll_rate   = 0;
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
    vg.throttle    = 0.5f;
    vg.bank        = 0;
    vg_shake_clear();
    vg.hit_flash   = 0;
    vg.missiles    = vg.spec->magazine;
    vg.reload_t    = 0.0f;          // a full rack is not reloading
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
    // Drawn into locals, in this order, deliberately. C++ does not define the
    // order arguments are evaluated in, and these two seeds are logged for
    // replay -- leaving it to the compiler would make the recording's meaning
    // depend on how it was built.
    const uint32_t venue_kind = vg_replay_rand();
    const uint32_t venue_seed = vg_replay_rand();
    vg_sky_generate((SkyKind)(venue_kind % (uint32_t)SKY_KINDS), venue_seed);
    vg_sky_set_reveal(0.0f);
    vg.wall_clear = vg_arena_clearance(vg_arena_local_of(v3(0, 0, 0)));

    // One opponent, taken from the bracket. A match is strictly one on one --
    // the old "keep a fight going" respawn belonged to an endless survival mode
    // and would make a knockout round unwinnable.
    vg_spawn_opponent();

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
// Collisions
// ---------------------------------------------------------------------------

static void collide_player(void) {
    // Boundary contact is fatal, so there is no bouncing the player back inside
    // any more -- the run is simply over.
    if (vg.wall_clear < SHIP_RADIUS) {
        // The player's own airframe against the boundary. Right on top of the
        // cockpit, so the flash vg_spawn_blast raises off the range is the
        // brightest one in the game -- which is correct for hitting a wall.
        vg_spawn_blast(v3(0, 0, 14), 34.0f, 7, 0, 1.7f);
        vg_spawn_shrapnel(v3(0, 0, 14), 26.0f, 40.0f, 30, 4.0f, 1.7f);
        vg_kill_player();
    }

    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        Asteroid* a = &vg.ast[i];
        if (!a->alive) continue;
        float r = a->radius + SHIP_RADIUS;
        if (vlen2(a->pos) < r * r) {
            vg_spawn_debris(a->pos, a->radius, 8);
            a->alive = false;
            vg_kill_player();
        }
    }

    // Merging head-on has to cost something, or flying straight through them
    // becomes a free way to reverse the geometry.
    for (int i = 0; i < MAX_ENEMIES; i++) {
        Ship* s = &vg.enemy[i];
        if (!s->alive) continue;
        float r = ENEMY_HIT_RADIUS + SHIP_RADIUS;
        if (vlen2(s->pos) < r * r) {
            // Say it BEFORE clearing alive. A pilot who dies in a collision gets
            // the same last line as one who dies to a missile; only the missile
            // path used to speak, so half the deaths in the game were silent.
            vg_comms_say(s, VOICE_DEATH);
            // A ram kills them too, and it used to be twelve shards and a radio
            // line. Same eruption a missile kill gets: the hull does not care
            // what opened it.
            vg_spawn_blast(s->pos, 46.0f, 9, 0, 1.9f);
            vg_spawn_shrapnel(s->pos, 30.0f, 54.0f, 34, 4.4f, 1.8f);
            s->alive = false;
            vg_kill_player();
        }
    }
}

// ---------------------------------------------------------------------------
// Update
// ---------------------------------------------------------------------------

// Attract autopilot: a slow weave, plus a pull back toward the middle whenever
// the wall closes in. Without that second term it flies straight out through the
// side of the tunnel within a few seconds.
void vg_attract_autopilot(float t, float* pitch_in, float* yaw_in) {
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
// Out of combat the backdrop is the menu's own: one fixed nebula, the same one
// every time, so the title screen is a place rather than a random wash. See
// vg_sky_menu -- it is free unless a venue has displaced it, so moving between
// the title, the bracket and the repair screen costs nothing.
static void use_menu_sky(void) {
    vg_sky_menu();
}

static void enter_attract(void) {
    use_menu_sky();
    for (int i = 0; i < MAX_ENEMIES;  i++) vg.enemy[i].alive = false;
    for (int i = 0; i < MAX_MISSILES; i++) vg.msl[i].alive   = false;
    for (int i = 0; i < MAX_DEBRIS;   i++) vg.deb[i].alive   = false;
    for (int i = 0; i < MAX_FIREBALLS; i++) vg.fire[i].alive = false;

    vg.trail_n     = 0;
    vg.trail_head  = 0;
    vg.trail_acc   = 0;
    vg.msl_event   = MSL_NONE;
    vg.msl_event_t = 0;
    vg.threat      = false;
    vg.lock_target = -1;
    vg.locked      = false;
    vg.hit_flash   = 0;
    vg_shake_clear();
}

// Every menu state flies the same idle scene underneath, so the tournament map
// and the ship select sit over moving space rather than a dead background.
static void menu_world(float dt) {
    // Nobody is holding the roll key on a menu, and the airframe buzz reads
    // vg.roll_rate whatever the state. Left alone, a player who backed out of the
    // course mid-roll would carry that rattle into the tournament map and keep it
    // there. The menu's own slow tumble below is a local, and unrelated.
    vg.roll_rate = 0.0f;

    float pitch_in, yaw_in;
    vg_attract_autopilot(vg.state_t, &pitch_in, &yaw_in);

    // Continuous roll rather than an oscillation. Something adrift does not rock
    // back to level -- it keeps going, slowly, and that is also what makes the
    // third axis unmistakable. About two minutes per revolution, breathing a
    // little so it never reads as a motor.
    const float roll_rate = 0.052f + 0.020f * sinf(vg.state_t * 0.023f);

    vg_world_step(dt, pitch_in, yaw_in, roll_rate * dt, 0.30f);

    vg.spawn_t -= dt;
    if (vg.spawn_t <= 0) { spawn_asteroid(); vg.spawn_t = vg_frand(0.8f, 1.6f); }
    vg_update_missiles(dt);
}

// ---------------------------------------------------------------------------
// The set turning on and off
// ---------------------------------------------------------------------------

static void enter_course(void) {
    vg_arena_init(ARENA_TORUS);
    vg.wall_clear = vg_arena_clearance(vg_arena_local_of(v3(0, 0, 0)));
    for (int i = 0; i < MAX_ENEMIES;  i++) vg.enemy[i].alive = false;
    for (int i = 0; i < MAX_MISSILES; i++) vg.msl[i].alive  = false;
    for (int i = 0; i < MAX_DEBRIS;   i++) vg.deb[i].alive  = false;
    for (int i = 0; i < MAX_FIREBALLS; i++) vg.fire[i].alive = false;
    // The course must ask for its own sky: coming from the menu there is no
    // backdrop at all, and the course is a place in the tournament's universe
    // rather than a void. Same three backdrops a match uses, drawn by the same
    // rule -- one rand, so the replay sequence keeps its length.
    const uint32_t sky_seed = vg_replay_rand();
    vg_sky_generate((SkyKind)(sky_seed % (uint32_t)SKY_KINDS), sky_seed);
    // Trim the field to the number a match flies with. The menu spawner has no
    // cap -- it tops up every second or so until all sixteen slots are full,
    // because nothing in a menu cares -- and the course inherited that field
    // wholesale. Sixteen close rocks was never a design decision; it was the
    // attract loop's housekeeping leaking into the one place that pays frame
    // time for it. The farthest go first, so what the player sees stays.
    {
        int alive = 0;
        for (int i = 0; i < MAX_ASTEROIDS; i++) if (vg.ast[i].alive) alive++;
        while (alive > AST_TARGET_COUNT) {
            int   far = -1;
            float d2  = -1.0f;
            for (int i = 0; i < MAX_ASTEROIDS; i++) {
                if (!vg.ast[i].alive) continue;
                const float d = vlen2(vg.ast[i].pos);
                if (d > d2) { d2 = d; far = i; }
            }
            vg.ast[far].alive = false;
            alive--;
        }
    }
    vg_course_begin();
    vg.roll      = 0;
    vg.roll_rate = 0;
    vg.bank      = 0;
    vg.hud_boot = HUD_BOOT_TIME;
    vg_sfx_play(SFX_READY, 1.0f);   // the panel finishing, not an event
    vg_input_calibrate();
}

// Arriving at the tournament table. Pulled out of the transition's switch so
// that it is the STATE's set-up and not one caller's: the table is reachable
// from the transition, from the repair screen and from the end of a round, and
// each of those used to prepare it differently.
static void enter_bracket(void) {
    // The table is a menu, so it gets the menu's backdrop -- it used to keep
    // whatever the last venue built.
    use_menu_sky();
    vg.ring_alive = false;
    vg_bracket_focus_player();
}

// ---------------------------------------------------------------------------
// THE STATES
//
// One row each: what a state is called, what it is, and what arriving at it
// sets up.
//
// There is no column for "arriving here cuts through the set". There was, and
// it was wrong: the tournament table is arrived at instantly from the repair
// screen and from the end of a round, and on a cut from the course and from the
// pause menu. A cut belongs to the edge, and the caller says so.
//
// The point of the table is not tidiness. It is that a state's properties are
// declared ONCE. They used to be spread over five hand-kept lists in four
// files, and the failure mode of a hand-kept list is not that it is wrong when
// written -- it is that adding a state silently leaves it right in four places
// and wrong in the fifth, with nothing to point at the one that was missed.
//
// `enter` is the set-up that arriving at the state requires, and it belongs to
// the STATE rather than to whoever sent us there. That distinction is not
// theoretical: the course reached VG_COURSE without ever generating a sky,
// because entering a state had no fixed meaning and each caller did whatever it
// remembered to. The backdrop was then whatever the last scene had built.
//
// The names are duplicated in vg_crumb.cpp, deliberately -- see the note there.
// VG_STATE_COUNT is what keeps the two the same length.
struct VgStateDef {
    const char* name;
    uint8_t     flags;
    void      (*enter)(void); // may be null: not every state sets anything up
};

// In enum order. Positional, like the crumb table, so the two read the same way
// side by side.
static const VgStateDef STATES[VG_STATE_COUNT] = {
    { "ATTRACT",   VGS_MENU,                           enter_attract   },
    { "ENTRY",     VGS_MENU,                           nullptr         },
    { "SELECT",    VGS_MENU,                           nullptr         },
    { "REPAIR",    VGS_MENU,                           nullptr         },
    { "BRACKET",   VGS_MENU,                           enter_bracket   },
    { "INTRO",     VGS_MENU,                           vg_match_start  },
    { "PLAYING",   VGS_LIVE | VGS_ENGINE | VGS_COMBAT, nullptr         },
    { "HIT",       VGS_LIVE | VGS_ENGINE | VGS_COMBAT, nullptr         },
    // Still flying, and that is the whole of it: the opponent is down and
    // talking, the player cannot be hurt, and cutting the hum at that moment
    // would be the loudest thing about it.
    { "KILL",      VGS_ENGINE,                         nullptr         },
    // Nothing. A pause is not a place -- it suspends one.
    { "PAUSE",     0,                                  nullptr         },
    { "COURSE",    VGS_LIVE | VGS_ENGINE,              enter_course    },
    { "ROUND_WON", VGS_MENU,                           nullptr         },
    { "OVER",      VGS_MENU,                           nullptr         },
    { "WON",       VGS_MENU,                           nullptr         },
};

static_assert(sizeof(STATES) / sizeof(STATES[0]) == VG_STATE_COUNT,
              "a state was added without a row, or a row without a state");
static_assert((int)VG_WON + 1 == VG_STATE_COUNT,
              "VG_STATE_COUNT does not match the enum -- vg_crumb.cpp's copy "
              "of the names is the same length and would now be misaligned");

uint8_t vg_state_flags(VgState s) {
    return ((int)s < VG_STATE_COUNT) ? STATES[s].flags : 0u;
}

void vg_state_go(VgState to) {
    if ((int)to >= VG_STATE_COUNT) return;
    const VgStateDef* d = &STATES[to];

    // The clock belongs to the state, not to the caller. It was reset by hand at
    // all nineteen sites that changed state, and it happened to be right at all
    // nineteen -- which is the good version of a rule that nothing enforces.
    vg.state   = to;
    vg.state_t = 0.0f;
    if (d->enter) d->enter();
}

void vg_state_resume(VgState to) {
    if ((int)to >= VG_STATE_COUNT) return;
    vg.state   = to;
    vg.state_t = 0.0f;
}

void vg_state_cut(VgState to) {
    if (vg.tv_phase != TV_NONE) return;   // one transition at a time
    // THE SHIP IS OFF BEFORE THE PICTURE IS. Gating the per-frame sources below
    // stops them being asked for again, but it cannot retract what is already
    // sounding -- an alert beep, a hull hit, the tail of a transmission -- and
    // those carried on over the black. The set going off is the end of the
    // session, so it takes everything with it and then makes its own noise.
    vg_sfx_silence();
    // AND THE BROADCAST IS OFF THE AIR. Same rule as the audio and for the same
    // reason: a cut ends the session, and an announcement is part of one. Skip
    // out of the course while the announcer is mid-line and the line used to
    // ride the transition and finish over the tournament table -- a system
    // message about a course the player has just left.
    //
    // The CUT, not every change of state. IFT_MATCH_END is posted over the
    // wreck and is meant to run on through the redraw, and that path is a
    // vg_state_go rather than a cut. The set never goes off, so the announcer
    // never does either.
    vg_ift_clear();
    vg_sfx_play(SFX_TV_OFF, 1.0f);
    vg.tv_phase = TV_OUT;
    vg.tv_to    = (uint8_t)to;
    vg.tv_t     = 0.0f;
}

// Runs at the JOIN, with the screen black. The set-up a state needs happens here
// rather than at the button, so the old scene is never the one the aperture
// opens back onto.
//
// It used to be a switch with a case for each destination, each calling that
// destination's set-up. There is nothing left to switch on: the far side of a
// cut is a state, and entering a state is one thing.
static void tv_join(void) {
    vg_state_go((VgState)vg.tv_to);
}

static void tv_update(float dt) {
    if (vg.tv_phase == TV_NONE) return;
    vg.tv_t += dt;

    if (vg.tv_phase == TV_OUT) {
        if (vg.tv_t >= TV_OUT_TIME) { vg.tv_phase = TV_HOLD; vg.tv_t = 0.0f; }
        return;
    }
    if (vg.tv_phase == TV_HOLD) {
        // THE SWITCH HAPPENS AT THE END OF THE DEAD AIR, not the start of it.
        //
        // Joining at the start would let the new scene run for a whole second
        // behind a black screen -- and the first thing a scene does is start
        // talking. The opening line of the ring course would have been a third
        // spent before the picture existed to show it, which is a mistake already
        // made once in this file's history and not worth making twice.
        if (vg.tv_t >= TV_HOLD_TIME) {
            tv_join();
            vg.tv_phase = TV_IN;
            vg.tv_t     = 0.0f;
            // With the picture, not before it: the thump and the dot are the
            // same moment, and hearing it during the dead air would put the
            // sound of the set striking over a screen that is still black.
            vg_sfx_play(SFX_TV_ON, 1.0f);
        }
        return;
    }
    if (vg.tv_t >= TV_IN_TIME) {
        vg.tv_phase = TV_NONE;
        vg.tv_t     = 0.0f;
    }
}

// The caution annunciators: how close the thing is, how fast that should blink,
// and the beep that goes with each time it lights.
//
// HERE, not in the HUD, for two reasons. The phase has to be advanced by dt and
// the draw does not have one; and a sound triggered from drawing code is a sound
// that does not happen when the panel is not drawn, which is a very strange rule
// for a warning.
//
// `k` is 0 at the far edge of the alert's range and 1 at the thing itself, so
// THE RATE IS THE RANGE: a player who cannot spare attention for a number can
// still feel a rhythm getting faster. Both alerts share this because they now
// behave identically -- the missile alert had its own double-beat shape and it
// read as a flicker.
static void alert_step(float* phase, bool* lit, bool active, float k, float dt,
                       SfxId cue) {
    if (!active) { *phase = 0.0f; *lit = false; return; }

    if (k < 0.0f) k = 0.0f; else if (k > 1.0f) k = 1.0f;
    const float period = ALERT_FLASH_SLOW
                       + (ALERT_FLASH_FAST - ALERT_FLASH_SLOW) * k;

    // Advanced, never sampled. The rate follows the period smoothly and the
    // phase cannot jump when the period changes.
    *phase += dt / period;
    if (*phase >= 1.0f) *phase -= (float)(int)*phase;

    const bool now = (*phase <= ALERT_FLASH_DUTY);
    if (now && !*lit) vg_sfx_play(cue, 1.0f);   // on the lit edge, once
    *lit = now;
}

static void update_alerts(float dt, bool alive) {
    if (!alive) {
        vg.alert_msl_ph = vg.alert_wall_ph = 0.0f;
        vg.alert_msl_lit = vg.alert_wall_lit = false;
        return;
    }
    const bool msl = vg.threat && vg.threat_range <= MSL_ALERT_RANGE;
    alert_step(&vg.alert_msl_ph, &vg.alert_msl_lit, msl,
               msl ? (1.0f - vg.threat_range / MSL_ALERT_RANGE) : 0.0f,
               dt, SFX_MSL_ALERT);

    const bool wall = (vg.wall_clear <= ARENA_ALERT_RANGE);
    alert_step(&vg.alert_wall_ph, &vg.alert_wall_lit, wall,
               wall ? (1.0f - vg.wall_clear / ARENA_ALERT_RANGE) : 0.0f,
               dt, SFX_WALL_ALERT);
}

void vg_game_update(float dt, const VgInput* in) {
    tv_update(dt);

    // A press during the wipe belongs to neither scene. Without this a tap that
    // started the transition is still live when the new screen appears and gets
    // spent on whatever happens to be under the finger.
    VgInput gated;
    if (vg.tv_phase != TV_NONE) {
        gated = *in;
        gated.fire_edge = gated.alt_edge = gated.tap_edge = gated.menu_edge = false;
        in = &gated;
    }

    vg.state_t += dt;

    // ---- ship systems, and whether there is a ship ------------------------
    //
    // A DEAD PILOT'S PANEL IS OFF. The alerts used to run in every state,
    // including the wreck screen and the menus behind it, so a boundary warning
    // the player could do nothing about followed them out of the match and kept
    // beeping over the title card. Whatever the threat was, it stopped being
    // theirs the moment they died.
    //
    // A TRANSITION ENDS THE SESSION. The state does not change until the join,
    // which is halfway through the wipe, so without this the alerts and the hum
    // ran the whole way out into the black and only stopped once the next scene
    // had already been built. Nothing flies during a transition.
    const uint8_t sf  = vg_state_flags(vg.state);
    const bool alive  = (vg.tv_phase == TV_NONE) && (sf & VGS_LIVE);
    // The engine runs a little wider: through VG_KILL the ship is still flying,
    // and cutting the hum the instant the opponent dies would be the loudest
    // thing about that moment.
    const bool flying = (vg.tv_phase == TV_NONE) && (sf & VGS_ENGINE);

    // Looking aft is a look and nothing else -- it takes no input away from the
    // ship and changes no state the simulation reads. Only while there is a
    // cockpit to look out of, and never during a transition, or the last frame
    // before the wipe would be shot from the wrong way round.
    vg.rear_view = alive && in->rear_held;

    update_alerts(alive ? dt : 0.0f, alive);
    vg_sfx_engine(flying, vg.throttle_vis);

    // SOMEBODY HAS DIED. One sound for it, wherever it happens: over the radio
    // when it is the opponent, and over the wreck screen when it is the player.
    // A tournament that kills people should use the same note for it both ways --
    // it is the one thing in the game that means exactly the same whichever side
    // of it you are on.
    //
    // Outlasts the last transmission rather than ending with it: the loser stops
    // talking and the tone is still there, which is the silence doing the work.
    // On the player's own death it simply does not stop until they tap away.
    vg_sfx_flatline(vg.tv_phase == TV_NONE
                 && ((vg.state == VG_KILL && vg.state_t < KILL_SPEECH + 1.2f)
                  || (vg.state == VG_OVER)));

    // ---- PAUSE, from anywhere that flies ----------------------------------
    //
    // Handled HERE, ahead of the state machine, because it is one rule and not a
    // rule per state. It used to be written out in each case that wanted it, and
    // the copy in the match required `playing` -- so the key did nothing in
    // VG_HIT, which is exactly where a player being shot at reaches for it.
    // Anything that flies can be paused, and pause always returns where it came
    // from.
    if (in->pwr_edge) {
        if (vg.state == VG_PAUSE) {
            // One key, one meaning: "take me back one". From a sub-page that is
            // the menu, not the game -- unpausing out of a page the player is
            // still reading would skip a step they did not ask to skip.
            if (vg.pause_page) {
                vg.pause_page = 0;
            } else {
                vg_state_resume((vg.pause_from == VG_COURSE) ? VG_COURSE
                                                            : VG_PLAYING);
            }
        } else if (flying) {
            vg.pause_from = (vg.state == VG_COURSE) ? VG_COURSE : VG_PLAYING;
            vg.pause_page = 0;
            vg_state_go(VG_PAUSE);
        }
    }

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
            vg_update_lock(dt);
            if (vg.fire_gap > 0) vg.fire_gap -= dt;
            vg_update_reload(dt);
            if (vg.locked) vg_player_fire();
            vg.health = vg.health_max;      // never let the load generator "die"
        }
#endif
        if (tap_up) {
            vg_entry_reset();
            vg_state_go(VG_ENTRY);
        }
        break;
    }

    case VG_ENTRY: {
        menu_world(dt);
        if (vg_entry_update(in, tap_up, tap_x, tap_y)) {
            vg_state_go(VG_SELECT);
        }
        break;
    }

    case VG_REPAIR: {
        menu_world(dt);
        if (vg_repair_update(in, tap_up, tap_x, tap_y)) vg_state_go(VG_BRACKET);
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
                // The draw is made HERE, so the course that follows has a
                // tournament to return to and the player flies it in the airframe
                // they actually picked.
                vg_tournament_begin(vg.ship);
                vg_bracket_focus_player();
                vg_state_cut(VG_COURSE);
            }
        }
        break;
    }

    case VG_INTRO: {
        // The shot itself lives in vg_cine.cpp. What belongs here is only the
        // handover: the cutscene has spent seventeen seconds turning and
        // drifting the viewpoint for the sake of framing, and relocating
        // between setups moved it again -- so where it ended up relative to the
        // arena is not something a match should have to inherit. Both the arena
        // and the opponent are re-established from scratch, which makes a bad
        // start structurally impossible rather than merely unlikely.
        //
        // Free to do because the handover is a hard cut to black with the
        // instruments rebooting over it. None of the snap is visible.
        // The broadcast introduces each fighter over its own shot. The cutscene
        // already hard-cuts between them, so the cues are the shot boundaries.
        if (vg.state_t > INTRO_DRIFT && !(vg.ift_fired & (1u << IFT_INTRO_YOU))) {
            vg.ift_fired |= (1u << IFT_INTRO_YOU);
            vg_ift_line(IFT_INTRO_YOU);
        }
        if (vg.state_t > INTRO_OPP_START && !(vg.ift_fired & (1u << IFT_INTRO_OPP))) {
            vg.ift_fired |= (1u << IFT_INTRO_OPP);
            vg_ift_line(IFT_INTRO_OPP);
        }

        if (vg_cine_update(dt, tap_up)) {
            vg_cine_clear();
            vg.cam_zoom = 1.0f;              // the cockpit is never zoomed
            vg_arena_init(ARENA_TORUS);
            vg.wall_clear = vg_arena_clearance(vg_arena_local_of(v3(0, 0, 0)));
            for (int i = 0; i < MAX_MISSILES; i++) vg.msl[i].alive = false;
            for (int i = 0; i < MAX_DEBRIS;   i++) vg.deb[i].alive = false;
            for (int i = 0; i < MAX_FIREBALLS; i++) vg.fire[i].alive = false;
            vg_spawn_opponent();

            vg_state_go(VG_PLAYING);
            vg.hud_boot = HUD_BOOT_TIME;
    vg_sfx_play(SFX_READY, 1.0f);   // the panel finishing, not an event
            vg.roll     = 0;
            vg.bank     = 0;
            vg.taunt_t  = 1.6f;
            vg_input_calibrate();
        }
        break;
    }

    case VG_COURSE: {
        // Flying, and nothing else. No opponent, no missiles, no purse. The wall
        // is still lethal because that is one of the things worth learning here.
        vg_world_step(dt, in->pitch, in->yaw, vg_roll_angle(in, dt), in->throttle);
        vg_course_update(dt);
        collide_player();

        if (vg_player_was_hit()) {
            // A crash costs the streak and nothing else. Full hull, back in the
            // tube, count at zero -- this is a practice range, and ending a
            // tournament that has not started would be absurd.
            vg_clear_player_hit();
            vg.health      = vg.health_max;
            vg.course_hits = 0;
            vg_arena_init(ARENA_TORUS);
            vg.wall_clear  = vg_arena_clearance(vg_arena_local_of(v3(0, 0, 0)));
            vg_ift_line(IFT_COURSE_MISS);
            vg_course_reset_streak();
        }

        // Finishing is a MOMENT, not an exit condition. The gate is already gone
        // and the player keeps flying while the broadcast marks it, exactly as a
        // kill does -- see COURSE_DONE_BEAT.
        if (vg.course_done) {
            vg.course_end_t += dt;
            if (vg.course_end_t > COURSE_DONE_BEAT) vg_state_cut(VG_BRACKET);
        }


        break;
    }

    case VG_KILL: {
        // The world keeps running -- wreckage still tumbles, their last trail
        // still fades -- but nothing can touch the player. The only job of this
        // state is to let the dead pilot finish talking.
        vg_world_step(dt, in->pitch, in->yaw, vg_roll_angle(in, dt), in->throttle);
        vg_update_missiles(dt);
        vg_update_threat();
        if (vg.fire_gap > 0) vg.fire_gap -= dt;
        // After the last transmission, not over it. KILL_SPEECH is exactly how
        // long the dying pilot holds the other slot, so this lands in the silence
        // that follows and runs on into the bracket redraw.
        if (vg.state_t > KILL_SPEECH && !(vg.ift_fired & (1u << IFT_MATCH_END))) {
            vg.ift_fired |= (1u << IFT_MATCH_END);
            vg_ift_line(IFT_MATCH_END);
        }
        if (vg.state_t > KILL_BEAT) {
            award_purse();
            vg_state_go(VG_ROUND_WON);
        }
        break;
    }

    case VG_BRACKET: {
        menu_world(dt);
        if (in->menu_held) vg_bracket_pan(in->menu_dx, in->menu_dy);
        if (tap_up) {
            if (vg_bracket_ready_at(tap_x, tap_y)) {
                vg_state_cut(VG_INTRO);
            } else if (vg_bracket_course_at(tap_x, tap_y)) {
                vg_state_cut(VG_COURSE);
            } else if (vg_bracket_repair_at(tap_x, tap_y)) {
                vg_repair_reset();
                vg_state_go(VG_REPAIR);
            }
        }
        break;
    }

    case VG_PAUSE: {
        // No world step: paused means paused.
        const bool from_course = (vg.pause_from == VG_COURSE);
        const VgState back     = from_course ? VG_COURSE : VG_PLAYING;
        (void)dt;

        // PWR again, which is what paused it. NOT the + key -- that is the roll
        // control, and a paused player leaning on it would be thrown back into
        // the fight mid-roll.
        if (vg.pause_page == 1) {
            // Dragged, not tapped: held rather than on release, so the fill
            // follows the finger instead of jumping when it lifts.
            if (in->menu_held) {
                if (vg_pause_music_at(in->menu_x, in->menu_y))
                    vg.vol_music = vg_pause_slider_value(in->menu_x);
                else if (vg_pause_sfx_at(in->menu_x, in->menu_y))
                    vg.vol_sfx = vg_pause_slider_value(in->menu_x);
            }
            if (tap_up && vg_pause_back_at(tap_x, tap_y)) {
                vg.pause_page = 0;
                vg_save_store();        // settings outlive the session
            }
            break;
        }

        if (tap_up) {
            switch (vg_pause_item_at(tap_x, tap_y, from_course)) {
            case PAUSE_RESUME:
                vg_state_resume(back);
                break;
            case PAUSE_AUDIO:
                vg.pause_page = 1;
                break;
            case PAUSE_SKIP:
                // Refused while the briefing runs: the player may pause over it,
                // read it and think about it, but not walk out of it.
                if (vg.course_named) vg_state_cut(VG_BRACKET);
                break;
            case PAUSE_QUIT:
                vg_state_cut(VG_ATTRACT);
                break;
            default: break;
            }
        }
        break;
    }

    case VG_ROUND_WON: {
        menu_world(dt);
        if (vg.state_t > 2.4f) {
            vg_tourney_resolve(true);
            if (vt.complete) {
                vg.champion = true;
                // Back out of combat, so back to the menu sky. The bracket
                // gets this from enter_bracket; the winner's card does not
                // pass through it.
                use_menu_sky();
                vg_state_go(VG_WON);
                vg_save_store();      // the name sticks from here on
            } else {
                vg_state_go(VG_BRACKET);
            }
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

        vg_world_step(dt, in->pitch, in->yaw, vg_roll_angle(in, dt), in->throttle);

        for (int i = 0; i < MAX_ENEMIES; i++) vg_update_enemy(&vg.enemy[i], i, dt);
        // After they have moved, or the range being tested is a frame stale --
        // which at a combined 800 units a second is sixteen units of error.
        vg_update_passes();

        vg_update_missiles(dt);
        vg_update_lock(dt);
        vg_update_threat();

        // No hull regeneration. Damage taken here is carried for the rest of the
        // tournament and only credits will undo it, which is what makes the
        // repair economy the difficulty curve rather than a side system.

        if (vg.fire_gap > 0) vg.fire_gap -= dt;
        vg_update_reload(dt);
        if (in->fire_edge) vg_player_fire();

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
                vg_state_go((vg.health > 0.0f) ? VG_HIT : VG_OVER);
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
                    // The player's own wreck, and the biggest eruption in the
                    // game. Scaled off c->scale (84) rather than a ship's own
                    // size: this one is deliberately staged large and close, and
                    // an explosion tuned for a distant fighter looked like a
                    // spark next to it.
                    vg_spawn_blast(c->pos, 62.0f, 11, 0, 2.2f);
                    vg_spawn_shrapnel(c->pos, 40.0f, 72.0f, 44, 4.8f, 2.0f);
                }
                break;
            }

            bool opponent_alive = false;
            bool opponent_met   = false;
            for (int i = 0; i < MAX_ENEMIES; i++) {
                if (vg.enemy[i].alive) opponent_alive = true;
                if (vg.enemy[i].engaged) opponent_met = true;
            }

            // An opponent who dies without ever reaching the player does not end
            // the match. Send the next one instead.
            //
            // Nothing should reach this now: the distance cull that deleted them
            // is gone, and the only remaining ways to die are a missile and a
            // collision, both of which require the two ships to be together. It
            // stays because the failure it prevents is the worst kind -- a
            // tournament round won without a fight, with no wreck and nobody
            // saying anything, which reads as a broken game rather than a
            // lucky one.
            if (!opponent_alive && !opponent_met) {
                vg_spawn_opponent();
                opponent_alive = true;
            }

            if (!opponent_alive) {
                // Not straight to the scorecard. They are still talking, and
                // cutting to a purse over the top of a dying pilot is the whole
                // difference between a tournament and a spreadsheet.
                vg_state_go(VG_KILL);
            }
        } else if (vg.state_t > 1.2f) {
            vg_state_go(VG_PLAYING);
        }
        break;
    }

    case VG_OVER: {
        // Dead men do not steer. Input is ignored entirely and the camera
        // tumbles on all three axes around the wreck it was thrown from --
        // slowly, and slowing further, like something that has stopped being a
        // ship and started being debris.
        const float decay = 1.0f / (1.0f + vg.state_t * 0.55f);
        vg_world_step(dt,
                   0.16f * decay * sinf(vg.state_t * 0.63f),
                   0.21f * decay * sinf(vg.state_t * 0.41f + 1.1f),
                   0.30f * decay * dt,
                   0.0f);
        vg_update_missiles(dt);

        // The image never settles. Camera jitter under the screen-space tearing
        // gives the failure somewhere physical to come from -- one alone reads
        // as an effect, the two together read as a machine coming apart.
        vg.shake_x += vg_frand(-3.4f, 3.4f);
        vg.shake_y += vg_frand(-3.4f, 3.4f);

        // Knocked out is knocked out: back to the main menu, not a restart.
        if (vg.state_t > 2.2f && tap_up) { vg_cine_clear(); vg_state_cut(VG_ATTRACT); }
        break;
    }
    }
}
