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
};

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

enum VgState : uint8_t {
    VG_ATTRACT = 0,
    VG_PLAYING,
    VG_HIT,               // brief invulnerable pause after taking a hit
    VG_OVER
};

struct VgGame {
    VgState  state;
    float    state_t;

    int      score;
    int      kills;
    float    difficulty;

    // The player's ship, chosen once and flown for a whole tournament.
    ShipClass       ship;
    const ShipSpec* spec;

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
    float    bank;        // cosmetic roll
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
void vg_game_start(void);
void vg_game_select_ship(ShipClass c);
void vg_game_update(float dt, const VgInput* in);
