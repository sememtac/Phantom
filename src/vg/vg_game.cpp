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
#include "vg_raster.h"
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

void vg_spawn_asteroid(void) {
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
    vg.wpn.rounds    = vg.spec->magazine;
    vg.wpn.target = -1;

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
    vg.wpn.rounds   = vg.spec->magazine;
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

    vg.course.ring_alive  = false;      // no gate follows the player into a round
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
    vg.cine.on     = false;
    vg.hud_boot    = 0;
    // THE BOOT CHAIN, disarmed. This runs from enter_intro, at the top of the CUTSCENE, so it is
    // seconds ahead of the player taking the seat -- and the raster side has to be disarmed with
    // the game side or the cue stays latched from the previous match and fires over the cutscene.
    vg.hud_cued    = false;
    vg.ready       = false;
    vg.radio_t     = 0;
    vg_canopy_intro_reset();
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
    vg.wpn.rounds    = vg.spec->magazine;
    vg.wpn.reload_t    = 0.0f;          // a full rack is not reloading
    vg.wpn.fire_gap    = 0;
    vg.wpn.target = -1;
    vg.wpn.lock_t      = 0;
    vg.wpn.locked      = false;
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
    //
    // The countdown does not start until vg.ready, so this is measured from a LIT PANEL rather
    // than from the top of the match -- which is why it can afford to be a real pause now
    // instead of the 1.4 s it was when it had to beat the boot.
    vg.comms.line = nullptr;
    vg.comms.t    = 0;
    vg.comms.pri  = 0;
    vg.taunt_t    = BOOT_FIRST_TAUNT;

    for (int i = 0; i < AST_TARGET_COUNT; i++) vg_spawn_asteroid();

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

// ---------------------------------------------------------------------------
// One function per state
//
// Each is a row's `update` column. They take the frame, the gated input and the
// resolved tap, and NOTHING ELSE -- checked before splitting them out, because a
// case that reached for one of the dispatch's own locals would have needed a
// wider signature and a worse one. None did.
// ---------------------------------------------------------------------------

void vg_upd_attract(float dt, const VgInput* in, const Tap* tap) {
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
        if (vg.wpn.fire_gap > 0) vg.wpn.fire_gap -= dt;
        vg_update_reload(dt);
        if (vg.wpn.locked) vg_player_fire();
        vg.health = vg.health_max;      // never let the load generator "die"
    }
#endif
    if (tap->up) {
        vg_entry_reset();
        vg_state_go(VG_ENTRY);
    }
}

void vg_upd_entry(float dt, const VgInput* in, const Tap* tap) {
    if (vg_entry_update(in, tap->up, tap->x, tap->y)) {
        vg_state_go(VG_SELECT);
    }
}

void vg_upd_repair(float dt, const VgInput* in, const Tap* tap) {
    if (vg_repair_update(in, tap->up, tap->x, tap->y)) vg_state_go(VG_BRACKET);
}

void vg_upd_select(float dt, const VgInput* in, const Tap* tap) {
    // The +/- key cycles as well as the cards, since it is already wired and
    // is the fastest way to feel the difference between classes.
    if (in->alt_edge)
        vg_game_select_ship((ShipClass)((vg.ship + 1) % SHIP_CLASSES));
    if (tap->up) {
        int card = vg_select_card_at(tap->x, tap->y);
        if (card >= 0) {
            vg_game_select_ship((ShipClass)card);
        } else if (vg_select_confirm_at(tap->x, tap->y)) {
            // The draw is made HERE, so the course that follows has a
            // tournament to return to and the player flies it in the airframe
            // they actually picked.
            vg_tournament_begin(vg.ship);
            vg_bracket_focus_player();
            vg_state_cut(VG_COURSE);
        }
    }
}

void vg_upd_intro(float dt, const VgInput* in, const Tap* tap) {
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

    if (vg_cine_update(dt, tap->up)) {
        // Both halves have names now. The cutscene's own teardown is INTRO's
        // leave hook and runs inside vg_state_go; the match set-up is
        // vg_begin_flight, which is called by name because VG_HIT re-enters
        // VG_PLAYING and must not repeat any of it.
        vg_begin_flight();
        vg_state_go(VG_PLAYING);
    }
}

void vg_upd_course(float dt, const VgInput* in, const Tap* tap) {
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
        vg.course.hits = 0;
        vg_arena_init(ARENA_TORUS);
        vg.wall_clear  = vg_arena_clearance(vg_arena_local_of(v3(0, 0, 0)));
        vg_ift_line(IFT_COURSE_MISS);
        vg_course_reset_streak();
    }

    // Finishing is a MOMENT, not an exit condition. The gate is already gone
    // and the player keeps flying while the broadcast marks it, exactly as a
    // kill does -- see COURSE_DONE_BEAT.
    if (vg.course.done) {
        vg.course.end_t += dt;
        if (vg.course.end_t > COURSE_DONE_BEAT) vg_state_cut(VG_BRACKET);
    }


}

void vg_upd_kill(float dt, const VgInput* in, const Tap* tap) {
    // The world keeps running -- wreckage still tumbles, their last trail
    // still fades -- but nothing can touch the player. The only job of this
    // state is to let the dead pilot finish talking.
    vg_world_step(dt, in->pitch, in->yaw, vg_roll_angle(in, dt), in->throttle);
    vg_update_missiles(dt);
    vg_update_threat();
    if (vg.wpn.fire_gap > 0) vg.wpn.fire_gap -= dt;
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
}

void vg_upd_bracket(float dt, const VgInput* in, const Tap* tap) {
    if (in->menu_held) vg_bracket_pan(in->menu_dx, in->menu_dy);
    if (tap->up) {
        if (vg_bracket_ready_at(tap->x, tap->y)) {
            vg_state_cut(VG_INTRO);
        } else if (vg_bracket_course_at(tap->x, tap->y)) {
            vg_state_cut(VG_COURSE);
        } else if (vg_bracket_repair_at(tap->x, tap->y)) {
            vg_repair_reset();
            vg_state_go(VG_REPAIR);
        }
    }
}

void vg_upd_pause(float dt, const VgInput* in, const Tap* tap) {
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
        if (tap->up && vg_pause_back_at(tap->x, tap->y)) {
            vg.pause_page = 0;
            vg_save_store();        // settings outlive the session
        }
        return;   // was a break out of the switch: this frame is done
    }

    if (tap->up) {
        switch (vg_pause_item_at(tap->x, tap->y, from_course)) {
        case PAUSE_RESUME:
            vg_state_resume(back);
            break;
        case PAUSE_AUDIO:
            vg.pause_page = 1;
            break;
        case PAUSE_SKIP:
            // Refused while the briefing runs: the player may pause over it,
            // read it and think about it, but not walk out of it.
            if (vg.course.named) vg_state_cut(VG_BRACKET);
            break;
        case PAUSE_QUIT:
            vg_state_cut(VG_ATTRACT);
            break;
        default: break;
        }
    }
}

void vg_upd_round_won(float dt, const VgInput* in, const Tap* tap) {
    if (vg.state_t > 2.4f) {
        vg_tourney_resolve(true);
        if (vt.complete) {
            vg.champion = true;
            // Back out of combat, so back to the menu sky. The bracket
            // gets this from enter_bracket; the winner's card does not
            // pass through it.
            vg_use_menu_sky();
            vg_state_go(VG_WON);
            vg_save_store();      // the name sticks from here on
        } else {
            vg_state_go(VG_BRACKET);
        }
    }
}

void vg_upd_won(float dt, const VgInput* in, const Tap* tap) {
    // Returns on its own. The sequence ends by handing the player back to
    // the title card, where the crawl now says the rumour is about them --
    // so the payoff is not the win screen, it is the menu behind it having
    // quietly changed. A prompt would invite a tap that skips exactly that.
    //
    // Tapping is allowed only once the name is fully up, so an impatient
    // hand cannot cut the one moment the whole story was built toward.
    if (vg.state_t > WON_RETURN ||
        (vg.state_t > WON_NAME_IN + 2.6f && tap->up)) vg_enter_attract();
}

void vg_upd_playing(float dt, const VgInput* in, const Tap* tap) {
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

    if (vg.wpn.fire_gap > 0) vg.wpn.fire_gap -= dt;
    vg_update_reload(dt);
    if (in->fire_edge) vg_player_fire();

    // Unprompted chatter, on a long timer and only when the radio is idle.
    // Taunts are flavour; letting one interrupt a hit or a kill would turn
    // the most informative channel on the HUD into noise.
    //
    // AND NOT UNTIL THE PANEL IS LIT. The countdown is held, not merely suppressed: an
    // opponent's opening line is the first thing you learn about them, and it was landing
    // while the cockpit was still coming online -- so it played to a dark screen and the
    // timer had already moved on by the time there was a panel to read it on. Held, the
    // opening remark arrives on a lit panel however long the boot took.
    if (vg.ready) {
        vg.taunt_t -= dt;
        if (vg.taunt_t <= 0.0f) {
            vg.taunt_t = vg_frand(12.0f, 21.0f);
            if (vg.comms.t <= 0.0f) {
                for (int i = 0; i < MAX_ENEMIES; i++)
                    if (vg.enemy[i].alive) { vg_comms_say(&vg.enemy[i], VOICE_TAUNT); break; }
            }
        }
    }

#if ENABLE_ASTEROIDS
    // Keep the field topped up as a speed cue.
    int alive_ast = 0;
    for (int i = 0; i < MAX_ASTEROIDS; i++) if (vg.ast[i].alive) alive_ast++;
    vg.spawn_t -= dt;
    if (alive_ast < AST_TARGET_COUNT && vg.spawn_t <= 0) {
        vg_spawn_asteroid();
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
                Ship* c = &vg.cine.ship;
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
                vg.cine.on  = true;
                // The player's own wreck, and the biggest eruption in the
                // game. Scaled off c->scale (84) rather than a ship's own
                // size: this one is deliberately staged large and close, and
                // an explosion tuned for a distant fighter looked like a
                // spark next to it.
                vg_spawn_blast(c->pos, 62.0f, 11, 0, 2.2f);
                vg_spawn_shrapnel(c->pos, 40.0f, 72.0f, 44, 4.8f, 2.0f);
            }
            return;   // was a break out of the switch: this frame is done
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
}

void vg_upd_over(float dt, const VgInput* in, const Tap* tap) {
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
    if (vg.state_t > 2.2f && tap->up) { vg_cine_clear(); vg_state_cut(VG_ATTRACT); }
}

// Returning to the title card has to take the finished run with it. Quitting
// from the pause menu and being knocked out both used to just set the state,
// leaving the loser's missiles and wreckage flying through the attract loop --
// and a missile whose seeker had broken draws in the dead-seeker grey, which is
// exactly the stray grey lines that were turning up on the menu.
void vg_game_update(float dt, const VgInput* in) {
    vg_tv_update(dt);

    // A press during the wipe belongs to neither scene. Without this a tap that
    // started the transition is still live when the new screen appears and gets
    // spent on whatever happens to be under the finger.
    VgInput gated;
    if (vg.tv.phase != TV_NONE) {
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
    const bool alive  = (vg.tv.phase == TV_NONE) && (sf & VGS_LIVE);
    // The engine runs a little wider: through VG_KILL the ship is still flying,
    // and cutting the hum the instant the opponent dies would be the loudest
    // thing about that moment.
    const bool flying = (vg.tv.phase == TV_NONE) && (sf & VGS_ENGINE);

    // Looking aft is a look and nothing else -- it takes no input away from the
    // ship and changes no state the simulation reads. Only while there is a
    // cockpit to look out of, and never during a transition, or the last frame
    // before the wipe would be shot from the wrong way round.
    vg.rear_view = alive && in->rear_held;

    vg_update_alerts(alive ? dt : 0.0f, alive);
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
    vg_sfx_flatline(vg.tv.phase == TV_NONE
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
    Tap tap = { false, 0.0f, 0.0f };

    if (in->menu_edge) { s_press_x = in->menu_x; s_press_y = in->menu_y; s_travel = 0; }
    if (in->menu_held) s_travel += fabsf(in->menu_dx) + fabsf(in->menu_dy);
    if (s_held && !in->menu_held && s_travel < MENU_TAP_SLOP) {
        tap.up = true;
        tap.x  = s_press_x;
        tap.y  = s_press_y;
    }
    s_held = in->menu_held;

    // ONE CALL, NOT SEVEN. Hoisted out of the cases because it is a property of
    // the state rather than a step in its logic -- and hoisted on VGS_DRIFT and
    // NOT on VGS_MENU, which is the trap this replaces. Nine states are menus;
    // only seven of them drift. INTRO gives the viewpoint to the cutscene camera
    // and OVER tumbles the wreck with its own world step, so hoisting on
    // VGS_MENU would have run two world motions at once in both.
    //
    // It was the first statement of all seven cases, so nothing moves past it.
    if (sf & VGS_DRIFT) vg_menu_world(dt);

    // ONE LINE, AND THIRTEEN NAMED FUNCTIONS. What used to be a 390-line switch
    // is now a column in the table, which is the same argument the table already
    // makes about flags and entry hooks: a state's properties are declared once,
    // in one place, where a missing one is visible as a gap in a row rather than
    // as a case somebody forgot to write.
    vg_state_update(dt, in, &tap);
}
