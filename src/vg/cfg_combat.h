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
// 3 points at a time, and fireballs shed ring segments. The missile trail walked all thirty points at full
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
// The opponent still dies outright, whatever its hull.
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
// So this sits high in the chain instead, under only the wall, which is also
// fatal. The network has had exactly this floor since it started flying; the
// tactics never did.
#define ENEMY_MERGE_FLOOR    150.0f

// THE SAME FLOOR AS A TIME, because a collision is a time problem and the floor
// above is a distance.
//
// At 150 units the floor above fired 0.25-0.4 s before contact in every
// accidental collision measured, and at 300-500 units a second of closing that
// is not long enough to turn out of. It had always fired; it had always been
// too late. The rams it was there to stop were 95% of all of them: a player who
// flies slowly with the nose on the fight -- which is how the M1 recordings say
// people do fly -- meets the slow-target press and the won-angle press, which
// point straight at them and have no break of their own, and the floor was the
// only thing left to call the pass off. Measured, an opponent died ramming the
// player in 8 of the author's 46 M1 wins and the player died to a ram in 3 of 12
// losses; the suicide run, which is what the rams looked like, was by the best
// estimate about one of the eleven.
//
// So this asks the question a pilot asks: if neither of us turns, how close do
// we pass, and how soon? Under ENEMY_AVOID_MISS within ENEMY_AVOID_LOOK and it
// turns toward the side it is already missing on -- the shortest way out -- and
// holds that for ENEMY_AVOID_HOLD. Something coming up from behind is outrun
// rather than turned from, because a slow ship turning aside is still a slow
// ship in the way.
//
// THE HOLD IS WHAT MAKES IT A TURN AND NOT A WOBBLE. Without one the test flips
// on and off as the geometry changes, and a first version that had none, asked
// 80 units and 1.2 s, left more collisions than no floor at all. Swept: at
// 0.25 s, 13% of turns started again within 0.3 s of letting go; at 0.6 s, 0.1%.
// The longer hold also took being run down from behind from 13 contacts to 5
// across the sweep. 75 units and
// a second ahead sit in the one corner of the sweep with no accidents at all on
// either test seat.
//
// It changes nothing about how they fight until the last second of a merge, and
// the fight is no softer for it. Against a seat that flies the way the author
// does, collisions went from 19.4 to 1.2 a fight-hour and every one left was a
// suicide run; the opponents killed the seat with missiles more often, 20.2 to
// 25.0 an hour, because they lived through the merge to do it; and a bout ran
// 31 s to 38 s at the median. The same on seeds the sweep never saw.
//
// UNDER THE WALL, and nothing else.
#define ENEMY_AVOID_MISS     75.0f   // closest approach it will accept, world units
#define ENEMY_AVOID_LOOK     1.0f    // ...this many seconds ahead
#define ENEMY_AVOID_HOLD     0.6f    // committed to the turn this long once it starts

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
// take the exchange.
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

// --- out of a circle, away, and back in to attack ---------------------------
//
// THE AUTHOR'S VERDICT ON EVERY OPPONENT, 2026-10-25: each one either circles --
// "dog tail chasing spinning around" -- or leaves and never comes back. Nothing
// punishes a player who sits at zero throttle and turns, so nothing ever makes
// them leave. What they asked for, in their words: "disengage, break away for a
// bit and turn around to fire at you", and pressure defined as "the need for me to
// personally disengage knowing that i might get hit if i keep engaging".
//
// THE RESET ABOVE IS NOT THIS. It waits for twelve seconds with nobody landing
// anything, and a hit EITHER WAY restarts that clock, so a circle with the odd
// graze never trips it; it then runs for under two and a half seconds, which opens
// a few hundred units at most; and nothing comes after it, so the fight resumes
// exactly where it was.
//
// SO THIS WATCHES FOR THE CIRCLE ITSELF, with the AI tab's census test so that
// what triggers it is what is measured: both ships pulling hard, the range not
// moving, inside this ship's lock range, and nobody holding a lock. Held for
// ENEMY_CIRCLE_TIME, the ship leaves at full throttle along the tube until it has
// opened its class's distance, then turns back in and makes ONE committed run:
// leading the player, slowing into its envelope, and shooting through the same
// trigger as always. Then the fight is the fight again, and if it goes back to a
// circle, the whole thing happens again.
//
// THE CIRCLE TEST IS THE AI TAB'S CENSUS, number for number, so that what this
// answers is what is measured. 0.4 is where the author's own smoothed pull sits on
// more than half their AEGIS frames in the M1 recordings; 0.25 of top speed keeps
// the median range rate of a mutual turn, which is under 80 units a second.
#define ENEMY_CIRCLE_PULL     0.40f   // both ships using this share of their turn
#define ENEMY_CIRCLE_RDOT_K   0.25f   // ...the range moving slower than this x top speed
#define ENEMY_CIRCLE_SMOOTH   0.50f   // seconds: the pulls and the range rate, smoothed
// SOONER, from the second playtest: against the network "it still takes too long
// for the computer to realize it should break away". At two seconds the circle
// was already being counted when the break began, and the smoothing on the
// pulls kept it counted for the first of the extend.
#define ENEMY_CIRCLE_TIME     1.0f    // seconds of an unbroken circle before it leaves
// THE EXTEND IS ALSO THE MISSILE DEFENCE. Flat out, every hull outruns the rounds
// of its own class, so a ship running away is a ship the player's shot cannot
// catch -- and the classic way to die is to turn back in front of a round that is
// still chasing. So it runs flat out the whole way, and it does not turn back while
// something is tracking it inside the alert range, up to a harder ceiling.
#define ENEMY_BRK_EXTEND_MAX  6.0f    // the extend ends here even if the range has not opened
#define ENEMY_BRK_EXTEND_HARD 9.0f    // ...unless a round is still chasing, then here
#define ENEMY_BRK_RUN_MAX     7.0f    // the run's ceiling, from the turn to the break
#define ENEMY_BRK_COOLDOWN    2.0f    // after a run, before another circle can start one
#define ENEMY_BRK_LEAD_MAX    0.30f   // seconds of the player's motion it aims ahead, at most
// A COMMITTED RUN takes the shot before it answers an incoming round, unless the
// round is this close: inside it, the break across is the only thing still worth
// doing. After its shot, it answers at the ordinary ENEMY_EVADE_RANGE.
#define ENEMY_BRK_LAST_DITCH  250.0f

// --- the exposed shot -------------------------------------------------------
//
// "NOT ENOUGH TO MAKE ME FEEL LIKE IT'S PRESSURING ME OR TAKING SHOT AT ME WHENEVER I
// AM EXPOSED" -- the author, on the tactics with the break, which they otherwise
// called the most fun opponent yet. Exposed means inside this ship's lock range
// while it is outside the player's own cone: the player cannot shoot it, so it is
// the moment to shoot them.
//
// MEASURED, NOTHING POINTED AT THEM. Against the author-like seat the player was
// exposed on most frames, and the opponent's nose was on them for 6-18% of those
// and its lock for 1-11%. The rack was not the reason and neither was the lock
// time: the tactics fly their curves, an empty AEGIS orbits to rearm, and the
// network points wherever it points. Nothing anywhere decided to turn onto a
// player who had turned away. So this does: nose on, slowed to the class's own
// lock speed, and the ordinary trigger does the rest.
//
// UNDER BOTH COLLISION FLOORS and over the break, because a shot at somebody who
// cannot answer it is worth more than the plan to set one up. A round coming in
// close is answered first, as everywhere.
#define ENEMY_EXPO_MAX        4.0f    // seconds it will spend lining one up
#define ENEMY_EXPO_COOLDOWN   1.5f    // ...and before it starts another

// --- the shot, judged ---------------------------------------------------------
//
// "8 OF 10 TACTICS SHOTS WENT INTO THE WALL." The opponent fired the moment it held
// a lock, and the lock says nothing about whether the round can follow the target.
// Measured on its rounds against the two author-like seats, what decides that
// is the target's SPEED and how fast it is CROSSING the line of sight, both
// against the round's own speed -- not the range, and not how far off the nose it
// left. An AEGIS round flies at 340 and a seat at full throttle at 420: of its
// rounds fired at the curved seat while it was faster than the round or crossing,
// 5% arrived and 74% hit the wall. The round lost the seat from its cone in the
// first second and flew on.
//
// So the pilot judges the shot, the way a person does: what chance does this round
// have against that target, flying the way it is flying now? See hit_chance. Below
// the class's minimum it holds the lock and waits for a better moment -- the seat
// slowing to turn or to lock, or the geometry closing up -- instead of spending
// the round. Not a weapon rule: the trigger's own gates are untouched, and a
// network that has its own opinion about the trigger keeps it.
//
// LOW FOR THE TWO HULLS A SEAT CAN OUTRUN. Against a seat flying its own hull an
// AEGIS or a CHARIOT round is slower than the seat, so the chance is low on almost
// every shot: at 0.45 an AEGIS fired once every eleven minutes at the curved seat,
// and a CHARIOT landed about a fifth of the hits it did with no gate. A round that
// probably misses still has to be answered, and that is pressure; a round fired at
// somebody flying away across the line of sight at full speed is only the wall. So
// these two hold only the shots with next to no chance -- AEGIS halved its rounds
// into the wall and lost no hits against the plain seat; for CHARIOT, whose seeker
// takes in almost everything, only the hopeless go. LANCE and BALLISTA were not
// moved by the gate at any height in their own matchups.
#define ENEMY_SHOT_MIN_FIGHTER   0.20f
#define ENEMY_SHOT_MIN_GEOMETRY  0.45f
#define ENEMY_SHOT_MIN_SLASH     0.10f
#define ENEMY_SHOT_MIN_STANDOFF  0.45f

// --- BALLISTA lights its round -------------------------------------------------
//
// "WE NEED TO ENSURE THAT THE SHIP KNOWS TO ILLUMINATE THE MISSILE ... ALWAYS FAVOR
// GUIDANCE UNLESS THE CHANCE OF BEING HIT IS HIGH." A semi-active round rides its
// launcher's lock, and the launcher let go of two in three of them: of 1,266 rounds
// a tactics BALLISTA fired at the author-like seats, 736 went dark before they
// arrived, because nothing in it knew a round was in the air. Not one of those
// hit; of the ones that stayed lit, 80% did. The network flying it did the same.
//
// So while one of its rounds is guided, it keeps the player in its lock -- above
// the break, the exposed shot and the network -- and lets go only for a round
// coming in at it that it judges likely to hit: its seeker still holds this ship,
// it is under ENEMY_DANGER_TTI from impact, and hit_chance, read from the incoming
// round's side, is over ENEMY_ILLUM_DANGER. Then everything below answers it as
// before.
#define ENEMY_ILLUM_DANGER   0.50f
#define ENEMY_ILLUM_SPEED    0.40f   // of its speed span, while it lights
// THE SAME CHANCE, READ FROM THE OTHER SIDE: a round coming in at this ship. Only
// a round this close in time is a danger yet; further out there is still time to
// answer it after whatever this ship is in the middle of. See incoming_danger.
#define ENEMY_DANGER_TTI         2.0f

// --- answering a round, with the advantage ------------------------------------------
//
// HOLDING THE ADVANTAGE, it answers only a round it judges likely to hit. The
// exposed shot gave way to any round inside ENEMY_EVADE_RANGE, so a player who
// threw a round at it -- any round, from any angle -- took the one shot it had
// lined up away from it. Now it gives way when incoming_danger says the round is
// likely to arrive, and otherwise takes the shot.
#define ENEMY_EVADE_DANGER    0.50f
// AN EMPTY AEGIS BREAKS TOO. Holding the break until a round came back held more
// than half of what was left of an AEGIS's circling against the network. Its run
// is flown nose-on, which is the contact in front, which is what refills an
// AR-AAM rack -- so it comes back with the round it went to get.
#define BRK_DRY_BREAK         1
// THE SNIPER'S BREAK IS ITS OWN STANDOFF. A BALLISTA flown by the network
// knife-fights, and with no break of its own it was half of all the circling
// left. An extend is wrong for it -- the slowest hull in the game, back turned --
// but its class tactic already holds a band and leaves when crowded, so its
// break is that tactic, flown for this long.
#define BRK_STANDOFF_BREAK    1
#define ENEMY_BRK_STANDOFF_T  5.0f
// PER CLASS: how far it opens the range, as a fraction of its own lock range; how
// fast it flies the last of the run, inside that range, as a fraction of its speed
// span; and how long after its first shot the run is over.
#define BRK_EXTEND_FIGHTER    1.00f
#define BRK_EXTEND_GEOMETRY   1.00f
#define BRK_EXTEND_SLASH      1.30f
// NOT ITS STANDOFF BAND. Three thousand units at the slowest speed in the game is
// nine seconds with its back to every round in the air -- measured, it took more
// hits on the way out than in the whole rest of the fight. It locks at 4200 in an
// eight-degree cone with no lock time, so it needs room to turn, not a band.
#define BRK_EXTEND_STANDOFF   0.45f
#define BRK_SPEED_FIGHTER     0.35f   // the fighter's close-in speed
#define BRK_SPEED_GEOMETRY    0.22f   // GEOM_SPEED_K: slow, to hold the lock
#define BRK_SPEED_SLASH       0.45f   // SLASH_SPEED_K: fast, just under the gate
#define BRK_SPEED_STANDOFF    0.15f   // point and hold
#define BRK_AFTER_FIGHTER     0.6f
#define BRK_AFTER_GEOMETRY    1.0f
#define BRK_AFTER_SLASH       1.5f    // the whole rack, which is the class
#define BRK_AFTER_STANDOFF    5.0f    // semi-active: it keeps lighting the round

// --- each hull's reset (playtest 3, the author's design) ------------------------
//
// "FOR AEGIS THE RETREAT IS A BIT TOO FAR AND IT WORKS AGAINST ITS PLAYSTYLE." It
// rearms only with the contact in its forward half, and an extend out to its own
// lock range turned its back for up to six seconds: its mean range in that sitting
// was 968, against about 400 before. So each hull now resets its own way:
//   AEGIS    a short break-off, then back in early, nose on, rearming as it comes
//   LANCE    a little more room than that, to lock on for its salvo -- not much
//            more, because it still has to aim to lock
//   CHARIOT  today's reset, which the author called a good baseline for the staple
//            run and gun, with its long cooldown
//   BALLISTA a shorter retreat and then closer combat: see STANDOFF3_HOLD_K
// How far the extend opens the range (a fraction of the hull's lock range), and
// its two ceilings in seconds: open or not, and chased or not.
#define BRK3_EXTEND_FIGHTER    0.40f
#define BRK3_EXTEND_GEOMETRY   0.60f
#define BRK3_EXT_MAX_FIGHTER   2.0f
#define BRK3_EXT_MAX_GEOMETRY  3.5f
#define BRK3_EXT_HARD_FIGHTER  3.0f
#define BRK3_EXT_HARD_GEOMETRY 4.5f
// How long a circle holds before the break, and how long after one before the next.
// LANCE keeps two seconds of circle: the first of them is the lock it is building,
// and one second cost it -- deaths 1.78 -> 2.88 per ten minutes against the plain
// seat, network flying.
#define BRK3_HOLD_GEOMETRY     2.0f
#define BRK3_CD_FIGHTER        1.0f
#define BRK3_CD_GEOMETRY       1.5f
#define BRK3_CD_SLASH          4.0f    // the long cooldown is the class
#define BRK3_CD_STANDOFF       2.0f
// BALLISTA'S BREAK, shorter, and the band it holds, closer. It held three thousand
// units and fired from there: of its rounds launched beyond 2500, 3.5% arrived and
// 86% ran out of life, where inside 1600 they hit 60-73%. The round is slow, but
// it turns at five radians a second with a 65-degree seeker -- a wide threshold up
// close, which is where the author wants it fought.
#define BRK3_STANDOFF_T        2.5f
#define STANDOFF3_HOLD_K       0.32f   // x its own lock range: about 1350
#define STANDOFF3_FLEE_K       1.0f    // x ENEMY_BREAK_RANGE: it leaves only inside 560

// --- the break, per hull ------------------------------------------------------
//
// LANCE HOLDS ITS LOCK, and that is what a run costs it. A committed run takes its
// shot before it answers a round, and a LANCE is the slowest of them to shoot: it
// has to hold its lock, so it sat in front of the player's rounds the longest.
// Measured against the author-like seat, it died fourteen times as often as before
// the break (0.16 to 2.28 a ten minutes) for 0.09 kills. So a LANCE on its run
// answers a round at the ordinary evade range, before its shot as well as after it.
// That took its deaths from 2.28 to 1.78 and kept its kills. Ending a run that has
// no lock after 1.5 s was also measured: it took the kills to nothing.
#define BRK_EVADE_EARLY_GEOMETRY  1
// A CHARIOT HITTING FROM THE CIRCLE IS NOT CIRCLING. The complaint was a circle with
// no punishment in it, and a twelve-round rack landing from the turn IS the
// punishment: breaking out of it halved this class's kills. So a CHARIOT does not
// break while the player has been hit in the last this-many seconds -- a little
// over its ten-second reload, so one burst a reload keeps it in the circle.
#define BRK_WINNING_SLASH         12.0f   // 0: break whatever

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

// =============================================================================
// AGGRESSION: every knob of the opponent's aggression, in one table (break v4)
// =============================================================================
//
// "WE ARE PUSHING TO MAKE AI BECOME MORE AGGRESSIVE (VERY VERY AGGRESSIVE) ... ID
// RATHER OVERTUNE THEN TUNE IT DOWN THAN UNDERTUNING LIKE WHAT WE HAVE NOW." -- the
// author, 2026-10-26, after the opponent in playtest 3 fired four rounds in three
// and a half minutes. The weapons and the flight model stay as they are; this is
// all behaviour, and it is all here, so that tuning it down is an edit to numbers.
//
// EACH KNOB HAS TWO ROWS. CALM is break v3, the opponent the author found not
// pressuring, with the values the earlier parts named; HOT is this tuning. One
// number blends them:
//   AGGRESSION 0   break v3, exactly -- the same fights, frame for frame
//   AGGRESSION 1   the HOT row
//   in between     the way back down; above 1, further than the table
// To tune a single knob, change its HOT cell.
#define AGGRESSION 1.0f

enum AggKnob {
    // THE HUNT, the layer that attacks: under the collision floors, BALLISTA's
    // light, the exposed shot and the break, over everything else.
    AGG_HUNT_RANGE,    // x lock range: inside this it hunts the player; 0 never
    AGG_HUNT_MIN,      // x lock range: ...and outside this; closer in, the class's
                       // own fight (the tactics or the network) has the ship
    AGG_NET_HUNT_MIN,  // the same, while the network flies it: inside this the
                       // network fights, and the hunt only brings it back in
    AGG_HUNT_DRY,      // s of reload left at which an empty rack hunts again
                       // (an AR-AAM rearms facing the player, so it always does)
    AGG_CHASE_SPEED,   // of its speed span, closing on them from outside...
    AGG_LOCK_RANGE,    // ...this, x lock range, inside which it flies at...
    AGG_LOCK_SPEED,    // ...this share of its speed span, to lock
    AGG_OFFSET,        // units: it aims this far beside the player, inside the lock
                       // cone, so a pass goes by them instead of into the floors
    // THE TRIGGER AND THE NERVE
    AGG_SHOT_MIN,      // hit chance it fires at (hit_chance); 0 fires on the lock
    AGG_EVADE_MIN,     // hit chance of a round coming in that it breaks off for
                       // while hunting or taking the exposed shot
    AGG_EXPO_CD,       // s after an exposed shot before it looks for the next
    // THE RESET, each hull's own: the break out of a circle
    AGG_HOLD,          // s of circle before it breaks
    AGG_EXTEND,        // how far it opens the range, x lock range
    AGG_EXT_MAX,       // s: the extend ends here, open or not...
    AGG_EXT_HARD,      // s: ...and here, chased or not
    AGG_BREAK_CD,      // s after a break before another can start
    // BALLISTA (the other columns are not read)
    AGG_STANDOFF_T,    // s: its break is its standoff for this long
    AGG_BAND,          // x lock range: the band its standoff holds
    AGG_FLEE,          // x ENEMY_BREAK_RANGE: it leaves only inside this
    AGG_ILLUM_DANGER,  // hit chance of a round coming in that makes it stop lighting
    // THE CHARGE AND THE BREAK-AWAY (playtest 4), inside the hunt: in nose on from
    // a distance, and when the player is closer than...
    AGG_IN_MIN,        // ...this, x lock range, away again (0: never) -- until...
    AGG_OUT_R,         // ...the range is back to this, x lock range...
    AGG_OUT_MAX,       // ...or this many seconds have gone,
    AGG_OUT_ANG,       // degrees off straight away: 0 opens the range, 90 crosses
                       // their path with them on the beam
    AGG_OUT_CHASED,    // 1: it keeps breaking away with the player's nose on it;
                       // 0: it turns back in -- it does not fear being caught
    AGG_SALVO,         // a banking rack: rounds banked before it fires (0: one)
    // THE TURN BACK AND THE BREAK-AWAY'S SPEED (playtest 5), inside the hunt. With
    // the player further off the nose than...
    AGG_TURN_COS,      // ...this cosine (below -1: never), the hunt flies at...
    AGG_TURN_SPEED,    // ...this, of its speed range: a turn is only tighter if the
                       // speed goes
    AGG_OUT_SPEED,     // the break-away's speed, of its speed range (1: flat out)
    // THE LIBRARY AND THE DIRECTOR (step C): see director_choose in vg_ai.cpp.
    AGG_DIR_ON,            // 1: the director chooses for this hull; 0: the rules
    AGG_DIR_PRESS,         // s without threatening the player, after which it may not slash
    AGG_DIR_STALE,         // s of ORBIT before the stalemate breaker slashes...
    AGG_DIR_WIN,           // ...unless it hit the player within this many s (the 12 s rule)
    AGG_DIR_OTAN,          // ORBIT: share of its motion across the line to the player...
    AGG_DIR_OW,            // ...and its sweep round them, rad/s (census_orbit.diff's)
    AGG_DIR_EXPO,          // 1: the old exposed shot still flies when nothing above it does
    AGG_STK_R,             // x lock range: it stalks inside this, and hunts outside
    AGG_STK_RDES,          // x lock range: the range a stalk holds...
    AGG_STK_RCLOSE,        // x lock range: ...and inside this it slashes after a shot (AEGIS: a strafe across)
    AGG_STK_K,             // 1/s: throttle per unit off STK_RDES, on the player's own speed
    AGG_STK_VMAX,          // of its speed range: a stalk is never faster
    AGG_STK_OFS,           // units beside the player it aims (the hunt's)...
    AGG_STK_CLAMP,         // ...kept inside this share of the acquiring cone
    AGG_STK_TURN,          // cos: the player further off the nose than this, it flies at its slowest
    AGG_FACE_BEHIND,       // cos: it faces a chaser this far off the nose (0: behind the beam)...
    AGG_FACE_RMIN,         // ...from this range out, x lock range...
    AGG_FACE_COS,          // ...until the player is inside this cos of its nose (a pivot too)
    AGG_PIN_SLOW,          // of the player's speed range: slower than this, close behind it, it slashes out
    AGG_SL_R,              // x lock range: a slash runs out to this range, then pivots
    AGG_SL_AFTER,          // 1: the slash at close range only after the stalk has fired
    AGG_SL_OFS,            // how hard the slash bends away from the player's nose (0: straight)
    AGG_PV_CHOP,           // of its speed range: a pivot holds its heading until it is this slow
    AGG_S1_SALVO,          // a banking rack in the library banks this many (0: AGG_SALVO)...
    AGG_S1_EDGE,           // ...and fires early past this share of the holding cone off the aim
    // THE SLASH ALONG THE TUBE (batch 1, the rams): see step_slash.
    AGG_SL_TUBE,           // 1: once past, the slash runs along the tube (extend_dir), not straight away
    // NOT AWAY (batch 1, the running-away gate): see step_slash and fly_evade_dir.
    AGG_SL_ANG,            // degrees off straight away for a slash (0: along the tube; 90: across their path)
    AGG_SL_BLOW,           // 1: it blows through on its heading first while the player is ahead; 0: across at once
    AGG_SL_T,              // s: a slash is over after this, whatever the range (0: by range)
    AGG_SL_PIVOT,          // 1: a pivot follows a slash; 0: straight back to the chooser
    AGG_DIR_EVTOW,         // 1: the director's evade crosses the round's path on the side toward the player
    AGG_PV_AWAY,           // 1: a pivot slows on its heading even pointing away; 0: pointing away, it turns at once
    AGG_KNOBS
};

//  knob                AEGIS                   LANCE                   CHARIOT                 BALLISTA
static constexpr float AGG_CALM[AGG_KNOBS][4] = {
    /* HUNT_RANGE    */ { 0.0f,                 0.0f,                   0.0f,                   0.0f                 },
    /* HUNT_MIN      */ { 0.0f,                 0.0f,                   0.0f,                   0.0f                 },
    /* NET_HUNT_MIN  */ { 0.0f,                 0.0f,                   0.0f,                   0.0f                 },
    /* HUNT_DRY      */ { 0.0f,                 0.0f,                   0.0f,                   0.0f                 },
    /* CHASE_SPEED   */ { 1.0f,                 1.0f,                   1.0f,                   1.0f                 },
    /* LOCK_RANGE    */ { 0.6f,                 0.6f,                   0.6f,                   0.6f                 },
    /* LOCK_SPEED    */ { BRK_SPEED_FIGHTER,    BRK_SPEED_GEOMETRY,     BRK_SPEED_SLASH,        BRK_SPEED_STANDOFF   },
    /* OFFSET        */ { 0.0f,                 0.0f,                   0.0f,                   0.0f                 },
    /* SHOT_MIN      */ { ENEMY_SHOT_MIN_FIGHTER, ENEMY_SHOT_MIN_GEOMETRY, ENEMY_SHOT_MIN_SLASH, ENEMY_SHOT_MIN_STANDOFF },
    /* EVADE_MIN     */ { ENEMY_EVADE_DANGER,   ENEMY_EVADE_DANGER,     ENEMY_EVADE_DANGER,     ENEMY_EVADE_DANGER   },
    /* EXPO_CD       */ { ENEMY_EXPO_COOLDOWN,  ENEMY_EXPO_COOLDOWN,    ENEMY_EXPO_COOLDOWN,    ENEMY_EXPO_COOLDOWN  },
    /* HOLD          */ { ENEMY_CIRCLE_TIME,    BRK3_HOLD_GEOMETRY,     ENEMY_CIRCLE_TIME,      ENEMY_CIRCLE_TIME    },
    /* EXTEND        */ { BRK3_EXTEND_FIGHTER,  BRK3_EXTEND_GEOMETRY,   BRK_EXTEND_SLASH,       BRK_EXTEND_STANDOFF  },
    /* EXT_MAX       */ { BRK3_EXT_MAX_FIGHTER, BRK3_EXT_MAX_GEOMETRY,  ENEMY_BRK_EXTEND_MAX,   ENEMY_BRK_EXTEND_MAX },
    /* EXT_HARD      */ { BRK3_EXT_HARD_FIGHTER, BRK3_EXT_HARD_GEOMETRY, ENEMY_BRK_EXTEND_HARD, ENEMY_BRK_EXTEND_HARD },
    /* BREAK_CD      */ { BRK3_CD_FIGHTER,      BRK3_CD_GEOMETRY,       BRK3_CD_SLASH,          BRK3_CD_STANDOFF     },
    /* STANDOFF_T    */ { BRK3_STANDOFF_T,      BRK3_STANDOFF_T,        BRK3_STANDOFF_T,        BRK3_STANDOFF_T      },
    /* BAND          */ { STANDOFF3_HOLD_K,     STANDOFF3_HOLD_K,       STANDOFF3_HOLD_K,       STANDOFF3_HOLD_K     },
    /* FLEE          */ { STANDOFF3_FLEE_K,     STANDOFF3_FLEE_K,       STANDOFF3_FLEE_K,       STANDOFF3_FLEE_K     },
    /* ILLUM_DANGER  */ { ENEMY_ILLUM_DANGER,   ENEMY_ILLUM_DANGER,     ENEMY_ILLUM_DANGER,     ENEMY_ILLUM_DANGER   },
    /* IN_MIN        */ { 0.0f,                 0.0f,                   0.0f,                   0.0f                 },
    /* OUT_R         */ { 0.0f,                 0.0f,                   0.0f,                   0.0f                 },
    /* OUT_MAX       */ { 0.0f,                 0.0f,                   0.0f,                   0.0f                 },
    /* OUT_ANG       */ { 0.0f,                 0.0f,                   0.0f,                   0.0f                 },
    /* OUT_CHASED    */ { 1.0f,                 1.0f,                   1.0f,                   1.0f                 },
    /* SALVO         */ { 0.0f,                 0.0f,                   0.0f,                   0.0f                 },
    /* TURN_COS      */ { -2.0f,                -2.0f,                  -2.0f,                  -2.0f                },
    /* TURN_SPEED    */ { 0.0f,                 0.0f,                   0.0f,                   0.0f                 },
    /* OUT_SPEED     */ { 1.0f,                 1.0f,                   1.0f,                   1.0f                 },
    /* DIR_ON        */ { 0.0f, 0.0f, 0.0f, 0.0f },
    /* DIR_PRESS     */ { 0.0f, 0.0f, 0.0f, 0.0f },
    /* DIR_STALE     */ { 0.0f, 0.0f, 0.0f, 0.0f },
    /* DIR_WIN       */ { 0.0f, 0.0f, 0.0f, 0.0f },
    /* DIR_OTAN      */ { 0.0f, 0.0f, 0.0f, 0.0f },
    /* DIR_OW        */ { 0.0f, 0.0f, 0.0f, 0.0f },
    /* DIR_EXPO      */ { 0.0f, 0.0f, 0.0f, 0.0f },
    /* STK_R         */ { 0.0f, 0.0f, 0.0f, 0.0f },
    /* STK_RDES      */ { 0.0f, 0.0f, 0.0f, 0.0f },
    /* STK_RCLOSE    */ { 0.0f, 0.0f, 0.0f, 0.0f },
    /* STK_K         */ { 0.0f, 0.0f, 0.0f, 0.0f },
    /* STK_VMAX      */ { 0.0f, 0.0f, 0.0f, 0.0f },
    /* STK_OFS       */ { 0.0f, 0.0f, 0.0f, 0.0f },
    /* STK_CLAMP     */ { 0.0f, 0.0f, 0.0f, 0.0f },
    /* STK_TURN      */ { 0.0f, 0.0f, 0.0f, 0.0f },
    /* FACE_BEHIND   */ { 0.0f, 0.0f, 0.0f, 0.0f },
    /* FACE_RMIN     */ { 0.0f, 0.0f, 0.0f, 0.0f },
    /* FACE_COS      */ { 0.0f, 0.0f, 0.0f, 0.0f },
    /* PIN_SLOW      */ { 0.0f, 0.0f, 0.0f, 0.0f },
    /* SL_R          */ { 0.0f, 0.0f, 0.0f, 0.0f },
    /* SL_AFTER      */ { 0.0f, 0.0f, 0.0f, 0.0f },
    /* SL_OFS        */ { 0.0f, 0.0f, 0.0f, 0.0f },
    /* PV_CHOP       */ { 0.0f, 0.0f, 0.0f, 0.0f },
    /* S1_SALVO      */ { 0.0f, 0.0f, 0.0f, 0.0f },
    /* S1_EDGE       */ { 0.0f, 0.0f, 0.0f, 0.0f },
    /* SL_TUBE       */ { 0.0f, 0.0f, 0.0f, 0.0f },
    /* SL_ANG        */ { 0.0f, 0.0f, 0.0f, 0.0f },
    /* SL_BLOW       */ { 0.0f, 0.0f, 0.0f, 0.0f },
    /* SL_T          */ { 0.0f, 0.0f, 0.0f, 0.0f },
    /* SL_PIVOT      */ { 0.0f, 0.0f, 0.0f, 0.0f },
    /* DIR_EVTOW     */ { 0.0f, 0.0f, 0.0f, 0.0f },
    /* PV_AWAY       */ { 0.0f, 0.0f, 0.0f, 0.0f },
};

// THE HOT ROW. Each hull keeps its shape -- AEGIS resets shortest, LANCE a little
// longer, CHARIOT long -- at a shorter length, and hunts. BALLISTA'S COLUMN IS ITS
// CALM ONE: it already resets short and fights close, and every hotter setting
// measured cost it hits on the curved author-like seat in its own matchup --
// hunting took it from 4.4 hits a minute to 1.5, the nerve knobs alone to 3.2.
//  knob                AEGIS   LANCE   CHARIOT BALLISTA
static constexpr float AGG_HOT[AGG_KNOBS][4] = {
    /* HUNT_RANGE    */ { 3.0f,   3.0f,   3.0f,   0.0f },   // the whole arena, near enough
    /* HUNT_MIN      */ { 0.0f,   0.0f,   0.0f,   0.0f },
    /* NET_HUNT_MIN  */ { 0.0f,   0.0f,   0.0f,   0.0f },   // the hunt flies the close fight over the
                                                            // network. CHARIOT: the author loved it.
                                                            // AEGIS and LANCE: their charge and
                                                            // break-away is here, and the gen-48
                                                            // network flying LANCE's close fight hit
                                                            // the curved seat 0.03 times a minute
    /* HUNT_DRY      */ { 0.0f,   2.0f,   0.0f,   0.0f },   // CHARIOT's dry extend is the class
    /* CHASE_SPEED   */ { 1.0f,   1.0f,   1.0f,   1.0f },   // flat out: a turning player bleeds speed
    /* LOCK_RANGE    */ { 0.8f,   0.8f,   0.8f,   0.6f },
    /* LOCK_SPEED    */ { 0.0f,   0.0f,   0.0f,   BRK_SPEED_STANDOFF },   // slowest: longest nose-on,
                                                                          // quickest lock
    /* OFFSET        */ { 200.0f, 200.0f, 200.0f, 0.0f },
    /* SHOT_MIN      */ { 0.2f, 0.0f, 0.0f, 0.0f },   // fire on the lock -- AEGIS 0 from playtest 5,
                                                            // 0.2 again from step C: stage 0 measured
                                                            // it with the slow approach (the curved
                                                            // seat: 0 hit nothing, 0.2 hit 0.42 a minute)
    /* EVADE_MIN     */ { 0.80f,  0.80f,  0.80f,  ENEMY_EVADE_DANGER },   // only a near-certain hit
    /* EXPO_CD       */ { 0.3f,   0.3f,   0.3f,   ENEMY_EXPO_COOLDOWN },
    /* HOLD          */ { 1.0f,   2.0f,   1.0f,   ENEMY_CIRCLE_TIME },    // LANCE keeps its two seconds
    /* EXTEND        */ { 0.30f,  0.45f,  1.00f,  BRK_EXTEND_STANDOFF },
    /* EXT_MAX       */ { 1.5f,   2.5f,   4.5f,   ENEMY_BRK_EXTEND_MAX },
    /* EXT_HARD      */ { 2.5f,   3.5f,   7.0f,   ENEMY_BRK_EXTEND_HARD },
    /* BREAK_CD      */ { 0.5f,   1.0f,   3.0f,   BRK3_CD_STANDOFF },
    /* STANDOFF_T    */ { BRK3_STANDOFF_T,  BRK3_STANDOFF_T,  BRK3_STANDOFF_T,  BRK3_STANDOFF_T },
    /* BAND          */ { STANDOFF3_HOLD_K, STANDOFF3_HOLD_K, STANDOFF3_HOLD_K, STANDOFF3_HOLD_K },
    /* FLEE          */ { STANDOFF3_FLEE_K, STANDOFF3_FLEE_K, STANDOFF3_FLEE_K, STANDOFF3_FLEE_K },
    /* ILLUM_DANGER  */ { ENEMY_ILLUM_DANGER, ENEMY_ILLUM_DANGER, ENEMY_ILLUM_DANGER, ENEMY_ILLUM_DANGER },
    // AEGIS AND LANCE, from the author's own diagnosis of playtest 4: "their class
    // mechanics work against the retreat and circling behavior, they often need to
    // break away repeatedly to look for opening and reorient, especially lance needs
    // some slight distance to charge up the salvo while aegis should not fear being
    // caught but rather try to always face the opponent to deliver attack when it's
    // charged up." Measured, the hunt took both into the merge -- a LANCE at 270
    // units, nose 120 degrees off a parked player, locked 2% of the time -- so the
    // hunt now stops closing at AGG_IN_MIN and breaks straight out: AEGIS briefly,
    // LANCE twice as long, for the distance a salvo needs. Against the curved
    // author-like seat flying any hull, that took AEGIS from 0.38 hits a minute to
    // 0.96 and LANCE from 0.45 to 1.05 (tactics). Breaking away across the player's
    // path, or at any angle, kept them alive and lost the hits. NOT YET FOR LANCE
    // AGAINST LANCE: an author-like LANCE chases one that breaks away, and in that
    // mirror it measured worse (0.28 to 0.09). CHARIOT and BALLISTA: off.
    /* IN_MIN        */ { 0.10f,  0.15f,  0.0f,   0.0f },
    /* OUT_R         */ { 0.45f,  0.50f,  0.0f,   0.0f },
    /* OUT_MAX       */ { 0.6f, 2.5f, 0.0f, 0.0f },
    /* OUT_ANG       */ { 90.0f, 0.0f, 0.0f, 0.0f },
    /* OUT_CHASED    */ { 1.0f,   1.0f,   1.0f,   1.0f },
    /* SALVO         */ { 0.0f,   2.0f,   0.0f,   0.0f },   // LANCE: two banked, then the salvo
    // AEGIS, FROM PLAYTEST 5: "aegis needs to know it's about facing the enemy,
    // even if it requires doing very short strafing runs", and in m3 it "doesn't turn
    // around as quickly and launch its attack". Its break-away was v5's -- straight
    // out, flat out, for up to a second and a half -- which is the one leg on which
    // an AR-AAM cannot rearm, and the turn back after it was flown at speed: two and
    // a half seconds for half a circle. Now the break-away is a strafe across the
    // player's path (OUT_ANG 90) for 0.6 s, the turn back is flown at its slowest
    // (TURN_COS, TURN_SPEED), and every round goes as it is ready (SHOT_MIN 0, which
    // was 0.2). Against the plain author-like seat in its own mirror that took its
    // deaths from 12 to 1.5 every ten minutes at the same 2.4 hits a minute. Against
    // the curved one it fires 40% more and hits almost nothing: those are the rounds
    // SHOT_MIN 0.2 held, at under one chance in ten. That is the trade the author
    // asked to see, and 0.2 puts it back.
    /* TURN_COS      */ { 0.0f, -2.0f, -2.0f, -2.0f },
    /* TURN_SPEED    */ { 0.0f,  0.0f,  0.0f,  0.0f },
    /* OUT_SPEED     */ { 1.0f,  1.0f,  1.0f,  1.0f },
    // THE LIBRARY AND THE DIRECTOR, step C, for AEGIS and LANCE. CHARIOT and
    // BALLISTA keep the rules, which the author called right.
    /* DIR_ON        */ { 1.0f, 1.0f, 0.0f, 0.0f },
    /* DIR_PRESS     */ { 6.0f, 6.0f, 0.0f, 0.0f },
    /* DIR_STALE     */ { 3.0f, 3.0f, 0.0f, 0.0f },
    /* DIR_WIN       */ { 12.0f, 12.0f, 0.0f, 0.0f },
    /* DIR_OTAN      */ { 0.85f, 0.85f, 0.0f, 0.0f },
    /* DIR_OW        */ { 0.25f, 0.25f, 0.0f, 0.0f },
    /* DIR_EXPO      */ { 1.0f, 0.0f, 0.0f, 0.0f },
    /* STK_R         */ { 1.0f, 1.0f, 0.0f, 0.0f },
    /* STK_RDES      */ { 0.55f, 0.35f, 0.0f, 0.0f },
    /* STK_RCLOSE    */ { 0.15f, 0.22f, 0.0f, 0.0f },
    /* STK_K         */ { 0.5f, 0.5f, 0.0f, 0.0f },
    /* STK_VMAX      */ { 0.3f, 0.3f, 0.0f, 0.0f },
    /* STK_OFS       */ { 200.0f, 200.0f, 0.0f, 0.0f },
    /* STK_CLAMP     */ { 0.5f, 0.5f, 0.0f, 0.0f },
    /* STK_TURN      */ { 0.0f, 0.0f, 0.0f, 0.0f },
    /* FACE_BEHIND   */ { 0.0f, 0.0f, 0.0f, 0.0f },
    /* FACE_RMIN     */ { 0.15f, 0.15f, 0.0f, 0.0f },
    /* FACE_COS      */ { 0.866f, 0.866f, 0.0f, 0.0f },
    /* PIN_SLOW      */ { 0.08f, 0.08f, 0.0f, 0.0f },
    /* SL_R          */ { 0.5f, 0.5f, 0.0f, 0.0f },
    /* SL_AFTER      */ { 1.0f, 1.0f, 0.0f, 0.0f },
    /* SL_OFS        */ { 0.0f, 0.0f, 0.0f, 0.0f },
    /* PV_CHOP       */ { 0.3f, 0.3f, 0.0f, 0.0f },
    /* S1_SALVO      */ { 0.0f, 4.0f, 0.0f, 0.0f },
    /* S1_EDGE       */ { 0.0f, 0.8f, 0.0f, 0.0f },
    // THE SLASH ALONG THE TUBE (batch 1, the rams): see step_slash.
    /* SL_TUBE       */ { 1.0f, 1.0f, 0.0f, 0.0f },
    // NOT AWAY (batch 1, the running-away gate): see step_slash and fly_evade_dir.
    /* SL_ANG        */ { 90.0f, 0.0f, 0.0f, 0.0f },
    /* SL_BLOW       */ { 0.0f, 1.0f, 0.0f, 0.0f },
    /* SL_T          */ { 0.6f, 0.0f, 0.0f, 0.0f },
    /* SL_PIVOT      */ { 0.0f, 1.0f, 0.0f, 0.0f },
    /* DIR_EVTOW     */ { 1.0f, 0.0f, 0.0f, 0.0f },
    /* PV_AWAY       */ { 1.0f, 0.0f, 0.0f, 0.0f },
};

// =============================================================================
// THE MANOEUVRES' MIN AND MAX, per hull (stage 1)
// =============================================================================
//
// Seconds. MIN: a chooser may not leave a manoeuvre sooner, except for EVADE or TAIL.
// MAX: it must leave by then; 0 is no ceiling. One row per manoeuvre in the order of
// VG_MNV_LIST (vg_mnv.h), which vg_ai.cpp checks.
//
// THE RULES DO NOT READ THESE. They keep each layer's own timers, which is what keeps
// the fights the fights the chain flew, so the rows of the old layers only say what
// those timers are, for a selector to read -- at AGGRESSION 1 where a timer is a knob.
// Where a layer has no commitment at all, MIN is the 0.5 s PLAN.md asks for.
//  manoeuvre       MIN: AEGIS LANCE CHARIOT BALLISTA          MAX
static constexpr float MNV_TIME[][2][4] = {
    /* none     */ { { 0.0f, 0.0f, 0.0f, 0.0f },
                     { 0.0f, 0.0f, 0.0f, 0.0f } },
    /* hunt     */ { { 0.5f, 0.5f, 0.5f, 0.5f },
                     { 0.0f, 0.0f, 0.0f, 0.0f } },
    /* breakout */ { { 0.0f, 0.0f, 0.0f, 0.0f },
                     { AGG_HOT[AGG_OUT_MAX][0], AGG_HOT[AGG_OUT_MAX][1],
                       AGG_HOT[AGG_OUT_MAX][2], AGG_HOT[AGG_OUT_MAX][3] } },
    /* cbreak   */ { { 0.0f, 0.0f, 0.0f, 0.0f },   // the extend's hard ceiling, then the run's
                     { AGG_HOT[AGG_EXT_HARD][0] + ENEMY_BRK_RUN_MAX,
                       AGG_HOT[AGG_EXT_HARD][1] + ENEMY_BRK_RUN_MAX,
                       AGG_HOT[AGG_EXT_HARD][2] + ENEMY_BRK_RUN_MAX,
                       AGG_HOT[AGG_STANDOFF_T][3] } },          // BALLISTA: its standoff
    /* reset    */ { { ENEMY_RESET_MIN, ENEMY_RESET_MIN, ENEMY_RESET_MIN, ENEMY_RESET_MIN },
                     { ENEMY_RESET_MAX, ENEMY_RESET_MAX, ENEMY_RESET_MAX, ENEMY_RESET_MAX } },
    /* evade    */ { { 0.9f, 0.9f, 0.9f, 0.9f },   // one leg; legs follow while a round is in
                     { 0.0f, 0.0f, 0.0f, 0.0f } },
    /* tail     */ { { ENEMY_DEFEND_MIN, ENEMY_DEFEND_MIN, ENEMY_DEFEND_MIN, ENEMY_DEFEND_MIN },
                     { 0.0f, 0.0f, 0.0f, 0.0f } },
    /* exposed  */ { { 0.0f, 0.0f, 0.0f, 0.0f },
                     { ENEMY_EXPO_MAX, ENEMY_EXPO_MAX, ENEMY_EXPO_MAX, ENEMY_EXPO_MAX } },
    /* light    */ { { 0.0f, 0.0f, 0.0f, 0.0f },   // the round's life
                     { 0.0f, 0.0f, 0.0f, 0.0f } },
    /* standoff */ { { 0.5f, 0.5f, 0.5f, 0.5f },
                     { 0.0f, 0.0f, 0.0f, 0.0f } },
    /* dry      */ { { 0.8f, 0.8f, 0.8f, 0.8f },   // one leg; legs follow until the rack is back
                     { 0.0f, 0.0f, 0.0f, 0.0f } },
    /* press    */ { { 0.5f, 0.5f, 0.5f, 0.5f },   // MAX: the v1 press, times the pilot's press
                     { ENEMY_PRESS_MAX, ENEMY_PRESS_MAX, ENEMY_PRESS_MAX, ENEMY_PRESS_MAX } },
    /* plan     */ { { 0.5f, 0.5f, 0.5f, 0.5f },
                     { 0.0f, 0.0f, 0.0f, 0.0f } },
    /* net      */ { { 0.0f, 0.0f, 0.0f, 0.0f },
                     { 0.0f, 0.0f, 0.0f, 0.0f } },
    // THE LIBRARY (step C), flown by these rows. The starting values of
    // ai_architecture.md 2.4: a hold of 1 to 5 s, a pivot until it faces or 2.5 s.
    /* stalk    */ { { 1.0f, 1.0f, 1.0f, 1.0f },
                     { 8.0f, 8.0f, 8.0f, 8.0f } },
    /* face     */ { { 0.5f, 0.5f, 0.5f, 0.5f },
                     { 2.5f, 2.5f, 2.5f, 2.5f } },
    /* slash    */ { { AGG_HOT[AGG_SL_T][0], 1.0f, 1.0f, 1.0f },   // AEGIS: its strafe's length
                     { 3.0f, 3.0f, 3.0f, 3.0f } },
    /* pivot    */ { { 0.5f, 0.5f, 0.5f, 0.5f },
                     { 2.5f, 2.5f, 2.5f, 2.5f } },
};
