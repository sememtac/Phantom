#include "vg_pilot.h"

// Five of them, weakest first, and the order is load bearing: vg_pilot_for
// indexes this table with a rating and nothing sorts it first.
//
// READING THE TABLE. The interesting column is `aim`, because it is the one the
// game never had. 0.09 rad is about five degrees of wander, which against a
// lock cone of roughly thirty is not a miss -- it is a pilot who takes noticeably
// longer to earn a lock, loses it to a turn that a steadier pilot would have
// ridden out, and puts the occasional round somewhere useless. That is what the
// bottom of the table should feel like: not harmless, just loose.
//
// The top of the table is deliberately NOT perfect. 0.012 rad still wanders
// about seven tenths of a degree, and reaction never reaches zero, because a
// pilot with no error at all is the machine this table exists to stop shipping.
// The ceiling is somebody very good, not somebody unbeatable.
const PilotSpec vg_pilot_kind[PILOT_KINDS] = {
    // A first tournament. Points roughly, notices late, and leaves early --
    // three separate reasons the same fight is survivable, which is better than
    // one big handicap because the player can see each of them happening.
    { "RAW", /* fly */ 0.62f, /* aim */ 0.090f, /* hold */ 1.30f,
             /* react */ 0.62f, /* nerve */ 0.80f, /* press */ 0.55f },

    // Has flown a few. Still loose on the trigger and still breaks off a merge
    // they were winning.
    { "JRN", /* fly */ 0.74f, /* aim */ 0.055f, /* hold */ 1.05f,
             /* react */ 0.44f, /* nerve */ 0.92f, /* press */ 0.75f },

    // The middle of the table, and the one the gym sends. This is roughly what
    // every enemy in the game used to be, ENEMY_SKILL having been 0.82.
    { "PRO", /* fly */ 0.82f, /* aim */ 0.034f, /* hold */ 0.85f,
             /* react */ 0.30f, /* nerve */ 1.00f, /* press */ 1.00f },

    // Stays in fights. The step up here is `press` more than `fly`: this is the
    // first pilot in the table who will sit on a tail rather than take the shot
    // and leave, and being unable to shake somebody is a different kind of hard
    // from being out-turned.
    { "VET", /* fly */ 0.90f, /* aim */ 0.021f, /* hold */ 0.70f,
             /* react */ 0.20f, /* nerve */ 1.14f, /* press */ 1.30f },

    // The far side of the draw. Quick to answer, steady on the nose, and does
    // not give a won position back.
    { "ACE", /* fly */ 0.97f, /* aim */ 0.012f, /* hold */ 0.55f,
             /* react */ 0.13f, /* nerve */ 1.28f, /* press */ 1.55f },
};

const PilotSpec* vg_pilot_default(void) { return &vg_pilot_kind[2]; }

const PilotSpec* vg_pilot_for(float rating) {
    if (rating < 0.0f) rating = 0.0f;
    if (rating > 1.0f) rating = 1.0f;
    // Nearest tier rather than a blend. The traits are not independently
    // meaningful part-way between two rows -- a pilot is a person, and half of
    // one is not a weaker person, it is a number nobody chose.
    int i = (int)(rating * (float)(PILOT_KINDS - 1) + 0.5f);
    if (i < 0) i = 0;
    if (i >= PILOT_KINDS) i = PILOT_KINDS - 1;
    return &vg_pilot_kind[i];
}
