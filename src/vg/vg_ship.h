#pragma once
#include <stdint.h>

// ===========================================================================
// SHIP CLASSES
//
// Everything that distinguishes one ship from another lives in this struct, and
// BOTH the player and the AI read from it. There is one flight model and one
// damage model in the game; a ship class is just a set of numbers fed into them.
//
// The four are named for the single quality that defines each:
//
//   AEGIS    the shield  -- the reference ship, forgiving, no glaring weakness
//   LANCE    the point   -- huge damage on a clean hit, almost none on a graze
//   CHARIOT  the speed   -- fast, agile, fragile, saturates the sky with chaff
//   BALLISTA the range   -- outranges everything, helpless once you are inside
//
// The mechanic that makes LANCE and CHARIOT genuinely different PLAYSTYLES
// rather than just different damage numbers is `msl_graze_floor`: damage scales
// with how close the proximity fuse actually went off, and the floor of that
// curve is per class. A high-yield narrow warhead demands correct geometry; a
// low-yield wide one rewards volume. It belongs to the attacker's warhead, not
// to the target's armour, because it is a distinction about SHOOTING.
// ===========================================================================

// WHICH FIGHT THE AI FLIES IN THIS HULL.
//
// Everything else about a class is a number, and the AI reads those already: it
// settles to a fighting speed at a fraction of its own lock_range, so a BALLISTA
// engages from further out than a CHARIOT without the behaviour code knowing
// that classes exist. That is scale, not tactics -- a BALLISTA still flies the
// same fight an AEGIS does, just bigger.
//
// This is the part that cannot be derived from a number, because two ships with
// identical stats could still want to fight differently. It lives on the spec so
// the AI keeps reading one table, and so a class's tactic sits beside the
// numbers it has to work with.
enum ShipTactic : uint8_t {
    TACTIC_FIGHTER = 0,   // merge, pass, extend, come round again
    TACTIC_STANDOFF,      // hold the range its reach buys, and run from a merge
    TACTIC_SLASH,         // one fast pass, the rack emptied, a long extend
    TACTIC_GEOMETRY,      // line the shot up, because only a clean one pays
};

// WHICH WEAPON SYSTEM THIS HULL CARRIES.
//
// THE INSTRUMENTS KEY OFF THIS AND NEVER OFF A NUMBER, and that rule is the whole
// reason the field exists.
//
// The guide circle is BALLISTA's, and it was drawn for any class whose
// lock_hold_cos was not the -2.0f viewport sentinel. That is not a design
// statement, it is a numeric coincidence -- and the coincidence stopped holding
// the moment two other classes were given honest hold cones. AEGIS and CHARIOT
// both inherited a semi-active guidance instrument they have no semi-active
// round to guide. Reported from the cockpit as CHARIOT's targeting having
// drifted, which is exactly what it was.
//
// A number cannot say WHICH FANTASY it belongs to. lock_hold_cos answers "how far
// off can they drift", and no value of it means "this class guides its rounds".
// So the fantasy is named here, once, and anything that draws or behaves
// differently per class asks this rather than inferring it from the tuning.
//
// It is not a duplicate of the numbers beneath it -- the invariants at the foot
// of vg_ship.cpp bind the two together, so a system and a round that disagree
// fail the build rather than the playtest.
//
// One per hull, and no hull shares one:
//
//   AEGIS    AR-AAM   active radar    -- launch and leave
//   CHARIOT  RF-AAM   rapid fire      -- the rate is the weapon
//   LANCE    SL-AAM   salvo lock      -- bank the locks, empty the bay
//   BALLISTA SA-AAM   semi-active     -- fly it all the way in
enum WeaponSystem : uint8_t {
    // AR-AAM, active radar. The round carries its own seeker, so the pilot's work
    // ends at the launch: point the nose, earn the lock, and let it go. No
    // guidance to fly, nothing to bank, no circle -- the reticle and the lock cone
    // are the whole instrument.
    //
    // AND THE BAY REARMS WHILE YOU ARE FACING THEM, one round at a time, which is
    // the inversion the class is built on. Every other weapon in the game reloads
    // by DISENGAGING: empty the rack, break off, wait. This one reloads by staying
    // in the fight, and rearms not at all if you turn away.
    //
    // TWO ROUNDS, because the point is the rhythm and not the volume. It carried
    // six on a short trigger, which reads as "a lot of missiles" and not as
    // anything in particular -- it was mistaken for LANCE in the cockpit, and the
    // two had nothing to tell them apart.
    //
    // Three was the first attempt and it was flown and rejected: "i dont get that
    // sense of starvation if i fire my missiles rapidly, it just always seems to
    // be two missiles ready". Two at 1.2 seconds was the second and was rejected
    // for the same reason -- the count was never the problem. A bay that hands a
    // round back faster than a pilot can use one has no dry time in it whatever
    // its size, and 1.2s is inside the time it takes to reacquire and fire.
    //
    // TWO AT FOUR SECONDS, and it took three cockpit passes to get there: 1.0,
    // then 1.2, then 2.5, each one flown and each one still reported as spammable.
    // The lesson in that sequence is that a rearm interval does nothing until it
    // is LONGER THAN THE PILOT'S OWN LOOP -- reacquire, close, aim, fire. Under
    // that, the bay refills inside the gap between shots and the rack may as well
    // be infinite whatever its size.
    //
    // The number that finally showed it is not the firing rate but the FRACTION OF
    // THE BAY'S CEILING the pilot consumes. At 1.0s they used 51% of what the bay
    // could make and never waited on it; at 2.5s, 76% and starting to. That ratio
    // is the honest measure of starvation and the raw rate is not.
    //
    // AND THE WARHEAD WENT UP WITH IT, 20 to 63. Starvation and output are the
    // SAME DIAL on a rate-limited weapon, and there is no way round it: halving
    // the rate halves the damage unless each round is worth twice as much. Every
    // setting that made the rack genuinely run dry, on its own, put the class
    // between 393 and 910 a run -- well under everything else.
    //
    // So the rounds got heavier as they got scarcer, which is the trade that keeps
    // both and the right one for a bay holding two.
    //
    // THE TABLE UNDER-READS THIS CLASS BY ROUGHLY 2.3x AND THAT IS THE MECHANIC
    // WORKING. The bay only fills while the target is in front, so output is now
    // a function of how well the nose is kept on somebody -- and the scripted seat
    // is bad at exactly that. Measured on one matchup: the bot takes 45% of the
    // bay's ceiling, a player 76%, and the player's hits are more central besides.
    // So a warhead tuned until the HARNESS reads in band overpays a human twice
    // over, which is how 63 came to double a real pilot's damage.
    //
    // The roster table therefore says AEGIS is the weakest of the four and it is
    // not. Do not "fix" that by raising the warhead: the cockpit is the instrument
    // here, and the harness number for this one class is known to be low.
    //
    // WATCH THE OTHER COLLISION. At this rate and this warhead the class has
    // BALLISTA's numeric shape, and only the mechanic keeps them apart -- fire and
    // forget at 1600 against a round flown by hand to 4200. Fixing a confusion
    // with LANCE must not quietly make one with BALLISTA.
    //
    // It is not LANCE's 32 by accident of arithmetic and it does not read as it.
    // LANCE's graze floor is 0.20, all or nothing; this one is 0.60 and forgives.
    // The two are told apart by what a near miss is worth and by the mechanic, not
    // by the size of the number.
    //
    // The deliberate opposite of WPN_SAAAM below, and the pair is meant to teach
    // itself: one you fly all the way in, one you forget. AEGIS's, and the
    // baseline every other system is a departure from, which is why it is the
    // ship you are handed first.
    WPN_ARAAM = 0,

    // RF-AAM, rapid fire. THE RATE IS THE WEAPON: twelve rounds 0.16 seconds
    // apart, launched faster than they can be aimed one at a time, and everything
    // else about the class follows from that rather than standing beside it. The
    // small warhead, the wide splash, the short reach and the ten-second reload
    // are all the price of the interval.
    //
    // CHARIOT's, and the class this separation was opened to fix. It has no
    // instrument of its own yet -- it drew BALLISTA's guide circle until the
    // system became a declaration, and what belongs there instead is the open
    // question. Whatever it turns out to be, it is about the RACK: a class whose
    // decision is when to commit the magazine needs to be told what it has left
    // and what the reload will cost, and the shared rack tick answers neither
    // while the rounds are leaving six times a second.
    WPN_RFAAM,

    // SL-AAM, salvo lock. Hold a loose contact and it BANKS locks, one per
    // msl_stack_time; the trigger spends everything banked and the launch costs
    // the lock. The reticle subdivides into the bay. LANCE's.
    WPN_SLAAM,

    // SA-AAM, semi-active. The round is flown by the SHOOTER for its whole life:
    // keep the target inside the guide circle or every round in the air goes dumb
    // at once. The circle narrows with range, so a long shot is the hard one.
    // BALLISTA's.
    //
    // THREE As, and they are all load-bearing: SA for semi-active, then AAM. It
    // reads awkwardly and the alternative was worse -- SAAM was the odd one out in
    // a set of four where every other name is two words abbreviated to two
    // letters, and a convention with one exception is not a convention. A miscount
    // here does not compile, which is the only reason it is safe to be ugly.
    WPN_SAAAM,
};

enum ShipClass : uint8_t {
    SHIP_AEGIS = 0,
    SHIP_LANCE,
    SHIP_CHARIOT,
    SHIP_BALLISTA,
    SHIP_CLASSES
};

struct ShipSpec {
    const char* name;
    const char* tagline;       // one line for the select screen -- the quality
                               // the class is named for, in the player's terms

    // --- airframe ------------------------------------------------------------
    float speed_min;           // world units/sec at zero throttle
    float speed_max;           // ...and at full throttle
    float turn_rate;           // rad/sec at full deflection, before agility
    float agility_slow_bonus;  // extra turn-rate fraction at idle
    float agility_fast_malus;  // turn-rate fraction lost at full throttle
    float hull;                // absolute hull points, NOT a fraction
    // How nervous the airframe is under power, as a multiplier on the buzz. This
    // is mass and structure as the player feels it rather than as a number they
    // are shown: a light hull rattles, a gun platform does not.
    float shake;

    // --- the warhead this ship launches --------------------------------------
    float msl_damage;          // hull points on a dead-centre detonation
    // The fuse radius, and it does TWO jobs: inside it is a kill, and it is also
    // the scale the falloff runs over -- a detonation at 0 does full damage, one
    // out at the rim does msl_graze_floor of it.
    //
    // Wider is not automatically better and narrower is not automatically
    // sharper. A missile closing head-on shuts the range at up to 760 units/sec,
    // which is about 12 units per simulation step, so a radius near that figure
    // gets stepped over between samples: the round arrives, the fuse never sees
    // the inside of the sphere, and the player is told MISSED on a shot that
    // looked perfect. A tight fuse is a real shooting demand; a fuse tighter than
    // the simulation can resolve is a lie.
    float msl_splash;
    float msl_graze_floor;     // fraction of that at the rim of the fuse radius
    // The speed it LEAVES THE RAIL at. Not the speed it flies at, if the class
    // accelerates -- see msl_accel.
    float msl_speed;
    // HOW MUCH FASTER IT GETS FOR EVERY SECOND THE LAUNCHER KEEPS THE LOCK, in
    // units per second per second. Zero for a class whose round flies at one
    // speed, which is three of the four.
    //
    // THE AIM IS THE ENGINE. A semi-active round already dies the moment the
    // launcher looks away; this makes holding the lock do something POSITIVE as
    // well as merely staying alive, so the aim is paid for rather than only
    // policed. Twenty seconds of holding somebody in view is a long time to be
    // predictable, and it should buy something.
    //
    // It also settles a plain absurdity: a BALLISTA airframe does 340 and its
    // round left the rail at 200, so the sniper outran its own ordnance. It still
    // does at the moment of launch, and stops doing so a few seconds later if the
    // pilot earns it.
    float msl_accel;
    // ...and the ceiling. Equal to msl_speed for a class that does not
    // accelerate, so the two fields together are always the true speed range.
    float msl_speed_max;
    // WHAT THE ROUND HAS TO BE DOING TO EARN THAT ACCELERATION, as the cosine
    // between its own heading and the bearing to its target.
    //
    // -2.0f means no requirement, and a cosine can never reach it -- the same
    // idiom msl_reacq_cos uses for "never". A class with the sentinel accelerates
    // whenever it is guided, which is BALLISTA: there the engine is the LAUNCHER's
    // aim, and holding somebody in view is what pays.
    //
    // A real cosine here makes the engine the ROUND's own geometry instead, and
    // that is LANCE. Because the seeker flies lead pursuit, this splits the class
    // by the shape of the fight rather than by any rule about it: head-on and
    // stern chase put almost no lead on the round, its nose sits on the target,
    // and it spools up the whole way in. A crossing shot is aimed AHEAD of where
    // they are, so the nose is off the bearing and the round never gets fast.
    //
    // It compounds in both directions on purpose. A round that is not
    // accelerating stays slow, which needs MORE lead, which keeps it off the
    // nose. One that is accelerating gets faster, which needs LESS lead, which
    // holds it on. Good geometry runs away with it; bad geometry never recovers.
    float msl_accel_cos;
    // WHAT IS LEFT OF THE WARHEAD AT THE FAR END OF ITS REACH, as a fraction.
    // 1.0f means no falloff, which is three of the four.
    //
    // A long shot is not free. A round that has flown two thousand units arrives
    // with less than one that has flown two hundred, and that turns a reach class
    // from "shoot from wherever they cannot answer" into a weapon with a BAND it
    // wants to be in. Without it, the biggest lock range in the game plus a
    // parked ship is simply the best play available -- the enemy locks at 1600,
    // BALLISTA at 4200, and nothing that happens in between is a fight.
    //
    // It runs over the ship's own lock_range, so it is the class describing its
    // own envelope rather than a distance imposed from outside.
    float msl_reach_floor;
    // HOW LONG A SEMI-ACTIVE ROUND KEEPS GUIDING ONCE THE LIGHT GOES OUT.
    //
    // Per class rather than one number for the game, because it is the class's
    // bargain and not the weapon's physics. Zero means ALL OR NOTHING: the light
    // goes out and the round is finished on that frame.
    //
    // BALLISTA HAS 0.60 NOW, and it used to be the zero. All or nothing was the
    // right shape and slightly the wrong size: reported from the cockpit as "it
    // can make or break", with a session ending early because a lock lost for an
    // instant took every round in the air with it. Six tenths of a second is long
    // enough to get the nose back on somebody and not long enough to fly away and
    // return -- the round still dies if you actually leave.
    //
    // WATCH WHO IT HELPS. The grace is worth most to a pilot who loses the lock
    // OFTEN, and that is the scripted seat: it drops illumination three hundred
    // times in a measured set where a human dropped it on 15% of rounds. So the
    // table's +60% for this class overstates what a player will feel, and tuning
    // the warhead down to pay for it would be paying with the wrong instrument.
    float msl_coast;
    // HOW HARD THIS CLASS PAYS FOR SPEED IN AGILITY, as an exponent on
    // launch_speed/speed. Zero is off and the table's turn rate stands.
    //
    // A missile holding a fixed sideways acceleration turns at a/v, so going
    // faster necessarily means turning worse. At 1.0 that is modelled honestly and
    // the speed a pilot earns by holding the lock is spent on commitment: the
    // round is fastest exactly when it is least able to correct.
    //
    // Per class rather than global because it is a statement about what a class
    // IS, and because the honest value takes a LANCE round to 1.06 rad/s at full
    // speed -- under the 1.20 that made that class useless before its seeker was
    // fixed. LANCE keeps 0.0 until its own rework says otherwise.
    float msl_turn_trade;
    // SECONDS OF HELD LOCK PER STACK, or zero for a class that fires one at a time.
    //
    // A stacking class does not shoot when it has a lock; it BANKS the lock, and
    // the trigger empties whatever has been banked in one salvo. The aim it asks
    // for is loose -- keep them roughly in front -- and the cost is paid in TIME
    // instead, which is a different skill from BALLISTA's and deliberately so.
    //
    // Losing them empties the bank on the frame it happens, so the demand is
    // sustained contact rather than precision. Four stacks at 0.95s is a little
    // under four seconds of unbroken tracking for a full salvo, in fights that
    // turn over faster than that.
    //
    // It was 1.2s, which flew correctly and read as slightly too long a toll: the
    // build-up was a trade being weighed rather than a rhythm being played. The
    // interval is the tempo dial for this class and the only one -- lock_time
    // decides how hard the FIRST stack is to earn, and the two are different
    // questions.
    float msl_stack_time;
    float msl_turn;            // rad/sec -- the seeker's agility
    float msl_life;            // seconds before it self-destructs
    // Seconds off the rail before the lead solution is allowed to steer. NOT a
    // safety arming delay -- the round can detonate at any age; what this gates
    // is lead correction only. A fast round with a wide turn radius wants to
    // leave straight, because the lead term will otherwise spend the only turn
    // it has in the first tenth of a second.
    float msl_arm_time;

    // The cone the bearing must stay inside for the lock to hold. Wider is not
    // better: a round that cannot be shaken is a round that is not a decision.
    //
    // Three classes sit near 60 degrees. CHARIOT IS AT 127, and that is the
    // resolution of a contradiction rather than a buff.
    //
    // Its warhead is the most forgiving in the game -- the widest splash and a
    // 0.85 graze floor, so a near miss still does most of its damage -- and it was
    // bolted to the only seeker that cannot hold a target, 1.70 rad/s against
    // hulls that turn 2.20. Measured: FORTY-EIGHT PER CENT of its rounds died to
    // this cone, and another fifteen sailed into a wall. A warhead built to
    // forgive misses never got the chance, because half the magazine was not in
    // the neighbourhood to miss FROM.
    //
    // So the low turn stops being a defect and becomes the mechanic. The round
    // cannot follow a hard break and no longer tries: it flies a lazy lead and
    // turns up NEAR you, which is what the splash is for. The counter is range and
    // angle -- get properly behind its nose -- rather than one hard turn.
    //
    // AND THE WARHEAD CAME DOWN WITH IT, 12 to 10. Widening the cone did not make
    // each round hit harder, it made most of them arrive at all -- so the class's
    // output nearly doubled with no change to what a hit is worth, and the author
    // flew it and reported it as very lethal. The cut is on the WARHEAD rather
    // than the cone on purpose: narrowing the cone would have taken back the fix,
    // where a smaller warhead leaves every round arriving and only makes each one
    // matter less, which is what a chaff cloud is supposed to be. Connect stays at
    // 72% either way -- measured across 10, 9, 8 and 7.
    //
    // NOT INFINITE, and that was measured too. A sentinel meaning "never gives up"
    // was the obvious version and it took the connect rate to 100%: nothing
    // escaped, no round ever met a wall, and the header line above stopped being
    // true. At 127 degrees a quarter are still shaken off and the damage lands
    // second in the roster rather than first.
    float msl_seeker_cos;
    // ...and the cone it may RE-acquire through, after coasting ballistic for
    // msl_reacq_delay. 2.0f means never, which is what a cosine can never
    // reach and therefore the honest way to spell "this class does not come back".
    float msl_reacq_cos;
    // How long it coasts before the seeker may try again. Long enough that the
    // player gets the moment of having beaten it -- without that beat,
    // re-acquisition would just read as a lock that never broke.
    //
    // Inert for any class whose msl_reacq_cos is the 2.0f sentinel. It is still
    // set on all four on purpose: a class that later gains re-acquisition would
    // otherwise silently inherit a zero delay, which is the unbreakable lock this
    // whole mechanism exists to avoid.
    float msl_reacq_delay;

    // --- how the AI flies it -------------------------------------------------
    ShipTactic tactic;

    // --- fire control --------------------------------------------------------
    // WHAT THIS SHIP SHOOTS WITH, and the authority on it. Every instrument and
    // every per-class branch in the weapon code asks this; none of them infers a
    // class's mechanic from the tuning underneath. See WeaponSystem.
    WeaponSystem wpn;

    // The nose cone the target must be inside to acquire, as a cosine. This is
    // the "you must aim" half of every lock in the game, and for a class with no
    // lock time at all it is the ONLY requirement -- which is what makes turning
    // away the counter, and what forces the choice between dodging and holding
    // the nose on target.
    //
    float lock_cos;
    // ...and the cone it is HELD in once acquired, which is a different question
    // and now a different number.
    //
    // ACQUIRING AND HOLDING ARE NOT THE SAME SKILL. Acquiring is the aim: put the
    // nose on them and the shot exists. Holding is only asking whether you have
    // lost them, and a ship drifting to the corner of the canopy has not been
    // lost -- it is right there, in plain view. Judging both with one cone means
    // either the aim is free or a visible target reads as gone.
    //
    // BELOW -1 IS NOT A CONE. A cosine cannot reach -2, so that is the honest way
    // to spell "the viewport instead" -- held for as long as it is on screen. Same
    // idiom as msl_reacq_cos's 2.0f for "never". At FOCAL 400 on a 480 px panel a
    // 0.86 cone is the circle INSCRIBED in the viewport, so a corner is 40 degrees
    // out: visible, and outside the cone. That gap is what this closes.
    //
    // Set it equal to lock_cos for a class that should not tell them apart.
    float lock_hold_cos;
    float lock_range;
    float lock_time;           // seconds in the nose cone, at low speed
    int   magazine;            // rounds per clip
    float fire_gap;            // seconds between launches -- the rate of fire
    // Seconds to refill the rack. PER CLIP for three of the four, and PER ROUND for
    // AR-AAM -- the weapon system says which, the same way it says what a trigger
    // press costs. One field with one meaning per weapon beats a second field
    // sitting dead on three rows. See vg_wpn_reload_step.
    //
    // Seconds to refill the WHOLE clip, and only once it is empty. Not a trickle
    // of one round at a time: emptying the rack is a decision the class is built
    // around, and a magazine that refills while you are still shooting out of it
    // never lets that decision cost anything.
    float reload;
};

extern const ShipSpec vg_ship_class[SHIP_CLASSES];

// The lock cone this class actually enforces at a given range, as a cosine.
//
// For a class with a real lock_hold_cos it narrows as 1/range beyond
// LOCK_TIGHTEN_REF -- see the note there. For one that uses the -2.0f sentinel it
// is the cone unchanged, because its rule is the viewport and a viewport does not
// narrow.
//
// It said THREE when it was written and LANCE is the only one now. That drift is
// not harmless trivia: the guide circle was gated on this same sentinel, on the
// same stale assumption about how many classes wore it, and it silently handed
// AEGIS and CHARIOT a mechanic belonging to BALLISTA. A count in a comment is a
// fact with no way to check itself, so do not put one here again. `hold` picks the holding cone over the acquiring one.
float vg_lock_cos_at(const ShipSpec* sp, float range, bool hold);

// Whether this class's rounds are flown by the SHOOTER for their whole life.
//
// Derived, and that is the entire point. It was a `msl_saam` bool sitting beside
// the system in the table -- two fields carrying one fact, kept honest by a
// static_assert that tested them against each other. An assert is the right tool
// when two independent numbers must agree; it is the wrong one when the second
// number was never independent. Now there is nothing to disagree.
//
// The reading site still gets to ask its own question: the missile code wants to
// know about the ROUND, not about the designation on the box.
static inline bool vg_msl_semi_active(const ShipSpec* sp) {
    return sp && sp->wpn == WPN_SAAAM;
}

static inline const ShipSpec* vg_spec(ShipClass c) {
    return &vg_ship_class[(c < SHIP_CLASSES) ? c : SHIP_AEGIS];
}

// How well this airframe rolls, relative to the baseline class. 1.0 is AEGIS.
//
// DERIVED, not a new number to keep in step. A dedicated roll_rate per class
// would be one more field to retune every time a ship's handling moved, and the
// first time somebody forgot, a ship would turn like a knife and roll like a
// barge for no reason anybody could find in the table.
//
// Two terms, and the second is the interesting one. turn_rate is the obvious
// part: a nimble airframe rolls nimbly. agility_fast_malus is what a class loses
// at full throttle, and roll is the HIGH-SPEED control -- so the class built to
// keep working fast should be the one that gets the most out of it, and the one
// that seizes up at speed should be denied its best tool exactly where it is
// already struggling. That turns a 1.4x spread in turn rate into better than 2x
// in roll, which is the difference between a stat and a personality.
//
// Normalised against AEGIS by reading its spec rather than against a constant,
// so retuning the baseline moves everything with it instead of silently
// rescaling every other class.
static inline float vg_ship_mobility(const ShipSpec* s) {
    const ShipSpec* base = &vg_ship_class[SHIP_AEGIS];
    const float b = base->turn_rate * (1.0f - base->agility_fast_malus);
    if (b <= 0.0f) return 1.0f;
    return (s->turn_rate * (1.0f - s->agility_fast_malus)) / b;
}
