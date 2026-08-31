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

    // SEMI-ACTIVE: THE LAUNCHER HAS TO KEEP LOOKING AT THEM.
    //
    // A normal round here is fire-and-forget -- it carries its own seeker, and
    // once it is away the launcher may do as it likes. A semi-active one has no
    // seeker worth the name: it flies down a target the LAUNCHER is holding, and
    // the moment that lock is lost the round has nothing to follow. Immediately,
    // and for good -- it keeps its heading and sails on like any other broken
    // lock.
    //
    // BALLISTA'S FANTASY, STATED AS A RULE. Its reach is the longest in the game
    // and its round is alive for thirty-two seconds; what pays for that is not
    // being allowed to look away. Firing across the arena now means holding the
    // target in view for the twenty seconds the round takes to arrive -- twenty
    // seconds of not manoeuvring, not turning on anybody else, and being
    // perfectly predictable to the person being shot at. Reported from play as
    // the class feeling too strong precisely because none of that was true.
    //
    // WHOSE VIEW NEEDS NO CONE OF ITS OWN. It is the class's lock_hold_cos, and
    // BALLISTA's is the viewport idiom -- so the lock holds for exactly as long
    // as the target is on screen, which is the promise the fantasy makes.
    bool  msl_saam;

    // --- how the AI flies it -------------------------------------------------
    ShipTactic tactic;

    // --- fire control --------------------------------------------------------
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
    // Seconds to refill the WHOLE clip, and only once it is empty. Not a trickle
    // of one round at a time: emptying the rack is a decision the class is built
    // around, and a magazine that refills while you are still shooting out of it
    // never lets that decision cost anything.
    float reload;
};

extern const ShipSpec vg_ship_class[SHIP_CLASSES];

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
