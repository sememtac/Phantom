#pragma once
#include <stdint.h>
#include "vg_vec.h"
#include "vg_config.h"
#include "vg_input.h"
#include "vg_ship.h"

// The whole simulation lives in VIEW SPACE: the player is permanently at the
// origin looking down +z, and the world rotates and translates around it. That
// removes the camera matrix entirely, keeps coordinates small no matter how far
// you fly, and makes "is it in front of me" a plain z test.
//
// A useful consequence for combat: in this frame the player really is
// stationary, so an enemy missile pursuing the player needs no lead prediction
// at all -- pure pursuit toward the origin is already a correct intercept.

struct AstModel {
    Vec3    v[AST_VERTS];
    uint8_t e[AST_EDGES][2];
    // Triangles, wound so the cross product of the first two edges points
    // outward. That lets the renderer decide facing with one dot product.
    uint8_t f[AST_FACES][3];
    uint8_t edge_count;
    uint8_t face_count;
};

struct Asteroid {
    bool     alive;
    Vec3     pos;
    Vec3     vel;
    float    radius;
    float    spin[3];
    float    spin_rate[3];
    uint8_t  model;
};

// An enemy fighter. Orientation is carried as forward+up rather than a matrix
// so that repeatedly folding in the world rotation cannot accumulate shear --
// the pair is re-orthonormalised every frame, which a matrix would not be.
struct Ship {
    bool  alive;
    // Which class this fighter is flying. Everything about its performance comes
    // from here, so an NPC BALLISTA is the same ship the player can pick.
    const ShipSpec* spec;
    // Pilot quality, as a scale on turn rate. The bracket will eventually seed
    // this per entrant; for now it is a constant that reproduces the difficulty
    // the game had when enemies were strictly worse than the player.
    float skill;
    Vec3  pos;
    Vec3  fwd;
    Vec3  up;
    float speed;
    float target_speed;
    float hull;           // absolute points, counted down from spec->hull
    float fire_cd;
    float evade_t;        // >0 while breaking away from an incoming missile
    Vec3  evade_dir;
    float break_t;        // >0 while extending away after a firing pass
    Vec3  break_dir;
    Vec3  offset_dir;     // stable lateral offset for the aim point
    float roll_vis;       // visual bank, radians, applied at render time
    float hit_flash;

    // Who is flying it. Carried on the ship rather than looked up through the
    // bracket, because a missile detonating has to name the pilot it just
    // killed and has no business knowing what a tournament is.
    char    tag[4];
    uint8_t voice;

    // Model size. Combat ships all fly at ENEMY_SCALE; the cutscene ship is
    // blown up so a hero shot can be framed without putting the camera inside
    // the near plane. At combat scale a fighter is about five pixels across at
    // any distance you could safely hold it, which is a contact, not a subject.
    float   scale;

    // Identity, and the ribbon that carries it. trail_p is the throttle setting
    // each point was laid down at, 0..255 -- what makes the contrail lengthen
    // under power and persist after the ship has backed off.
    float   hue;
    float   trail_acc;
    uint8_t trail_n;
    uint8_t trail_head;
    uint8_t trail_p[SHIP_TRAIL];
    Vec3    trail[SHIP_TRAIL];
};

// What a player missile did, shown briefly in the middle of the screen. A
// proximity-fused seeker is otherwise ambiguous -- a detonation nearby looks
// identical whether it took a third of their hull or nothing at all.
enum MslEvent : uint8_t {
    MSL_NONE = 0,
    MSL_MISSED,
    MSL_HIT,
    MSL_DESTROYED
};

// How long an outcome holds. Shortened when others are waiting, so a burst of
// four resolves in about two and a half seconds rather than five.
#define MSL_BANNER      1.10f
#define MSL_BANNER_FAST 0.62f

struct Missile {
    bool    alive;
    bool    from_player;
    bool    locked;       // false once the seeker has broken lock: goes ballistic
    // The warhead that launched this round. A pointer into static storage, so it
    // stays valid even after the ship that fired it has been destroyed -- which
    // matters, because a missile routinely outlives its launcher.
    const ShipSpec* spec;
    Vec3    pos;
    Vec3    dir;          // unit heading
    float   life;
    float   age;
    int     target;       // enemy index; -1 means the player
    float   last_range;   // for the proximity fuse
    float   trail_acc;
    uint8_t trail_n;
    uint8_t trail_head;
    Vec3    trail[MISSILE_TRAIL];
};

struct Debris {
    bool  alive;
    Vec3  pos;
    Vec3  seg;
    Vec3  vel;
    float life, life0;
};

// Screen flow:
//
//   ATTRACT --tap--> SELECT --confirm--> BRACKET --ready--> PLAYING
//                                           ^                  |
//                        won a round, not the final            |
//                                           +------ ROUND_WON -+
//
// PLAYING drops to PAUSE on the alt button (quit from there returns to
// ATTRACT), to OVER when the player dies, and to WON on taking the final.
enum VgState : uint8_t {
    VG_ATTRACT = 0,
    VG_ENTRY,             // callsign and trail colour
    VG_SELECT,            // ship selection -- once per tournament, then locked
    VG_REPAIR,            // spend credits on hull, reached from the bracket
    VG_BRACKET,           // the tournament map
    VG_INTRO,             // launch cutscene: both fighters introduced
    VG_PLAYING,
    VG_HIT,               // brief invulnerable pause after taking a hit
    VG_KILL,              // opponent is down and talking; player cannot be hurt
    VG_PAUSE,
    VG_ROUND_WON,         // beat between winning a match and the bracket redraw
    VG_OVER,              // knocked out -- the run is finished
    VG_WON                // took the whole tournament
};

// --- launch cutscene schedule ----------------------------------------------
// Camera adrift, then each fighter flown across the view in turn, with a hard
// cut between them. Skippable, because the fifth time through it is furniture.
// Paced to settle the player rather than to get out of the way. Two seconds of
// empty space first, then four and a half on each fighter -- long enough to
// look at it, which is the point of introducing it at all.
#define INTRO_DRIFT     2.2f
#define INTRO_YOU_END   6.6f
#define INTRO_OPP_START 7.0f
#define INTRO_OPP_END  11.4f
#define INTRO_END      11.8f

// How long the instruments take to come up once the cockpit is back.
#define HUD_BOOT_TIME   1.5f

// After the opponent goes down. Two distinct beats, and they do different jobs.
//
// KILL_SPEECH is how long their last transmission stays up. It is the only line
// in the game the player cannot provoke a second time, so it is given room.
//
// KILL_REFLECT is the silence afterwards -- cockpit, wreckage, nobody talking.
// Cutting from a death straight to a scorecard is what makes a kill feel like a
// score, and the quiet is the only thing that stops it.
//
// The player is invulnerable for both, and free to fly through what is left.
#define KILL_SPEECH     5.6f
#define KILL_REFLECT    4.6f
#define KILL_BEAT       (KILL_SPEECH + KILL_REFLECT)

// Victory sequence schedule. The state machine owns it because it decides when
// the state ends; the overlay only draws to it. Each beat finishes before the
// next begins -- overlapping them means neither is ever fully present and the
// moment has nothing to land on.
#define WON_RUMOR_IN    4.2f    // result has cleared; the rumour fades up
#define WON_NAME_IN     8.8f    // rumour has been read; the name takes over
#define WON_RETURN     15.0f    // name has stood alone; back to the menu

// Menu states run the attract autopilot underneath and draw no instruments.
// States that carry no instruments. The cutscene and the death sequence are in
// here for the same reason the menus are: there is no cockpit to report from.
static inline bool vg_state_is_menu(VgState s) {
    return s == VG_ATTRACT || s == VG_ENTRY   || s == VG_SELECT ||
           s == VG_BRACKET || s == VG_REPAIR  || s == VG_INTRO  ||
           s == VG_ROUND_WON || s == VG_WON   || s == VG_OVER;
}

struct VgGame {
    VgState  state;
    float    state_t;

    int      score;
    int      kills;
    float    difficulty;

    // The player's ship, chosen once and flown for a whole tournament.
    ShipClass       ship;
    const ShipSpec* spec;

    // Identity. Three characters like every other entrant, and a hue -- the one
    // thing in the game allowed to carry colour.
    char     callsign[4];
    float    trail_hue;      // 0..1

    // Persistent across runs. NOT reset by starting a tournament: this is the
    // bank, and losing it would defeat the point of a meta-currency.
    int      credits;

    // Set once a tournament has been taken, and never cleared. The rumour in
    // the hangar bays is about YOU from then on -- the intro crawl changes to
    // match, which is the whole point of the name.
    bool     champion;

    float    health;       // absolute hull points remaining
    float    health_max;   // ...out of this, from the chosen class

    float    throttle;    // smoothed by the flight model, 0..1
    // A second, much faster-tracking copy of the same command, used only by the
    // HUD warp. The flight model is deliberately sluggish so committing to speed
    // costs something; the canopy should still flex the moment you move the
    // slider, or the control feels unresponsive even though the ship is behaving
    // exactly as intended.
    float    throttle_vis;
    float    speed;
    float    agility;     // turn-rate multiplier from throttle
    float    bank;        // cosmetic roll, applied at projection only
    // TRUE roll, folded into the world transform, so it composes with pitch and
    // yaw instead of merely rotating the finished image. Accumulated because the
    // backdrop is not carried by that transform and has to be told the total.
    float    roll;
    float    shake;
    float    shake_x, shake_y;
    float    hit_flash;

    // Weapons
    int      missiles;         // rounds remaining
    float    reload_t;
    float    fire_gap;
    int      lock_target;      // enemy index, or -1
    float    lock_t;           // time held inside the nose cone
    float    lock_need;        // time required at the current speed
    bool     locked;

    MslEvent msl_event;
    float    msl_event_t;  // counts down; the banner shows while positive
    // Every round the player fires must resolve into something the player is
    // told. A single slot dropped outcomes whenever two shots landed close
    // together, so pending ones queue behind the one on screen instead.
    MslEvent msl_queue[6];
    uint8_t  msl_qn;

    // Radio. One line at a time, held by priority so a death cry is never
    // stepped on by the next round going wide.
    char        comms_tag[4];
    const char* comms_line;
    float       comms_t;
    uint8_t     comms_pri;
    float       taunt_t;   // countdown to the next unprompted remark

    // The player's own ribbon. Visible because the world counter-rotates around
    // a fixed camera, so a hard turn sweeps your own track into view -- you can
    // see the arc you just flew.
    float    trail_acc;
    uint8_t  trail_n;
    uint8_t  trail_head;
    uint8_t  trail_p[SHIP_TRAIL];
    Vec3     trail[SHIP_TRAIL];

    // A ship nobody is flying: the one shown crossing the view during the
    // launch cutscene, and the wreck the camera tumbles around after a death.
    // Kept apart from the enemy array so neither the AI nor the collision pass
    // can ever see it.
    Ship     cine;
    bool     cine_on;

    float    hud_boot;     // >0 while the instruments are coming up

    float    wall_clear;   // distance to the arena boundary, recomputed each frame

    // Threat state, recomputed each frame for the HUD
    bool     threat;
    float    threat_range;
    Vec3     threat_pos;

    float    spawn_t;

    Ship     enemy[MAX_ENEMIES];
    Missile  msl[MAX_MISSILES];
    Asteroid ast[MAX_ASTEROIDS];
    Debris   deb[MAX_DEBRIS];
    Vec3     star[NUM_STARS];
    uint8_t  star_b[NUM_STARS];
    Vec3     mote[NUM_MOTES];      // near-field dust, for a sense of speed
};

extern VgGame   vg;
extern AstModel vg_models[NUM_MODELS];

// Enemy fighter, in local space (+x right, +y up, +z forward).
//
// Verts 0..4 form a CLOSED hull (nose apex over a four-vertex rear ring), which
// is what hidden-line rendering needs -- the old open skeleton had no faces to
// fill. The tail fin is a flat blade with no volume, so it stays wireframe and
// is drawn over the hull.
#define SHIP_VERTS     7
#define SHIP_FACES     6
#define SHIP_FIN_EDGES 3
extern const Vec3 vg_ship_verts[SHIP_VERTS];
extern uint8_t    vg_ship_faces[SHIP_FACES][3];      // wound outward at init
extern const uint8_t vg_ship_fin[SHIP_FIN_EDGES][2];

// Build the render basis for a ship: columns are right/up/forward, with the
// visual roll folded in.
Mat3 vg_ship_basis(const Ship* s);

void vg_game_init(void);

// Begin a fresh tournament with the chosen ship: full hull, new bracket.
void vg_tournament_begin(ShipClass c);

// Set up the next match against whoever the bracket says. Hull is NOT restored
// here -- damage carries between rounds, and only credits will ever undo it.
void vg_match_start(void);

void vg_game_select_ship(ShipClass c);

// Credits paid out for the most recent round win, for the ROUND WON card.
int  vg_last_purse(void);
void vg_game_update(float dt, const VgInput* in);
