#pragma once

// ===========================================================================
// Missiles, player weapons and enemy behaviour.
//
// Anything that VARIES BY SHIP CLASS -- missile speed, turn rate, life, damage,
// magazine, reload, lock range and time -- now lives in ShipSpec (vg_ship.h).
// What is left here is the rules that apply to every ship equally.
// ===========================================================================

// --- missiles --------------------------------------------------------------
// Sized so the WORST HONEST CASE fits, which it did not.
//
// A CHARIOT magazine is twelve and it empties in under two seconds, so a player
// who does that leaves two slots for everything else in the game. A BALLISTA
// opponent carries three, and its rounds stay alive for thirty-two seconds --
// including after they have lost the lock and gone ballistic, because losing a
// lock does not shorten the fuse. Twelve and three is fifteen against fourteen.
//
// WHAT THAT LOOKS LIKE FROM THE COCKPIT: press fire, and nothing happens. The
// launch fails, the round is correctly not spent, and there is no sound and no
// message because no missile ever existed to report on. Reported from the board
// as being unable to fire after taking a hit, flying exactly that matchup.
//
// Instrumented, a bot that cannot even empty a CHARIOT rack still drove the pool
// to eleven of fourteen. A person emptying one reaches twelve on their own.
//
// Twenty leaves room for a full player rack plus both opponents holding theirs.
#define MAX_MISSILES         20
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
// gunner. See design/notes/, which is the authority on intent.
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

// WHAT A COLLISION COSTS, as a fraction of the victim's own full hull.
//
// It used to cost the match. vg_states.cpp called vg_kill_player on any contact,
// no hull check -- so a ram was not a threat, it was an outcome, and a pilot who
// had decided to ram had already won. Reported from the cockpit: "what's meant for
// a desperation move for some pilot ends up being the thing that kills me every
// time... it's too cheap."
//
// At 0.55 a ram takes more than half of anything it hits. It is survivable at full
// hull and lethal to somebody already hurt, which is what a desperation move
// should be: the pilot trades their own life for most of yours, and whether that
// finishes you depends on the fight so far rather than on the contact itself.
//
// The rammer still dies outright. They are under ENEMY_KAMIKAZE_HULL by the time
// they commit -- a third of a hull -- so this kills them anyway, and saying so
// here is cheaper than a special case.
#define SHIP_COLLIDE_DAMAGE  0.55f
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

// HOW MUCH A SLOW TARGET SHORTENS THE BREAK, as a fraction taken off the break
// range at a dead stop.
//
// The break exists for two reasons and a parked ship defeats both. It stops a
// merge becoming a collision -- but something that is not moving is not going to
// run into anybody. And it stops a faster ship cornering you -- but a ship at
// idle cannot chase what it just let go.
//
// So against somebody who has stopped, breaking off is not caution, it is a gift:
// it hands them the seconds they need to line the next shot up, which is exactly
// the loop a human finds in about a minute. Cut the throttle, hold the nose on
// them, fire, and wait for the enemy to politely leave and come back.
//
// The AI knew the player's speed in exactly ONE place before this -- whether to
// run or turn with somebody already on its six -- and nowhere in the decision
// that mattered.
#define ENEMY_PRESS_SLOW_K   0.70f

// HOW SLOW A TARGET HAS TO BE before the AI stops flying a pattern and takes an
// aimed shot at it, as a fraction of that ship's own speed range.
//
// An enemy's aim_dir IS its nose -- see update_aim, which only adds the pilot's
// error to s->fwd. So it can only lock what it happens to be flying at, and what
// it flies is a positioning curve that deliberately misses. Measured, it holds
// its aim 76 degrees off target on average and has a lock, a round and a cool
// trigger together on 0.7% of frames. It never TAKES a shot; it flies, and now
// and then the curve points the right way.
//
// A moving target is worth flying a curve at, because a straight approach is
// where they get their own shot. A parked one is not: it cannot punish the
// approach and it cannot leave. So below this, the ship stops manoeuvring, slows
// down enough for the lock to be quick, and points at them -- which is exactly
// what the player is doing to it.
#define ENEMY_PRESS_SLOW_AT  0.30f

// The same test for a hull that closes for a living. It is higher because those
// classes give up nothing by taking an aimed shot -- they were going to be in
// close anyway -- so they should come for anybody who is loitering, not only for
// somebody who has fully stopped.
//
// A STANDOFF hull keeps the lower number on purpose and it is not a tuning
// choice. Measured at 0.55 it left its range against ordinary flying and its
// damage went 0.55 -> 2.04, which is a sniper quietly becoming a brawler for the
// second time in one evening. It abandons its distance for a target that has
// actually parked, and for nothing less.
#define ENEMY_PRESS_SLOW_CLOSE 0.55f

// THE FIRING PASS: how long a ship commits its nose to a target, and how long it
// waits before lining up another.
//
// The run ends by itself, or the break at ENEMY_BREAK_RANGE ends it -- which is
// what makes the cycle line up, shoot, break, come back rather than one long
// converging chase. The cooldown is what stops the break turning straight back
// into a run and producing a merry-go-round.
#define ENEMY_ATTACK_TIME_MIN 1.4f
#define ENEMY_ATTACK_TIME_MAX 2.4f
#define ENEMY_ATTACK_COOLDOWN 2.2f
// How far out it is worth lining one up, as a fraction of the ship's lock range.
// Beyond this the round spends too long in the air for a moving target to still
// be there.
#define ENEMY_ATTACK_RANGE_K  0.90f
// ...and how close is too close to bother lining one up. Below this it is already
// in the merge and the break owns the next second, not the aim. Deliberately far
// under ENEMY_BREAK_RANGE: fights settle around 400 units, and a window that
// started at the break range would almost never open.
#define ENEMY_ATTACK_MIN      180.0f

// THE HARD FLOOR ON A MERGE, below which a ship bends away whatever else it had
// planned.
//
// Contact is INSTANT DEATH for the player -- vg_states.cpp calls vg_kill_player
// on any touch, no hull check -- so the cost of an accidental merge is not a
// dent, it is the match. The break at ENEMY_BREAK_RANGE was supposed to prevent
// this, but it lives inside the class tactic and anything above the tactic
// preempts it: evading a missile, answering a tail, or a firing pass. Played,
// that reads as the enemy laying a trap.
//
// So this sits high in the chain instead, under only the wall and a suicide run
// -- the wall is also fatal, and a pilot who has DECIDED to ram is doing this on
// purpose and has already announced it. The network has had exactly this floor
// since it started flying; the tactics never did.
#define ENEMY_MERGE_FLOOR    150.0f

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

// --- cornered ---------------------------------------------------------------
//
// A damaged pilot COMES AT YOU instead of nursing the wreck.
//
// WHY THIS HAD TO BE A RULE. A cloned policy copies its teacher's caution along
// with their skill, and the recordings are unambiguous about what a person does
// when hurt: over 70% hull they close at +0.082 and run the throttle at 0.34;
// under 35% they close at +0.046 and 0.28. Half the closing rate. So the moment
// the player damaged a trained opponent it stopped coming, and the fight got
// EASIER exactly when it should have got worse. Reported from the cockpit as the
// threat dropping once the upper hand was gained.
//
// AND THERE WAS NOWHERE FOR IT TO GO. The game already had "back off" and it had
// "ram them at 34% hull" -- which is 18% of pilots and a suicide run, not a fight.
// The whole space between was empty, and this is that space.
//
// NOT A SUICIDE RUN. It still obeys every firing gate, still evades a missile,
// still turns away from the wall. It presses: nose on, closing, and willing to
// take the exchange. The kamikaze is untouched and still sits below it.
#define ENEMY_CORNERED_HULL  0.50f   // fraction of hull below which it presses
// How much of the way to turn from wherever it wanted to point, toward the
// player. Not all the way: a cornered pilot commits, it does not stop flying.
#define ENEMY_CORNERED_PULL  0.55f
// ...and the speed it asks for, as a fraction of its own span. Under the firing
// gate on purpose, so pressing does not cost it the ability to shoot.
#define ENEMY_CORNERED_SPEED 0.45f

// --- breaking a stalemate ---------------------------------------------------
//
// How long a fight may go with nobody landing anything before one pilot throws
// the geometry away and re-merges from somewhere else.
//
// A TRAINED PILOT CANNOT NOTICE THIS BY ITSELF. Cloning teaches what somebody
// did in each situation; it never teaches that the situation has repeated forty
// times. The hand-written tactics break off after every pass and come back from
// a new angle, which is what keeps a fight moving, and a network fitted to a
// pilot who does not disengage simply holds the orbit.
//
// Twelve seconds, against a median quiet stretch of three in a CHARIOT fight --
// so it fires on a stall and not on an ordinary lull between passes.
// HOW LONG A SEMI-ACTIVE ROUND KEEPS GUIDING AFTER ITS LAUNCHER STOPS LOOKING.
//
// Without this the mechanic is unflyable by anything that must also avoid the
// arena: measured, an enemy BALLISTA put 74 of 77 rounds into the wall and NOT
// ONE was still guided when it stopped, because the boundary outranks every
// other priority and a wall turn is exactly what breaks the lock. The tube is
// 1100 units in radius and BALLISTA locks at 4200, so it is nearly always inside
// the margin -- it does not get to choose a moment.
//
// A coast is the honest fix rather than a concession: a real semi-active weapon
// flies on its last solution when the illumination drops, and it is the seconds
// of a break rather than an abandonment that this has to cover.
//
// IT DOES NOT ACCELERATE WHILE DARK. "The aim is the engine" still holds -- a
// round that is not being fed keeps what it earned and no more.
#define MSL_COAST_TIME       1.4f

// HOW HARD SPEED IS PAID FOR IN AGILITY, as an exponent on launch_speed/speed.
//
// A missile pulling a fixed sideways acceleration turns at a/v: going faster
// necessarily means turning worse, and the turn radius grows with the square of
// the speed. That is real, and it is also exactly the trade the classes want --
// a round is fastest when it is least able to correct and nimblest when it is
// slow, so the acceleration a pilot earns by holding the lock is spent on
// commitment.
//
// It only touches a class that accelerates. The other two fly at one speed, so
// the ratio is 1 and nothing changes.
//
// 1.0 is the honest physics. Lower blends toward the flat turn rate the table
// states, and it is a dial because the honest answer takes a LANCE round to 1.06
// at full speed -- under the 1.20 that made the class useless before its seeker
// was fixed.
#define MSL_TURN_TRADE       1.0f

// HOW LONG A MODE MUST SURVIVE before the network may pick another, in seconds.
//
// This is the whole of the policy's memory, and it is supplied here rather than
// learned. The network is a function of one frame: asked sixty times a second it
// answers sixty times, and a plan that can be abandoned on the next frame is a
// reflex wearing a plan's clothes.
//
// Measured across every recording, a pilot's own mode lasts a median of 1.8
// seconds once the labels are smoothed. A floor of one second sits under that on
// purpose: it should stop the flicker, not stop the pilot changing their mind.
#define VG_MODE_DWELL        1.0f

#define ENEMY_STALE_TIME     12.0f
#define ENEMY_RESET_MIN      1.4f    // seconds committed to the re-merge
#define ENEMY_RESET_MAX      2.2f

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
// RETIRED, AND KEPT ONLY FOR THE NUMBERS.
//
// These four -- the in-flight caps, ENEMY_FIRE_RANGE_K and ENEMY_ENGAGE_SPEED --
// used to gate an enemy's trigger, and the player was never subject to any of
// them. That made every balance question two questions, and it made an opponent
// that was inconsistent rather than hard: none of it was visible from the
// cockpit or written in the class table.
//
// The trigger now asks what the player's asks and nothing more: rounds in the
// rack, a cooled trigger, a lock. Anything that should restrain a class goes in
// the class table, where it binds both seats.
//
// The values stay here because they are TUNED, and because the judgements behind
// them were sound even though the place they lived was not. If a class needs a
// slower burst, the honest form of that is its own fire_gap.
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
// HOW MUCH GROUND A RUN HAS TO GAIN to have been worth making, in world units,
// and how long the ship stops trying when it does not.
//
// A standoff hull is the slowest thing in the game and it flees from ships that
// are faster, so running is not always available to it. A pilot who has just
// discovered that turns and fights instead of repeating it, which is the
// difference between a plan and a loop.
#define STANDOFF_FLEE_GAIN   220.0f
#define STANDOFF_FLEE_GIVEUP 4.0f

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
#define ENEMY_KAMIKAZE_CHANCE   0.09f   // of pilots who would do it at all

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
