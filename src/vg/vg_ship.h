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

    // --- the warhead this ship launches --------------------------------------
    float msl_damage;          // hull points on a dead-centre detonation
    float msl_graze_floor;     // fraction of that at the rim of the fuse radius
    float msl_speed;
    float msl_turn;            // rad/sec -- the seeker's agility
    float msl_life;            // seconds before it self-destructs

    // --- fire control --------------------------------------------------------
    float lock_range;
    float lock_time;           // seconds in the nose cone, at low speed
    int   magazine;
    float reload;              // seconds per rearmed round
};

extern const ShipSpec vg_ship_class[SHIP_CLASSES];

static inline const ShipSpec* vg_spec(ShipClass c) {
    return &vg_ship_class[(c < SHIP_CLASSES) ? c : SHIP_AEGIS];
}
