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

// LEVEL OF DETAIL ALONG THE ARC, and this was the one thing in the world with none.
//
// Everything else already steps down with distance or size: the ship ribbon walks 1, 2 then
// 3 points at a time, asteroids drop through a wireframe to a diamond to a dot, and
// fireballs shed ring segments. The missile trail walked all thirty points at full
// resolution, and the head is stroked two pixels wide, which is two primitives a segment.
// Fourteen missiles of that is the densest geometry in the frame at exactly the moment the
// most is happening.
//
// Measured over a missile-heavy session: `ord` means 103 us and peaks at 1891, an
// eighteenfold swing and the sharpest curve in the world split.
//
// Full resolution near the HEAD, because that is the part being read -- the arc says where
// the round came from and where it is going -- and coarser behind it, where the trail is
// dimmer and tapered to a hairline anyway. The ribbon stays unbroken because each segment
// starts where the last one ended, whatever the step.
#define MISSILE_LOD_NEAR     9        // points from the head drawn one for one
#define MISSILE_LOD_MID      18       // ...then every second, then every third
#define TRAIL_SAMPLE_DT      0.028f   // seconds between trail samples

// THE SEEKER CONE lives in ShipSpec::msl_seeker_cos, per class. The constant that
// used to sit here was cos(60 deg) and had not been read by anything for some
// time, but the reasoning attached to it is still the reason the classes barely
// differ on the value, so it stays with the rule rather than with the number.
//
// Once the bearing to the target leaves the cone the lock breaks and the missile
// coasts ballistic. That is what produces the "whizzes past" miss instead of an
// infinite chase -- and a cone wide enough to be unshakeable makes evasion stop
// being a skill. Evasion has to stay something the player can rely on against
// every opponent in the bracket.
//
// BALLISTA gets its tenacity from RE-ACQUISITION instead, which costs the player
// nothing they had already earned: the lock still breaks, the round still sails
// past, the dodge still works. It simply comes back for another pass. Beating it
// once and forgetting about it is the only thing that stops working.

// THE COAST BEFORE A RETRY is ShipSpec::msl_reacq_delay, per class, and the fuse
// radius and the lead-correction delay are msl_splash and msl_arm_time beside it.
// They moved because a warhead is a property of the ship that launched it, and
// because msl_splash in particular is now a real point of difference: it is the
// radius the graze floor applies OVER, so a global one was leaving half of the
// LANCE-versus-CHARIOT idea unsaid.

// --- player weapons --------------------------------------------------------
// Rate of fire is ShipSpec::fire_gap now, not one number for everybody: it is
// most of what separates CHARIOT's twelve-round dump from BALLISTA's three
// deliberate shots, and a class cannot have a firing style if the game owns the
// trigger. The nose cone went the same way, as ShipSpec::lock_cos.

// Lock time scales with speed: at full throttle it takes (1 + this) times as long
// to ACQUIRE. Holding is unaffected -- see the latch in update_lock.
//
// Was 4.5, which at full throttle meant 5.5x the base time: far more than the
// geometry will hold a target in the cone for, so the number did not read as
// "harder at speed" but as "impossible at speed". Combined with a turn rate 2.5x
// better at idle, that made stopping the only way to fight, which is exactly what
// the first playtest reported back.
//
// 1.8 keeps the trade legible -- an AEGIS wants 0.45s at rest and 1.26s at full,
// which is a real cost you can still choose to pay -- without deciding the fight
// on the player's behalf.
#define LOCK_SPEED_PENALTY   1.8f

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
// Fraction slower to shoot than the class's own sustained rate.
//
// Raised from 1.30 when the player's magazine became a whole-clip affair. The
// sustained rate works out shorter than the old per-round reload for every class,
// so at 1.30 every enemy would have silently got faster on the trigger.
//
// 2.02 holds the AVERAGE cadence across the four classes at what it was before
// fire control became per-class. It does NOT hold any individual class there, and
// no single number could -- the classes have moved relative to each other, which
// is the point. It is still a balance shift and not a neutral one:
//
//   class      before   after
//   AEGIS        4.16    2.78
//   LANCE        5.20    4.54
//   CHARIOT      1.82    2.07
//   BALLISTA     7.80    9.59
//
// -- so an enemy AEGIS is the one that got meaningfully harder, and an enemy
// BALLISTA the one that got easier. Retune this whenever a class's magazine,
// trigger or reload moves; it is derived from all three.
#define ENEMY_FIRE_GAP_K     2.09f

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
//
// THIS IS A REQUEST, NOT A DISTANCE. The spawn is placed along a ray and then
// vg_arena_clamp_inside pulls it back toward the tube centreline, because a point
// far along a straight line leaves a torus of r_major 4200 / r_minor 1100 very
// quickly. So the number here is always larger than what the match gets, and it
// SATURATES -- the arena runs out of room to put anyone. Measured against the real
// spawn distribution, with the player on the centreline:
//
//     request   p10    median   p90
//       2900   2179     2616   3243
//       3800   2569     3202   4022
//       5400   3078     3979   5008
//       6500   3342     4342   5456
//       8000   3599     4711   5883
//
// 6500 for a median of 4342, which is where a BALLISTA match opens at about its
// own 4200 lock range -- often just outside it, so there is a moment of hunting
// before the first contact. Asking for more buys almost nothing: 8000 is 1500
// more on paper and 370 more in the arena.
#define ENEMY_SPAWN_DIST     6500.0f
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
#define MSL_ALERT_RANGE      900.0f   // warning starts here, and only flashes

// The cadence is ALERT_FLASH_* in cfg_hud.h, shared with the boundary alert.
// This one used to have a double-beat shape of its own, which is the more
// authentic thing and read as a flicker on a 480x480 panel held at arm's length.
// One flash, accelerating, is what the boundary does and it works.
