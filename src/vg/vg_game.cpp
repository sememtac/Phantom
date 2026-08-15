#include "vg_sim.h"
#include "vg_states.h"
#include "vg_shake.h"
#include "vg_cockpit.h"
#include "vg_surge.h"
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
#include "vg_prof.h"
#include "vg_canopy_set.h"
#include "vg_anomaly.h"
#include "vg_flight.h"
#include "vg_weapons.h"
#include <Arduino.h>
#include "vg_replay.h"
#include <math.h>
#include <string.h>
#include "vg_canopy_draw.h"
#include "vg_tv.h"

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


// Put the bracket's opponent into the world. Called twice: once at match setup
// so the cutscene has a ship to introduce, and again when the cutscene ends, by
// which point the viewpoint has drifted for twelve seconds and their position
// relative to the arena means nothing.
void vg_spawn_opponent(void) {
    const Entrant* opp = vg_tourney_opponent();
    if (opp) {
        vg_spawn_enemy(0, opp->cls,
                    ENEMY_SKILL * (0.75f + 0.35f * opp->rating), opp->hue);
        vg.enemy[0].voice = opp->voice;
        for (int i = 0; i < 4; i++) vg.enemy[0].tag[i] = opp->tag[i];
    } else {
        vg_spawn_enemy(0, SHIP_AEGIS, ENEMY_SKILL, 0.02f);
    }
}

void vg_spawn_enemy(int i, ShipClass cls, float skill, float hue) {
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
    // A COCKPIT FROM THE START, so nothing downstream has to cope with not having one. The
    // per-hull choice is made again at each match entry; this is only so the rasteriser's
    // pointer is never null between boot and the first flight.
    vg_canopy_use(vg_canopy_default());
    // The attract loop and every menu fly a round tube. Set here so nothing has to
    // remember to clear it on the way out of a match.
    vg_anomaly_clear();
    vg_surge_clear();
    // AND THE COURSE, for a reason worth stating: its state left VgGame, so the memset
    // at the top of this function no longer reaches it. begin_record calls vg_game_init
    // to restart the game before a recording, and without this a session would open
    // carrying the gates of whatever was flown before it -- which a replay would not
    // reproduce, and which nothing else would have reported.
    vg_course_clear();
    // Same reason as the two above: the transition's three fields left VgGame, so the
    // memset no longer reaches them and a recording must not start mid-cut.
    vg_tv_clear();
    vg_trail_clear();
    vg_cine_reset();
    vg_wpn_clear();
    vg_threat_clear();
    vg_shake_clear();
    vg_cockpit_clear();
    vg_wall_clear();
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
    vg_vol.music   = 0.70f;
    vg_vol.sfx     = 0.85f;
    vg.ship        = SHIP_AEGIS;
    vg.spec        = vg_spec(vg.ship);
    vg.health_max  = vg.spec->hull;
    vg.health      = vg.health_max;
    vg.throttle    = 0.45f;
    vg.speed       = vg.spec->speed_min;
    vg_wpn.rounds    = vg.spec->magazine;
    vg_wpn.target = -1;

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
    vg_wpn.rounds   = vg.spec->magazine;
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

// THE CHAMPION DIED. See the declaration in vg_sim.h for why this exists.
void vg_title_lost(void) {
    // WHO TAKES IT. The round's opponent, and it has to be read BEFORE the reset,
    // because vg_tourney_opponent indexes the bracket the reset is about to abandon.
    //
    // Guarded rather than assumed: a champion can die outside a tournament match once
    // there is any other way to fly, and an inherited name of three NULs would print
    // as nothing and seed a nameless legend. No opponent means the name simply goes
    // back to being a rumour, which is where the crawl has it anyway.
    const Entrant* opp = vg_tourney_opponent();
    if (opp && opp->tag[0]) {
        for (int i = 0; i < 3; i++) vg.phantom_tag[i] = opp->tag[i];
        vg.phantom_tag[3] = 0;
    }

    // AND THE PLAYER IS NOBODY AGAIN. Everything attained goes: the bank, the name,
    // the hull, the title. Not the volumes -- those are the room's, not the pilot's.
    //
    // The same values vg_game_init starts a fresh install with, because that is
    // exactly what this is. Written out here rather than left to the entry screen, so
    // that a power cycle between the death and the next tournament cannot restore a
    // champion who is dead.
    vg.champion    = false;
    vg.credits     = CREDIT_START;
    vg.callsign[0] = 'A'; vg.callsign[1] = 'C'; vg.callsign[2] = 'E';
    vg.callsign[3] = 0;
    vg.trail_hue   = 0.52f;
    vg.ship        = SHIP_AEGIS;
    vg.spec        = vg_spec(vg.ship);
    vg.health_max  = vg.spec->hull;
    vg.health      = vg.health_max;
    vg_wpn.rounds  = vg.spec->magazine;
    vg_save_store();
}

void vg_match_start(void) {
    for (int i = 0; i < MAX_ENEMIES;   i++) vg.enemy[i].alive = false;
    for (int i = 0; i < MAX_MISSILES;  i++) vg.msl[i].alive   = false;
    for (int i = 0; i < MAX_ASTEROIDS; i++) vg.ast[i].alive   = false;
    for (int i = 0; i < MAX_DEBRIS;    i++) vg.deb[i].alive   = false;
    for (int i = 0; i < MAX_FIREBALLS; i++) vg.fire[i].alive  = false;

    vg_course.ring_alive  = false;      // no gate follows the player into a round
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
    vg_cine.on     = false;
    vg_cockpit.boot    = 0;
    // THE BOOT CHAIN, disarmed. This runs from enter_intro, at the top of the CUTSCENE, so it is
    // seconds ahead of the player taking the seat -- and the raster side has to be disarmed with
    // the game side or the cue stays latched from the previous match and fires over the cutscene.
    vg_cockpit.cued    = false;
    vg_cockpit.ready       = false;
    vg_cockpit.radio_t     = 0;
    vg_cockpit.regions_lit = 0;
    vg_canopy_intro_reset();
    vg.msl_event   = MSL_NONE;
    vg.msl_event_t = 0;
    vg_trail.n     = 0;
    vg_trail.head  = 0;
    vg_trail.acc   = 0;
    vg.spec        = vg_spec(vg.ship);
    vg.health_max  = vg.spec->hull;
    // THE INPUT'S THROTTLE TOO, not just the simulation's. vg.throttle is a follower --
    // vg_world_step lerps it toward in->throttle every frame -- so setting it here and
    // nothing else meant a new match opened at the last one's speed. Reported from play.
    vg.throttle     = THROTTLE_START;
    vg.throttle_vis = THROTTLE_START;
    vg_input_throttle_set(THROTTLE_START);

    // THE ARENA LAUNCHES SMOOTH, WHATEVER IS COMING. The match's weather is rolled here and
    // the tunnel is set round, so the venue the player is introduced to over the cutscene is
    // the one they take off into. A disturbance that was already there at launch would be a
    // property of the map; arriving partway through a fight makes it an event.
    //
    // Read from the bracket rather than passed in. vg_match_start is reached from exactly one
    // place -- enter_intro, by way of the table -- and a match is always a tournament match, so
    // vt.round is the round about to be flown. The course is the other way in and does not come
    // through here at all, which is what makes this the right place for a tournament-only effect.
    //
    // THE ROUND IS TURNED INTO A 0..1 RAMP HERE, and not inside the event. This is the one place
    // that already holds the bracket, and an event module that reached for vt would be coupling
    // the venue's weather to the tournament's shape for the sake of one division.
    vg_anomaly_begin_match((vt.round < 3) ? (float)vt.round / 3.0f : 1.0f);
    // THE SAME RAMP, and rolled straight after the anomaly so the order of the draws from
    // the recorded stream is fixed and documented rather than incidental. See vg_event.h:
    // this ordering IS the behaviour, and moving it changes every recording.
    vg_surge_begin_match((vt.round < 3) ? (float)vt.round / 3.0f : 1.0f);
    vg.bank        = 0;
    vg_shake_clear();
    vg.hit_flash   = 0;
    vg_wpn.rounds    = vg.spec->magazine;
    vg_wpn.reload_t    = 0.0f;          // a full rack is not reloading
    vg_wpn.fire_gap    = 0;
    vg_wpn.target = -1;
    vg_wpn.lock_t      = 0;
    vg_wpn.locked      = false;
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
    vg_wall_seed();

    // One opponent, taken from the bracket. A match is strictly one on one --
    // the old "keep a fight going" respawn belonged to an endless survival mode
    // and would make a knockout round unwinnable.
    vg_spawn_opponent();

    // They open the match. The first thing you learn about an opponent should
    // be what kind of person they are, not what they are flying.
    //
    // The countdown does not start until vg_cockpit.ready, so this is measured from a LIT PANEL rather
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


// Returning to the title card has to take the finished run with it. Quitting
// from the pause menu and being knocked out both used to just set the state,
// leaving the loser's missiles and wreckage flying through the attract loop --
// and a missile whose seeker had broken draws in the dead-seeker grey, which is
// exactly the stray grey lines that were turning up on the menu.
uint32_t g_upd_pre, g_upd_ship, g_upd_arena, g_upd_sky, g_upd_field,
         g_upd_trail, g_upd_enemy, g_upd_ord, g_upd_vfx,
         g_upd_ai, g_upd_combat;

void vg_game_update(float dt, const VgInput* in) {
    const uint32_t t_pre = micros();
    vg_tv_update(dt);

    // A press during the wipe belongs to neither scene. Without this a tap that
    // started the transition is still live when the new screen appears and gets
    // spent on whatever happens to be under the finger.
    VgInput gated;
    if (vg_tv.phase != TV_NONE) {
        gated = *in;
        gated.fire_edge = gated.alt_edge = gated.menu_edge = false;
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
    const bool alive  = (vg_tv.phase == TV_NONE) && (sf & VGS_LIVE);
    // The engine runs a little wider: through VG_KILL the ship is still flying,
    // and cutting the hum the instant the opponent dies would be the loudest
    // thing about that moment.
    const bool flying = (vg_tv.phase == TV_NONE) && (sf & VGS_ENGINE);

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
    vg_sfx_flatline(vg_tv.phase == TV_NONE
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

    // ONE CALL, NOT SEVEN (the call itself is below, after the profile bracket). Hoisted out of
    // the cases because it is a property of the state rather than a step in its logic -- and hoisted on VGS_DRIFT and
    // NOT on VGS_MENU, which is the trap this replaces. Nine states are menus;
    // only seven of them drift. INTRO gives the viewpoint to the cutscene camera
    // and OVER tumbles the wreck with its own world step, so hoisting on
    // VGS_MENU would have run two world motions at once in both.
    //
    // It was the first statement of all seven cases, so nothing moves past it.
    // CLOSED BEFORE THE DRIFT, and it was closed after it first, which made every span
    // in the world step get counted twice on a menu.
    //
    // vg_menu_world calls vg_world_step. With the bracket below the call, all seven of
    // the world step's own spans ran INSIDE pre -- so the title screen reported pre 506
    // of a 523 total while the other spans came to 310, and `oth`, being a subtraction,
    // clamped at zero rather than going negative and saying so.
    //
    // The flight path hid it completely: there, the world step runs from the state's own
    // update, which is after this point, so pre read a truthful 57 us. A nesting fault
    // is invisible in exactly the case you are profiling for.
    g_upd_pre += micros() - t_pre;

    // ONE CALL, NOT SEVEN -- see the note below. It is a world step, so its cost belongs
    // to the world step's spans, and what is left of it falls into `oth`.
    if (sf & VGS_DRIFT) vg_menu_world(dt);

    // ONE LINE, AND THIRTEEN NAMED FUNCTIONS. What used to be a 390-line switch
    // is now a column in the table, which is the same argument the table already
    // makes about flags and entry hooks: a state's properties are declared once,
    // in one place, where a missing one is visible as a gap in a row rather than
    // as a case somebody forgot to write.
    vg_state_update(dt, in, &tap);
}
