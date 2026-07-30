#pragma once

// ===========================================================================
// Missiles, player weapons and enemy behaviour.
//
// Anything that VARIES BY SHIP CLASS -- missile speed, turn rate, life, damage,
// magazine, reload, lock range and time -- now lives in ShipSpec (vg_ship.h).
// What is left here is the rules that apply to every ship equally.
// ===========================================================================

// --- missiles --------------------------------------------------------------
#define MAX_MISSILES         14
#define MISSILE_TRAIL        30       // trail sample points per missile
#define TRAIL_SAMPLE_DT      0.028f   // seconds between trail samples

// cos(60 deg). Once the bearing to the target leaves the seeker's cone the lock
// breaks for good and the missile coasts ballistic -- this is what produces the
// "whizzes past" miss instead of an infinite chase.
//
// Universal on purpose: a seeker cone that varied by class would make some
// missiles simply undodgeable, and evasion has to stay a skill the player can
// rely on against every opponent in the bracket.
#define MISSILE_SEEKER_COS   0.50f

// Widened with the speed rebalance. A missile closing head-on shuts the range at
// up to 760 units/sec -- about 12 units per frame -- so a tight hit radius would
// be stepped straight over between samples.
//
// This is also the falloff scale: a detonation at 0 does full warhead damage,
// one out at the rim does ShipSpec::msl_graze_floor of it.
#define MISSILE_HIT_RADIUS   18.0f
#define MISSILE_ARM_TIME     0.22f    // no lead correction while clearing the rail

// --- player weapons --------------------------------------------------------
#define PLAYER_FIRE_GAP      0.5f
#define PLAYER_LOCK_COS      0.86f    // cos(~31 deg) nose cone to acquire lock

// Lock time scales with speed: at full throttle it takes (1 + this) times as
// long, far more than the geometry will hold a target in the cone for. This is
// the mechanism that puts high speed OUT of effective engagement range -- and
// because it is re-evaluated every frame, accelerating also drops a lock you had.
#define LOCK_SPEED_PENALTY   4.5f

// --- enemy -----------------------------------------------------------------
#define MAX_ENEMIES          2

// NPCs fly the same four classes the player picks from, so their speeds, turn
// rate, hull and weapons all come from ShipSpec. What is left is the AI's own
// character, expressed as fractions of its ship's capability -- which means a
// BALLISTA automatically fights from much further out than a CHARIOT without
// the behaviour code knowing anything about ship classes.
//
// This is a placeholder for real per-archetype behaviour. It stops a BALLISTA
// throwing away its range advantage, but it will not make one FIGHT like a
// gunner. See DESIGN.md.
#define ENEMY_SKILL          0.82f    // turn-rate scale; 1.0 = as good as a player
#define ENEMY_FIRE_RANGE_K   0.875f   // fraction of its own lock range
#define ENEMY_CLOSE_RANGE_K  0.94f    // ...at which it settles to a fighting speed
#define ENEMY_FIRE_GAP_K     1.30f    // fraction slower to shoot than its reload

#define ENEMY_FIRE_COS       0.90f

// Enemies live under the same rule as the player: flat out, they cannot shoot.
// Without this they would disengage AND keep firing, which strictly beats every
// option the player has.
#define ENEMY_ENGAGE_SPEED   0.55f    // fraction of max, above which they hold fire

#define ENEMY_SCALE          7.0f
#define ENEMY_HIT_RADIUS     16.0f
// Far enough that a match opens with a search rather than a merge. At 1300 the
// two of you were on top of each other before the HUD had finished coming up;
// out here you have to hunt, the radar earns its place, and the first contact
// is something you flew to rather than something you woke up in.
//
// They will find each other regardless -- the AI closes on the player, so the
// distance costs a few seconds of approach, not a stalemate.
#define ENEMY_SPAWN_DIST     2900.0f
#define ENEMY_EVADE_RANGE    540.0f   // break when a missile gets this close

// Enemies aim at a point offset from the player rather than at the player, so
// even a perfectly flown pursuit curve produces a firing pass instead of a
// collision. Inside ENEMY_BREAK_RANGE they abandon the attack and extend away.
#define ENEMY_OFFSET         150.0f   // lateral aim offset, world units
#define ENEMY_BREAK_RANGE    560.0f
#define ENEMY_BREAK_TIME_MIN 1.1f
#define ENEMY_BREAK_TIME_MAX 1.8f

// --- suicide runs ----------------------------------------------------------
// Some pilots will trade their ship for yours. The willingness is rolled once
// per pilot at spawn, so a given opponent either is that sort or is not, and it
// stays true for the whole match. Rolling it per frame would make every enemy
// occasionally suicidal, which reads as a bug rather than as a character.
#define ENEMY_KAMIKAZE_CHANCE   0.35f   // of pilots who would do it at all

// A willing pilot commits when their hull is this low. They are going to die to
// the next hit anyway, so the ship stops being an asset to protect and becomes
// the largest weapon they have left.
#define ENEMY_KAMIKAZE_HULL     0.34f

// ...and only from inside this range, so it reads as a decision made during a
// fight rather than a behaviour they spawned with.
#define ENEMY_KAMIKAZE_RANGE    1800.0f

// Aim point for the run. Not the origin: the player IS the origin, and a ship
// that converges perfectly on it decelerates into a stern chase it cannot win.
// Aiming slightly beyond means the closing speed is still rising at contact.
#define ENEMY_KAMIKAZE_LEAD     140.0f

// --- incoming missile alert -------------------------------------------------
// The alert used to appear at 260 units, which against a missile closing at
// several hundred a second is under a second of warning -- an alert that only
// tells you what already happened.
//
// It now starts far enough out to be acted on, and its cadence carries the
// range: a double beat that gets faster as the seeker closes, and then stops
// beating and stays lit once evasion is no longer the question.
#define MSL_ALERT_RANGE      900.0f   // warning starts here
#define MSL_ALERT_SOLID      190.0f   // steady from here in: no longer blinking

// The cadence is ALERT_FLASH_* in cfg_hud.h, shared with the boundary alert.
// This one used to have a double-beat shape of its own, which is the more
// authentic thing and read as a flicker on a 480x480 panel held at arm's length.
// One flash, accelerating, is what the boundary does and it works.
