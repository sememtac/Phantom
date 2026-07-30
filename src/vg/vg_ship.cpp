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
// twelve-round magazine, BALLISTA 3.

const ShipSpec vg_ship_class[SHIP_CLASSES] = {

    // ---- AEGIS -- the shield -------------------------------------------------
    // The reference ship, and the one an average player should have a good time
    // with having never picked anything else.
    {
        "AEGIS", "NO WEAKNESS, NO EDGE",
        /* speed      */ 100.0f, 420.0f,
        /* turn       */ 1.90f, 0.75f, 0.30f,
        /* hull       */ 110.0f,
        /* warhead    */ 20.0f, 0.60f, 340.0f, 2.50f, 10.0f,
        /* fire ctrl  */ 1600.0f, 0.45f, 6, 0.50f, 5.0f,
    },

    // ---- LANCE -- the point --------------------------------------------------
    // A 0.20 graze floor is the whole ship: a rim detonation does a fifth of the
    // damage, so only correct geometry pays. Slightly slower than AEGIS and four
    // rounds instead of six, so a wasted shot is felt twice -- once as a quarter
    // of the magazine, and again as six and a half seconds of being unarmed.
    {
        "LANCE", "CLEAN HITS ONLY",
        /* speed      */ 100.0f, 390.0f,
        /* turn       */ 1.90f, 0.75f, 0.30f,
        /* hull       */ 95.0f,
        /* warhead    */ 32.0f, 0.20f, 340.0f, 2.50f, 10.0f,
        /* fire ctrl  */ 1600.0f, 0.60f, 4, 0.85f, 6.5f,
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
        /* hull       */ 70.0f,
        /* warhead    */ 12.0f, 0.85f, 380.0f, 1.70f, 7.0f,
        /* fire ctrl  */ 1300.0f, 0.25f, 12, 0.16f, 10.0f,
    },

    // ---- BALLISTA -- the range -----------------------------------------------
    // A missile SLOWER than everything it shoots at but alive for eighteen
    // seconds: it cannot run you down, it just refuses to go away. Three rounds
    // on a nine second reload, a slow trigger between them, and it loses almost
    // all its agility at speed -- so anything fast that gets inside 2800 is its
    // whole problem, and running the rack dry is how it gets there.
    {
        "BALLISTA", "KILL THEM FIRST",
        /* speed      */ 100.0f, 340.0f,
        /* turn       */ 1.60f, 0.85f, 0.45f,
        /* hull       */ 90.0f,
        /* warhead    */ 40.0f, 0.50f, 300.0f, 2.30f, 18.0f,
        /* fire ctrl  */ 2800.0f, 1.10f, 3, 1.60f, 9.0f,
    },
};
