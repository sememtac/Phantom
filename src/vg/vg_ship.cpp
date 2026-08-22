#include "vg_ship.h"

// First pass at the four classes. Every number here is internally consistent but
// unvalidated on hardware -- see DESIGN.md, which is the authority on intent.
//
// AEGIS reproduces the tuning the game shipped with, so it is the control: if a
// change makes AEGIS feel worse, the change is wrong regardless of what it does
// for the other three.
//
// Reading the table: against a 110-hull AEGIS these come out at roughly AEGIS 6
// clean hits, LANCE 4 clean but 17 if it keeps grazing, CHARIOT 10 out of a
// twelve-round magazine, BALLISTA 4.
//
// BALLISTA was 3, which meant one rack was a kill on any hull in the game -- fly
// out past what anyone can answer, land the magazine, win. A sniper should be
// able to kill from range; it should not be able to do it with a magazine that
// cannot fail to be lethal. At 30 a full rack is 90 against 110: a serious wound
// that has to be followed up, and still the most a single round carries after
// LANCE's clean hit.

constexpr ShipSpec vg_ship_class[SHIP_CLASSES] = {

    // ---- AEGIS -- the shield -------------------------------------------------
    // The reference ship, and the one an average player should have a good time
    // with having never picked anything else.
    {
        "AEGIS", "NO WEAKNESS, NO EDGE",
        /* speed      */ 100.0f, 420.0f,
        /* turn       */ 1.90f, 0.75f, 0.30f,
        /* hull       */ 110.0f, /* shake */ 1.00f,
        /* warhead    */ 20.0f, 18.0f, 0.60f, 340.0f, 2.50f, 10.0f, 0.22f,
        /* seeker     */ 0.50f, 2.0f, 0.9f,
        /* fire ctrl  */ 0.86f, 0.86f, 1600.0f, 0.45f, 6, 0.50f, 5.0f,
    },

    // ---- LANCE -- the point --------------------------------------------------
    // A 0.20 graze floor is the whole ship: a rim detonation does a fifth of the
    // damage, so only correct geometry pays. It fires at very nearly AEGIS's
    // cadence ON PURPOSE -- the two should feel alike on the trigger, so that what
    // separates them is what happens when the round arrives, not how often one
    // can be sent. The cost is four rounds instead of six, a longer lock, and six
    // and a half seconds unarmed if they are spent badly.
    {
        "LANCE", "CLEAN HITS ONLY",
        /* speed      */ 100.0f, 390.0f,
        /* turn       */ 1.90f, 0.75f, 0.30f,
        /* hull       */ 95.0f,  /* shake */ 1.30f,
        /* warhead    */ 32.0f, 14.0f, 0.20f, 520.0f, 1.20f, 10.0f, 0.35f,
        /* seeker     */ 0.50f, 2.0f, 0.9f,
        /* fire ctrl  */ 0.90f, 0.90f, 1600.0f, 0.60f, 4, 0.55f, 6.5f,
    },

    // ---- CHARIOT -- the speed ------------------------------------------------
    // The inverse of LANCE: a 0.85 floor means aim barely matters and volume
    // does. Keeps far more of its agility at full throttle than anything else,
    // which is what lets it run circles -- paid for with a 70-point hull.
    //
    // RUN AND GUN, and the fire control is where that lives now: twelve rounds at
    // a sixth of a second empties the whole rack in under two, and then TEN
    // seconds of nothing -- five times as long as it took to spend. It is the
    // only class that can delete an opponent in a single pass and the only one
    // that is completely toothless if it does not.
    {
        "CHARIOT", "FAST, LOUD, FRAGILE",
        /* speed      */ 100.0f, 460.0f,
        /* turn       */ 2.20f, 0.60f, 0.15f,
        /* hull       */ 70.0f,  /* shake */ 1.70f,
        /* warhead    */ 12.0f, 22.0f, 0.85f, 380.0f, 1.70f, 7.0f, 0.12f,
        /* seeker     */ 0.52f, 2.0f, 0.9f,
        /* fire ctrl  */ 0.80f, 0.80f, 1300.0f, 0.25f, 12, 0.16f, 10.0f,
    },

    // ---- BALLISTA -- the range -----------------------------------------------
    // A missile SLOWER than everything it shoots at but alive for twenty seconds:
    // it cannot run you down, it just refuses to go away -- and now it means that
    // literally. It is the only seeker in the game that RE-ACQUIRES, so breaking
    // its lock buys a pass rather than a kill.
    //
    // Locks from 4200 out and takes no time to do it -- but it has to be ACQUIRED
    // in the nose cone like anything else, and only then does it hold for as long
    // as the target is anywhere on screen. That split is the whole ship. The cone
    // is the aim, and the aim is the price; the viewport is only the game agreeing
    // that a ship in the corner of the canopy has not vanished.
    //
    // Held in one test, it broke either way round. A cone tight enough to be an
    // aiming requirement drops targets in plain view; a window wide enough to keep
    // them makes the aim free -- and free is what a playtest called it: point
    // roughly, fire, win. A lock that breaks now costs the nose to get back, which
    // is what puts the choice between dodging and shooting back into a head-on.
    //
    // 4200 is not a taste, it is the WORLD. CULL_RADIUS is 4200 and so is the
    // arena's major radius, so a longer lock would be a lock on something the
    // round can never reach. 320 x 20s of travel services it with the arc to
    // spare, and the missile cull follows the class so the shot is not deleted at
    // the moment it arrives.
    //
    // Three rounds on a nine second reload, a slow trigger between them, and it
    // loses almost all its agility at speed -- so anything fast that gets inside is
    // its whole problem, and running the rack dry is how it gets there.
    {
        "BALLISTA", "KILL THEM FIRST",
        /* speed      */ 100.0f, 340.0f,
        /* turn       */ 1.60f, 0.85f, 0.45f,
        /* hull       */ 90.0f,  /* shake */ 0.55f,
        /* warhead    */ 30.0f, 17.0f, 0.50f, 320.0f, 2.30f, 20.0f, 0.30f,
        /* seeker     */ 0.42f, -0.30f, 0.9f,
        /* fire ctrl  */ 0.86f, -2.0f, 4200.0f, 0.0f, 3, 1.60f, 9.0f,
    },
};

// WHAT THE TABLE IS NOT ALLOWED TO SAY.
//
// The rows are POSITIONAL. A miscounted row does not fail to compile, it silently
// reassigns every value after the mistake, and a short row zero-fills the tail --
// which lands on msl_splash and msl_speed, both of which are divisors. These catch
// that at build time instead of in a fight.
//
// The reach test is the one worth having. A class whose round cannot fly as far as
// its own lock range will let the player earn a LOCK, fire, and watch the missile
// expire on the way -- which reads as a broken weapon, not as a range limit. 1.4x
// is the pursuit-curve allowance: a seeker flies an arc, not a chord.
#define SHIP_INVARIANTS(C)                                                            static_assert(vg_ship_class[C].msl_splash > 0.0f,  #C " splash is a divisor");     static_assert(vg_ship_class[C].msl_speed  > 0.0f,  #C " speed is a divisor");      static_assert(vg_ship_class[C].magazine   > 0,     #C " magazine is a divisor");     static_assert(vg_ship_class[C].reload     > 0.0f,  #C " reload is a divisor");     static_assert(vg_ship_class[C].speed_max  > vg_ship_class[C].speed_min,                          #C " speed span is a divisor");                                      static_assert(vg_ship_class[C].msl_speed * vg_ship_class[C].msl_life                             > vg_ship_class[C].lock_range * 1.4f,                                              #C " cannot reach its own lock range")

SHIP_INVARIANTS(SHIP_AEGIS);
SHIP_INVARIANTS(SHIP_LANCE);
SHIP_INVARIANTS(SHIP_CHARIOT);
SHIP_INVARIANTS(SHIP_BALLISTA);

#undef SHIP_INVARIANTS

