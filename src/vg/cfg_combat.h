#pragma once

// ===========================================================================
// Missiles, player weapons and enemy behaviour.
// ===========================================================================

// --- missiles --------------------------------------------------------------
#define MAX_MISSILES         14
#define MISSILE_TRAIL        30       // trail sample points per missile
#define TRAIL_SAMPLE_DT      0.028f   // seconds between trail samples

// Sits at ~75% of the ship's 100..420 range, so you only outrun one near full
// throttle. This has to be re-derived whenever the speed range moves: at the old
// 200 against a 100 minimum, a third of the throttle would have been enough to
// escape any missile, which would have collapsed the whole tactical trade.
#define MISSILE_SPEED        340.0f

// The balance point of the whole game. A missile that can out-turn anything is
// undodgeable; one that cannot is decoration. Tuned so a hard break at the right
// moment defeats it and a lazy turn does not.
#define MISSILE_TURN_RATE    2.5f     // rad/sec
#define MISSILE_LIFE         10.0f

// cos(60 deg). Once the bearing to the target leaves the seeker's cone the lock
// breaks for good and the missile coasts ballistic -- this is what produces the
// "whizzes past" miss instead of an infinite chase.
#define MISSILE_SEEKER_COS   0.50f
// Widened with the speed rebalance. A missile closing head-on now shuts the
// range at up to 760 units/sec -- about 12 units per frame -- so a tight hit
// radius would be stepped straight over between samples.
#define MISSILE_HIT_RADIUS   18.0f
#define MISSILE_ARM_TIME     0.22f    // no lead correction while clearing the rail

// --- player weapons --------------------------------------------------------
#define PLAYER_MISSILES      6
#define PLAYER_RELOAD        3.2f     // seconds per rearmed round
#define PLAYER_FIRE_GAP      0.5f
#define PLAYER_LOCK_COS      0.86f    // cos(~31 deg) nose cone to acquire lock
#define PLAYER_LOCK_RANGE    1600.0f
#define PLAYER_LOCK_TIME     0.45f    // seconds held in cone, at low speed

// Lock time scales with speed: at full throttle it takes (1 + this) times as
// long, far more than the geometry will hold a target in the cone for. This is
// the mechanism that puts high speed OUT of effective engagement range -- and
// because it is re-evaluated every frame, accelerating also drops a lock you had.
#define LOCK_SPEED_PENALTY   4.5f

// --- enemy -----------------------------------------------------------------
#define MAX_ENEMIES          2
#define ENEMY_SPEED_MIN      110.0f
#define ENEMY_SPEED_MAX      380.0f
#define ENEMY_TURN_RATE      1.55f
#define ENEMY_FIRE_COS       0.90f
#define ENEMY_FIRE_RANGE     1400.0f
#define ENEMY_FIRE_GAP       4.2f

// Enemies live under the same rule as the player: flat out, they cannot shoot.
// Without this they would disengage AND keep firing, which strictly beats every
// option the player has.
#define ENEMY_ENGAGE_SPEED   0.55f    // fraction of max, above which they hold fire

#define ENEMY_HP             3
#define ENEMY_SCALE          7.0f
#define ENEMY_HIT_RADIUS     16.0f
#define ENEMY_SPAWN_DIST     1300.0f
#define ENEMY_EVADE_RANGE    540.0f   // break when a missile gets this close

// Enemies aim at a point offset from the player rather than at the player, so
// even a perfectly flown pursuit curve produces a firing pass instead of a
// collision. Inside ENEMY_BREAK_RANGE they abandon the attack and extend away.
#define ENEMY_OFFSET         150.0f   // lateral aim offset, world units
#define ENEMY_BREAK_RANGE    560.0f
// Range at which they stop closing and settle to a speed they can fight at.
#define ENEMY_CLOSE_RANGE    1500.0f
#define ENEMY_BREAK_TIME_MIN 1.1f
#define ENEMY_BREAK_TIME_MAX 1.8f
