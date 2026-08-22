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
    float msl_speed;
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

    // --- fire control --------------------------------------------------------
    // The nose cone the target must be inside to acquire, as a cosine. This is
    // the "you must aim" half of every lock in the game, and for a class with no
    // lock time at all it is the ONLY requirement -- which is what makes turning
    // away the counter, and what forces the choice between dodging and holding
    // the nose on target.
    float lock_cos;
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
