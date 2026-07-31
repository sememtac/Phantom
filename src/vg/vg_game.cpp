#include "vg_sim.h"
#include "vg_arena.h"
#include "vg_sky.h"
#include "vg_tourney.h"
#include "vg_screens.h"
#include "vg_draw.h"
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
    // Badge this line unless it is genuinely CARRYING ON from the last one.
    //
    // The test used to be "is the previous line still on screen", and a pilot's
    // line stays up for 2.4 seconds while a pilot in a fight speaks far more
    // often than that -- so nearly every transmission counted as a continuation
    // and the callsign effectively never appeared again after the first. The
    // rule was right and the threshold was somebody else's.
    //
    // Continuing means immediately. Anything after a beat is a new statement,
    // and a new statement says who is making it.
    //
    // A LAST TRANSMISSION IS NEVER A CONTINUATION. A pilot almost always takes
    // hits on the way down, and a salvo puts those hurt lines well inside the
    // threshold -- so the one line in the match that most needs a name on it was
    // the one reliably losing it. It is not the end of a sentence somebody was
    // already saying. It is a different kind of statement, and it is the last
    // thing that pilot ever says.
    const bool same_voice = (vg.comms_tag[0] == s->tag[0]
                          && vg.comms_tag[1] == s->tag[1]
                          && vg.comms_tag[2] == s->tag[2]);
    vg.comms_mark  = (ev == VOICE_DEATH)
                  || !(same_voice && vg.comms_since < 0.7f);
    vg.comms_since = 0.0f;

    vg.comms_tag[0] = s->tag[0];
    vg.comms_tag[1] = s->tag[1];
    vg.comms_tag[2] = s->tag[2];
    vg.comms_tag[3] = 0;
    vg.comms_pri = (uint8_t)ev;
    // A last transmission is left up far longer. It is the only line the player
    // cannot provoke a second time, and the round is already decided -- there is
    // nothing it can be competing with.
    vg.comms_t = (ev == VOICE_DEATH) ? KILL_SPEECH : 2.4f;

    // The radio opening, not the words. A last transmission gets a lower blip --
    // the only thing distinguishing it by ear, and it should not sound routine.
    vg_sfx_play(SFX_COMMS, (ev == VOICE_DEATH) ? 0.72f : 1.0f);
}

// The broadcast voice. White, and outside the hue system entirely.
//
// Every ship in the arena earns a hue, because hue means identity here and a
// trail is the one colour you never mistake. The IFT is not in the fight, so
// giving it a hue would make it a sixteenth entrant. White is the absence of one,
// which is what a broadcast layer should be -- and it costs nothing from the
// fifteen pilot hues that vg_tourney already has to spread and keep clear of the
// player's own.
//
// It can be given several lines at once and will read them in order. One line at
// a time was right while it only ever announced a result; it is wrong the moment
// it has to say something conversational, because a paragraph delivered as one
// overlong line is not the same performance as three beats.
#define IFT_QUEUE_MAX 4

static char  s_ift_q[IFT_QUEUE_MAX][48];
static float s_ift_hold[IFT_QUEUE_MAX];
static int   s_ift_n = 0;   // lines waiting
static int   s_ift_i = 0;   // how far through them we are
static float s_ift_gap = 0.0f;   // silence owed before the next line

static void ift_pop(bool opens_run) {
    if (s_ift_i >= s_ift_n) { s_ift_n = s_ift_i = 0; return; }
    vg.ift_line = s_ift_q[s_ift_i];
    vg.ift_t    = s_ift_hold[s_ift_i];
    vg.ift_mark = opens_run;
    // EVERY line is announced, because every line is a system message. The
    // opener gets the full double beat and the lines continuing it get the short
    // form -- the distinction the badge draws visually, drawn again by ear.
    vg_sfx_play(opens_run ? SFX_IFT : SFX_IFT_SHORT, 1.0f);
    s_ift_i++;
}

void vg_ift_say(const char* line, float hold, bool badge) {
    if (!line) return;
    s_ift_gap = 0.0f;
    // An immediate line cuts off anything queued. A broadcast that has moved on
    // must not have the tail of the last announcement surface behind it.
    s_ift_n = s_ift_i = 0;
    vg.ift_line = line;
    vg.ift_t    = hold;
    vg.ift_mark = badge;
    // Same rule as the badge: an unbadged line is a caption on somebody else's
    // ship, and captions do not announce themselves.
    if (badge) vg_sfx_play(SFX_IFT, 1.0f);
}

// True while the broadcast is mid-announcement: a line up, a pause between two,
// or lines still waiting to be read. Callers use it to keep out of the way --
// anything that would post its own line has to wait, or it silently deletes the
// rest of what was being said.
bool vg_ift_busy(void) {
    return (vg.ift_line && vg.ift_t > 0.0f) || s_ift_gap > 0.0f || s_ift_i < s_ift_n;
}

// Queued lines are COPIED. A caller composing each line in its own scratch
// buffer and queueing three of them would otherwise end up holding three
// pointers to the same buffer and hear the last line three times.
void vg_ift_queue(const char* line, float hold) {
    if (!line || !*line) return;

    // A SILENT CHANNEL WITH LINES STILL QUEUED means somebody zeroed the timer
    // out from under a run -- vg_match_start does exactly that -- and those
    // lines will never be read now. Discarding them here is what keeps the queue
    // honest without every such caller having to remember the indices exist.
    //
    // Leaving them cost more than a stale line: the next announcement appended
    // to the wreckage and popped from the MIDDLE of it, so the wrong line opened
    // the run and took the badge with it. That is how this was found.
    //
    // A PENDING GAP IS NOT A DEAD RUN. Mid-announcement the channel is silent by
    // design, and without this second test the gap between two lines would look
    // exactly like abandonment and throw away the rest of what was being said.
    if (vg.ift_t <= 0.0f && s_ift_gap <= 0.0f && s_ift_i < s_ift_n)
        s_ift_n = s_ift_i = 0;

    if (s_ift_n >= IFT_QUEUE_MAX) return;   // silently, rather than shouting over
    snprintf(s_ift_q[s_ift_n], sizeof(s_ift_q[0]), "%s", line);
    s_ift_hold[s_ift_n] = hold;
    s_ift_n++;
    // Nothing up and nothing owed, so this one starts talking -- and starting is
    // what earns the badge.
    if (vg.ift_t <= 0.0f && s_ift_gap <= 0.0f) ift_pop(true);
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
// Damage
// ---------------------------------------------------------------------------

static bool s_player_hit = false;

bool vg_player_was_hit(void)  { return s_player_hit; }
void vg_clear_player_hit(void) { s_player_hit = false; }

// Any contact kills, whatever the hull is and whatever happened a moment ago.
//
// This deliberately ignores the VG_HIT grace period that vg_damage_player
// honours. That grace exists so one missile cannot cascade into three, which is
// a fairness rule about being SHOT. Flying into a wall while still flinching
// from a hit is not unfair, it is flying into a wall.
//
// VG_KILL still protects: at that point the opponent is already dead and the
// round is decided, and a win must not be taken back after the fact.
void vg_kill_player(void) {
    if (vg.state == VG_KILL) return;
    // BOTH. The hull cue carries the panel's damage beeps, and dying is the one
    // moment they most belong -- a collision comes straight through here without
    // ever touching vg_damage_player, so the loudest thing that can happen to the
    // player was also the quietest thing on the panel.
    vg_sfx_play(SFX_EXPLODE, 0.8f);
    vg_sfx_play(SFX_HIT, 1.0f);
    vg.health        = 0.0f;
    vg.hit_flash     = 0.6f;
    vg.damage_glitch = DAMAGE_GLITCH;
    vg.shake         = 1.0f;
    s_player_hit     = true;
}

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
    vg_sfx_play(SFX_HIT, 1.0f);
    vg.hit_flash     = 0.6f;
    vg.damage_glitch = DAMAGE_GLITCH;
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

    // Lock time scales with speed: acquiring is harder the faster you are going,
    // which is the trade the throttle is supposed to be. ACQUIRING -- not holding.
    // See the latch below.
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
        vg.locked      = false;
    }
    vg.lock_t += dt;

    // ONCE EARNED, A LOCK IS HELD. It used to be re-evaluated from scratch every
    // frame against a threshold that rises with speed, so opening the throttle
    // after locking instantly revoked it -- the player was forbidden from using
    // the one control the fight is supposed to be about, at the exact moment they
    // had committed to an attack.
    //
    // That was the sharpest tooth in a set of three, all pushing the same way:
    // turn rate is 2.5x better at idle, acquisition is far slower at speed, and
    // this. Any one of them is a trade. Together they made "stop and shoot" not a
    // style but the only style, which is what the playtest found.
    //
    // The lock still drops the moment the target leaves the cone or the range, so
    // it is held by keeping the nose on them -- not by having once been fast
    // enough. Acquiring at speed is still hard; that trade is the point and it
    // stays.
    if (vg.lock_t >= vg.lock_need) vg.locked = true;
}

// The magazine refills ALL AT ONCE, and only from empty.
//
// It used to trickle a round back every `reload` seconds whenever the rack was
// below full, which quietly meant a class could never actually run dry: shoot
// four of six, wait, and you were topped up without ever having made a decision.
// A clip that refills while you are still shooting out of it cannot cost
// anything, so it cannot define a playstyle either.
//
// Now emptying the rack is the commitment. CHARIOT dumps twelve rounds in under
// two seconds and then has nine seconds of nothing; BALLISTA has three and has to
// mean all of them.
static void update_reload(float dt) {
    if (vg.missiles > 0 || vg.reload_t <= 0.0f) return;
    vg.reload_t -= dt;
    if (vg.reload_t <= 0.0f) {
        vg.reload_t = 0.0f;
        vg.missiles = vg.spec->magazine;
    }
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
    vg.fire_gap = vg.spec->fire_gap;
    vg_sfx_play(SFX_LAUNCH, 1.0f);

    // Emptying the rack starts the clock. Doing it here rather than in the tick
    // means the reload is timed from the shot that emptied it, not from the next
    // frame that happened to notice.
    if (vg.missiles <= 0) vg.reload_t = vg.spec->reload;
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
    vg_sky_generate(SKY_MENU, vg_replay_rand());   // we boot straight into the menu

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

    vg.ring_alive  = false;      // no gate follows the player into a round
    // Every round is its own broadcast, so the announcer's one-shot flags clear
    // here rather than at boot. Clearing them at boot only would have introduced
    // the fighters once and then gone quiet for the rest of the tournament.
    vg.ift_fired   = 0;
    vg.ift_line    = nullptr;
    vg.ift_t       = 0.0f;
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
    vg.shake       = 0;
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
// The roll command as an angle for this frame. Lives here rather than at the
// three call sites because it is part of the flight model, and because the
// throttle it depends on is the smoothed one -- roll authority should lag a
// shove of the slider exactly as speed does.
static inline float roll_angle(const VgInput* in, float dt) {
    const float scale = ROLL_SLOW_SCALE
                      + (ROLL_FAST_SCALE - ROLL_SLOW_SCALE) * vg.throttle;
    // ...and the airframe. A LANCE and a CHARIOT rolled at exactly the same rate
    // before this, which quietly said the classes were interchangeable on the one
    // axis the player had just been handed.
    const float want = in->roll * ROLL_RATE * scale * vg_ship_mobility(vg.spec);

    // CHASED, not set. The rate the player asks for is where the airframe is
    // going, not where it already is -- so a roll has to be started and has to be
    // allowed to stop, and letting go leaves the ship still turning for a moment.
    // That carried-through part is the difference between flying it and rotating
    // the picture.
    float k = dt * ROLL_LERP;
    if (k > 1.0f) k = 1.0f;
    vg.roll_rate += (want - vg.roll_rate) * k;

    return vg.roll_rate * dt;
}

void vg_world_step(float dt, float pitch_in, float yaw_in, float roll_in,
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

    // What the airframe is doing about it. Quadratic in speed against the fastest
    // class, times this ship's own nervousness -- so a CHARIOT at full is rougher
    // than a BALLISTA at full, rather than the two merely being equally rough at
    // their own maxima, which is what a throttle-fraction curve gives you and is
    // not the same fantasy at all.
    const float sn = vg.speed / SPEED_SHAKE_REF;
    vg.buzz = sn * sn * vg.spec->shake
    // Rolling is work, and the airframe should be seen doing it. Without this the
    // ship rattled harder the faster it went and then rolled through ninety
    // degrees in perfect calm, which is the picture turning rather than anything
    // happening to the machine.
            + fabsf(vg.roll_rate) * ROLL_BUZZ;

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

    // The cosmetic lean, from the yaw command as always -- plus a lead into any
    // roll. Rolling used to have NO visual signature of its own: yaw is forced to
    // zero while the roll key is held, so the bank actually decayed to nothing
    // during the one manoeuvre that deserved the most emphasis.
    //
    // Same sign as the roll, which is not a guess: vg_sky_step above is handed
    // `vg.bank + vg.roll` as a single apparent angle, so the two already share a
    // convention and adding them exaggerates rather than cancels.
    const float roll_rate_now = (dt > 0.0f) ? (roll_in / dt) : 0.0f;
    const float bank_target   = (-yaw_in * BANK_MAX)
                              + roll_rate_now * ROLL_BANK_LEAD;

    float kb = dt * BANK_LERP;
    if (kb > 1.0f) kb = 1.0f;
    vg.bank += (bank_target - vg.bank) * kb;

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
        // Rotation cannot skew an orthonormal pair, but floating point drift
        // over thousands of frames can, and the failure is silent -- the model
        // simply stops having front faces.
        {
            Vec3 u = vsub(c->up, vmul(c->fwd, vdot(c->up, c->fwd)));
            if (vlen2(u) > 1e-6f) c->up = vnorm(u);
        }
        if (vg.gate_t > 0.0f) {
            vg.gate_pos = mat3_apply(R, vg.gate_pos);
            vg.gate_r   = vnorm(mat3_apply(R, vg.gate_r));
            vg.gate_u   = vnorm(mat3_apply(R, vg.gate_u));
        }
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

        // NO DISTANCE CULL. There used to be one at CULL_RADIUS, which is 4200,
        // and ARENA_TORUS_RMAJ is also 4200. The arena is a LOOP of that radius,
        // so two ships on opposite sides of it are 8400 apart -- twice the
        // distance that deleted one of them. An opponent spawns at up to 3625,
        // already most of the way there, and any drift down the tunnel finished
        // the job.
        //
        // It killed the opponent with no debris, no explosion and no last line,
        // the match saw nobody left alive, and the player won a fight that never
        // happened. The clamp above already keeps every enemy inside the torus,
        // so there is nothing left for a cull to save us from.
    }

    // The gate is world geometry like anything else: it counter-rotates and
    // recedes, so the course needs no idea how flight works.
    if (vg.ring_alive) {
        vg.ring_pos  = mat3_apply(R, vg.ring_pos);
        vg.ring_norm = vnorm(mat3_apply(R, vg.ring_norm));
        vg.ring_pos.z -= dz;
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

    // Buzz, on top of whatever the impact shake is doing. Quadratic in speed, so
    // it is present the whole way up rather than switching on near the stop, and
    // scaled by the airframe -- see vg.buzz in the world step.
    if (vg.buzz > 0.0f) {
        const float a = SPEED_SHAKE_MAX * vg.buzz;
        vg.shake_x += vg_frand(-a, a);
        vg.shake_y += vg_frand(-a, a);
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
    if (vg.hud_boot      > 0) vg.hud_boot      -= dt;
    if (vg.damage_glitch > 0) vg.damage_glitch -= dt;
    if (vg.gate_t   > 0) vg.gate_t   -= dt;
    vg.comms_since += dt;

    if (vg.comms_t > 0) {
        vg.comms_t -= dt;
        if (vg.comms_t <= 0) { vg.comms_line = nullptr; vg.comms_pri = 0; }
    }
    if (vg.ift_t > 0) {
        vg.ift_t -= dt;
        // Down, then a beat of silence before whatever is next -- so a queued
        // announcement reads as consecutive beats rather than one paragraph.
        if (vg.ift_t <= 0) {
            vg.ift_line = nullptr;
            s_ift_gap   = (s_ift_i < s_ift_n) ? IFT_GAP : 0.0f;
        }
    } else if (s_ift_gap > 0.0f) {
        s_ift_gap -= dt;
        // A continuation, so no badge: the channel is mid-announcement and nobody
        // else can have taken it.
        if (s_ift_gap <= 0.0f) ift_pop(false);
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
            vg_spawn_debris(s->pos, 20.0f, 12);
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
// Out of combat the backdrop is always the menu one. Regenerating costs ~60ms,
// which is invisible at a screen change and is the price of not carrying a
// second full texture just to avoid it.
static void use_menu_sky(void) {
    vg_sky_generate(SKY_MENU, vg_replay_rand());
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
    // The course never asked for a sky, so it kept whatever was last built --
    // which coming from the menu is the menu's own backdrop, and that one does
    // not pan. The hole therefore hung off the camera and only turned with the
    // roll, so a player who tried to look at it could never bring it round. It
    // is a place now, at a fixed bearing, found by turning towards it.
    vg_sky_generate(SKY_COURSE, vg_replay_rand());
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

const char* vg_state_name(VgState s) {
    return ((int)s < VG_STATE_COUNT) ? STATES[s].name : "?";
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
            update_lock(dt);
            if (vg.fire_gap > 0) vg.fire_gap -= dt;
            update_reload(dt);
            if (vg.locked) player_fire();
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
        vg_world_step(dt, in->pitch, in->yaw, roll_angle(in, dt), in->throttle);
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
        vg_world_step(dt, in->pitch, in->yaw, roll_angle(in, dt), in->throttle);
        vg_update_missiles(dt);
        update_threat();
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
                if (!vg.course_briefing) vg_state_cut(VG_BRACKET);
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

        vg_world_step(dt, in->pitch, in->yaw, roll_angle(in, dt), in->throttle);

        for (int i = 0; i < MAX_ENEMIES; i++) vg_update_enemy(&vg.enemy[i], i, dt);

        vg_update_missiles(dt);
        update_lock(dt);
        update_threat();

        // No hull regeneration. Damage taken here is carried for the rest of the
        // tournament and only credits will undo it, which is what makes the
        // repair economy the difficulty curve rather than a side system.

        if (vg.fire_gap > 0) vg.fire_gap -= dt;
        update_reload(dt);
        if (in->fire_edge) player_fire();

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
                    vg_spawn_debris(c->pos, 30.0f, 18);
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
