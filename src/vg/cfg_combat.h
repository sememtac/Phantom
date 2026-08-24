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
// THE TRIGGER HANDICAP IS GONE, AND THE MAGAZINE IS THE HANDICAP NOW.
//
// ENEMY_FIRE_GAP_K was here: a fraction slower than the class's own SUSTAINED
// rate -- magazine and reload averaged into one interval, a round sent on it for
// ever, and this constant holding the average cadence across the four classes at
// what it had been before fire control went per-class.
//
// It did that, and the averaging is what a playtest found. A class's cadence is
// not an average, it is a BURST and then nothing: CHARIOT sends twelve rounds in
// under two seconds and holds nothing for ten. Averaged it became a round every
// 2.07s against AEGIS's 2.78, so the glass cannon and the reference fighter were
// near enough the same on the trigger, and the deal a burst offers the player --
// live through the pass, own the next ten seconds -- did not exist to be taken.
//
// An enemy carries a real rack now (see Enemy::rounds), fires at its class's own
// sp->fire_gap, and sits out sp->reload when it runs dry. That is faster in the
// burst and slower over a minute, which is the shape the ships were designed with
// and the shape the player's own weapon has.
//
// NOTHING NEEDS RETUNING HERE WHEN A CLASS CHANGES ANY MORE, which was the other
// cost of the old constant: it was derived from magazine, trigger AND reload, so
// every table edit silently moved it.

// THE CONE AN ENEMY MAY HOLD A LOCK IN, when its class says "the viewport".
//
// lock_hold_cos below -1 means "held for as long as the target is on screen",
// which is an honest answer for the player and meaningless for an enemy: they
// have no screen. This is that rule in the only terms an enemy has -- the cone
// that reaches the CORNER of the player's panel, which the note on lock_hold_cos
// works out at about 40 degrees off axis. So a class whose lock survives anywhere
// in view keeps a lock that survives anywhere in the same solid angle.
#define ENEMY_LOCK_HOLD_WIDE 0.766f

// ENEMY_FIRE_COS WAS HERE, and it was the last piece of an enemy's fire control
// that did not come from the ship it was flying.
//
// One cone, 0.90, for every class, and no lock time at all: an enemy pointed
// roughly at the player could fire, where the player has to hold the nose on a
// target for lock_time to earn the same shot. On LANCE that is the whole class --
// 0.60s per acquisition, nearly a second and a half at a fighting speed -- so an
// enemy LANCE put four rounds in the air for an aim the player would have been
// charged four locks for. Reported from play as a volley that should not have
// been permissible.
//
// Enemies now run the player's own acquisition: the class's lock_cos to acquire,
// its lock_hold_cos to keep, its lock_range, and its lock_time scaled by speed
// through the same LOCK_SPEED_PENALTY. See enemy_update_lock.

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

// --- being chased ------------------------------------------------------------
//
// NOTHING USED TO ASK WHETHER ANYBODY WAS BEHIND THEM. Enemies reacted to a
// missile already in the air and to raw proximity, and to nothing else -- so a
// player who settled onto a tail was never answered. They flew their approach
// pattern and were shot off it, and the fight was over the moment it began.
//
// The counter to a tail is not a better turn. It is making the attacker CHOOSE.
// Parking at zero throttle buys the best turn rate in the game and the shortest
// lock, and costs the ability to follow anything -- so an enemy that simply
// leaves cannot be chased by a pilot who is parked. To give chase you have to
// open the throttle, and that is the trade the whole game is built on, finally
// pointed at the player instead of only at them.
#define ENEMY_SIX_RANGE      900.0f   // near enough that a tail is a threat
#define ENEMY_SIX_COS        0.15f    // bearing to the attacker, behind this is "behind me"
#define ENEMY_SIX_AIM_COS    0.80f    // ...and they are pointed this near to me
#define ENEMY_SIX_SLOW       0.35f    // attacker throttle under this: outrun them
#define ENEMY_DEFEND_MIN     1.3f     // seconds committed to the answer, so it is
#define ENEMY_DEFEND_MAX     2.2f     // a manoeuvre and not a twitch

// --- an empty rack ----------------------------------------------------------
//
// A pilot holding nothing should not be flying at you as though they were. Once
// an enemy carries a real magazine, running it dry is a state they are IN for
// several seconds, and a ship that keeps boring in with no weapon reads as an AI
// that cannot count -- it also throws away the deal a burst is supposed to offer,
// which is that surviving the pass buys the reload.
//
// ONLY WHILE THE PLAYER IS NEAR ENOUGH TO MATTER. A dry ship at long range is in
// no danger and has nothing to run from; making it run anyway would have BALLISTA,
// which is dry more often than not, spend the match with its back turned. Inside
// this it leaves; outside it, it goes on positioning.
#define ENEMY_DRY_RANGE      1700.0f
// Speed while extending on an empty rack. Flat out: there is nothing to slow down
// for, since the firing gate is the only reason an enemy ever flies slowly.
#define ENEMY_DRY_SPEED_K    1.00f

// --- pressing a won position ------------------------------------------------
//
// Every tactic breaks off on a RANGE test, and range alone cannot tell a merge
// that went well from one that went badly. A pass that ends with the enemy behind
// the player, nose on, is the one pass worth not finishing -- and it was exactly
// the pass they all abandoned, which is why fights read as a sequence of jousts
// with no consequence to losing one.
//
// The mirror of on_my_six, and deliberately STRICTER than it. A pilot should
// need a clearly won position to abandon the plan, where they need only a
// suspicion of a threat to answer one.
#define ENEMY_ANGLE_RANGE    1500.0f  // close enough for the position to be worth holding
#define ENEMY_ANGLE_BEHIND   0.35f    // how far into the player's rear hemisphere
#define ENEMY_ANGLE_NOSE     0.86f    // ...and how near their own nose the player sits
// Committed for this long once taken, so losing the angle for a frame -- which a
// hard turn does constantly -- does not drop the pursuit. It ends when the timer
// runs out and the test fails again, which is the player having actually escaped.
#define ENEMY_PRESS_TIME     1.5f
// A ceiling on how long one pursuit can run before they have to let go and
// re-merge. Without it a pilot who keeps just barely holding the angle never
// disengages, and an opponent that can never be shaken is not a fight.
#define ENEMY_PRESS_MAX      6.0f

// --- temperament ------------------------------------------------------------
//
// NERVE MOVED TO THE PILOT. It was a bare random rolled per ship here, between
// 0.80 and 1.30, which made two ships of the same class feel like two pilots but
// could not make a FIRST ROUND opponent feel different from a finalist -- the
// same range was drawn from at both ends of the bracket.
//
// It is a trait of the character now, in vg_pilot.h, alongside the two the game
// never had: how steadily somebody points, and how long they take to notice. The
// per-ship roll survives as a small jitter around the character's value, so an
// archetype decides the middle and the roll decides the person.

// --- how many of its own rounds a pilot will have in the air ----------------
//
// THE RATE LIMIT WAS NEVER THE PROBLEM, and it is worth writing down why, because
// it was measured twice. An enemy has always spent sp->fire_gap between rounds:
// instrumented, an enemy BALLISTA fires at 1.59s, 1.76s, 1.62s, 1.51s against a
// table value of 1.60 with a ten per cent jitter, then goes quiet for its nine
// second reload. Every class runs the same class-agnostic code.
//
// What a playtest saw was three rounds arriving together, and that is a different
// fault. BALLISTA's round flies at 200 and lives for thirty-two seconds, so three
// rounds fired 1.6s apart are ALL STILL IN THE AIR, converging from one bearing.
// The rate limit is obeyed and the effect is a salvo anyway, because the rate is
// fast relative to how long the ordnance lasts.
//
// That contradicts the class in its own words. vg_ship.cpp says a twenty-second
// flight means "the pilot has to pick a moment that will still be true when it
// gets there" -- and a pilot who empties the rack in 3.2 seconds is not picking
// moments, they are covering the possibilities. Semi-active guidance sharpened it
// further: all three ride ONE lock, so the aiming discipline the class is built
// around gets paid once and buys three rounds.
//
// So the discipline is HOW MANY ROUNDS OF MINE ARE STILL FLYING, which is the
// question a pilot would actually ask. A sniper fires and watches. A slasher
// empties the rack, because that IS the class. Zero means no limit.
#define ENEMY_INFLIGHT_STANDOFF  1   // fire, then watch it all the way in
#define ENEMY_INFLIGHT_FIGHTER   2
#define ENEMY_INFLIGHT_GEOMETRY  2   // clean hits only: a second round is the follow-up
#define ENEMY_INFLIGHT_SLASH     0   // no limit -- emptying the rack in one pass is the ship

// --- how each archetype positions itself ------------------------------------
//
// Everything above this is the fight EVERY enemy flies. These are the three that
// fly a different one, and they are dispatched on ShipSpec::tactic.
//
// All of them still obey the firing gates -- under ENEMY_ENGAGE_SPEED, and a LOCK
// earned in the class's own cone over the class's own lock_time -- because being
// slow and pointed at somebody to shoot them is the trade the whole game is built
// on, and a class that was exempt from it would not be a different tactic, it
// would be a different game.

// STANDOFF -- BALLISTA. Holds a band at a fraction of its own lock range, faces
// the player because the nose has to be on target to fire, and runs when
// anything gets inside. It cannot fly backwards, so it holds range by aiming
// PAST the player rather than at them: a wide arc that keeps the bearing inside
// the firing cone while closing slowly, instead of boring straight in.
#define STANDOFF_HOLD_K      0.72f   // x its own lock range: the band it wants
#define STANDOFF_FLEE_K      2.4f    // x ENEMY_BREAK_RANGE: inside this it runs
#define STANDOFF_ARC         0.30f   // x range: how far beside the player it aims

// SLASH -- CHARIOT. One fast pass, the rack emptied, and a long extend. It still
// has to slow to shoot, so the speed on the merge sits just under the gate
// rather than at it: fast enough to be a pass, slow enough to be allowed.
#define SLASH_OFFSET_K       1.8f    // x ENEMY_OFFSET: crosses rather than jousts
#define SLASH_BREAK_K        1.35f   // x ENEMY_BREAK_RANGE: turns away sooner
#define SLASH_SPEED_K        0.45f   // fraction of its speed span on the pass
#define SLASH_EXTEND_K       1.7f    // x the break time: stays away longer

// GEOMETRY -- LANCE. A 0.20 graze floor means only a clean hit pays, so this one
// wants alignment more than it wants angles: a smaller offset, a later break and
// a slower merge, all of which buy a steadier shot at the cost of being an
// easier target while it lines one up.
#define GEOM_OFFSET_K        0.45f   // x ENEMY_OFFSET: nearly head-on
#define GEOM_BREAK_K         0.75f   // x ENEMY_BREAK_RANGE: holds the merge longer
#define GEOM_SPEED_K         0.22f   // fraction of its speed span: slow and steady
#define ENEMY_BREAK_TIME_MIN 1.1f
#define ENEMY_BREAK_TIME_MAX 1.8f

// --- suicide runs ----------------------------------------------------------
// Some pilots will trade their ship for yours. The willingness is rolled once
// per pilot at spawn, so a given opponent either is that sort or is not, and it
// stays true for the whole match. Rolling it per frame would make every enemy
// occasionally suicidal, which reads as a bug rather than as a character.
//
// DOWN FROM 0.35, on a playtest report of dying to rams more often than the
// mechanic was meant to fire. At a third of all pilots it was not a character
// trait any more, it was the standard ending to a won fight.
//
// THE GYM MAKES IT LOOK WORSE THAN IT IS, and that is worth knowing before this
// number is touched again. A tournament run meets four opponents; the gym hands
// out a fresh pilot on every respawn, so a session there samples this dozens of
// times an hour where a run samples it four times. Judge the frequency in a
// tournament, not in the workshop.
#define ENEMY_KAMIKAZE_CHANCE   0.18f   // of pilots who would do it at all

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
