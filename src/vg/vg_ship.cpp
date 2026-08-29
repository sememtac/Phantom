#include "vg_ship.h"

// First pass at the four classes. Every number here is internally consistent but
// unvalidated on hardware -- see DESIGN.md, which is the authority on intent.
//
// AEGIS reproduces the tuning the game shipped with, so it is the control: if a
// change makes AEGIS feel worse, the change is wrong regardless of what it does
// for the other three.
//
// EVERY HULL IS THREE TIMES WHAT IT WAS, and that is an experiment about TIME TO
// KILL, not a re-tune of the classes. The complaint it answers: an opponent could
// die in a flyby, which reads as cheap rather than as decisive. Warheads are
// untouched, so the ratios between the four are exactly what they were and only the
// clock moved.
//
// DOUBLE WAS FLOWN FIRST and it worked -- the report was that the fight came alive,
// because a pilot who lost the first exchange now has time to answer instead of the
// match being decided by whoever got the drop. This is that result taken one step
// further to find where it stops improving. It is a probe, and the number to beat
// is 2x, not the original 1x.
//
// Reading the table: against a 330-hull AEGIS these come out at roughly AEGIS 17
// clean hits, LANCE 10 clean but 52 if it keeps grazing, CHARIOT 28 out of a
// twelve-round magazine, BALLISTA 11.
//
// SETTLED, AND THE FEAR WAS WRONG. The worry was attrition: long stretches where
// neither pilot can convert and the reload becomes most of the match. Measured on
// the control matchup, AEGIS against AEGIS, both sides hand-written, four fixed
// seeds of twelve simulated minutes each:
//
//     hull   kills/min   mean fight   holding nothing to fire
//      1x       0.80        74.6s              2.9%
//      2x       0.51       118.6s              4.7%
//      3x       0.35       170.1s              5.2%
//
// The reload never becomes the match at any scaling. What changes is the clock,
// and it does NOT change in proportion -- doubling the hull buys 59% more fight
// and tripling buys 128%, because more hull is also more time to land hits and
// the two partly cancel. Worth knowing before anybody reaches for this dial again
// expecting it to be linear.
//
// 3x stays, on the author's call: it is the room a pilot who lost the first
// exchange needs to answer, which is what 2x was liked for.
//
// THESE ARE BOT FIGHTS and the scripted pilot converts badly, so the absolute
// seconds overstate a human match. The ratios are the part to trust.
//
// CHARIOT IS STILL THE ROW THAT CHANGES MOST. Its premise below is that it empties
// the rack into somebody and deletes them in one pass; a full magazine is 144,
// which killed a 110 AEGIS outright, was two thirds of the doubled 220, and is
// under half of this. If it feels toothless rather than fast, the correction is
// CHARIOT's warhead rather than putting the hulls back.
//
// BALLISTA was 3, which meant one rack was a kill on any hull in the game -- fly
// out past what anyone can answer, land the magazine, win. A sniper should be
// able to kill from range; it should not be able to do it with a magazine that
// cannot fail to be lethal. At 30 a full rack is 90, which was a serious wound that
// had to be followed up against the old 110 and is under a third of this -- still
// the most a single round carries after LANCE's clean hit.

// CONSTEXPR ON THE FIRMWARE BUILD ONLY, and it is not a preference.
//
// The invariants below read the table at compile time, which needs it constexpr.
// MSVC will not accept constexpr on a definition whose header declaration says
// `extern const`, and the header cannot say constexpr instead, because a
// constexpr declaration without an initialiser is ill-formed. Both requirements
// cannot be met there at once.
//
// So the desktop build takes const and goes without the checks, which is the
// right way round: the firmware is what ships, and it is the build that would
// otherwise carry a miscounted row into a fight.
#if defined(_MSC_VER)
const ShipSpec vg_ship_class[SHIP_CLASSES] = {
#else
constexpr ShipSpec vg_ship_class[SHIP_CLASSES] = {
#endif

    // ---- AEGIS -- the shield -------------------------------------------------
    // The reference ship, and the one an average player should have a good time
    // with having never picked anything else.
    {
        "AEGIS", "NO WEAKNESS, NO EDGE",
        /* speed      */ 100.0f, 420.0f,
        /* turn       */ 1.90f, 0.75f, 0.30f,
        /* hull       */ 330.0f, /* shake */ 1.00f,
        /* warhead    */ 20.0f, 18.0f, 0.60f,
        /* speed      */ 340.0f, /* accel */ 0.0f, /* max */ 340.0f,
        /* seeker arc */ 2.50f, 10.0f, 0.22f,
        /* seeker     */ 0.50f, 2.0f, 0.9f, /* saam */ false,
        /* tactic     */ TACTIC_FIGHTER,
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
        /* hull       */ 285.0f, /* shake */ 1.30f,
        /* warhead    */ 32.0f, 14.0f, 0.20f,
        /* speed      */ 520.0f, /* accel */ 0.0f, /* max */ 520.0f,
        /* seeker arc */ 1.20f, 10.0f, 0.35f,
        /* seeker     */ 0.50f, 2.0f, 0.9f, /* saam */ false,
        /* tactic     */ TACTIC_GEOMETRY,
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
        /* hull       */ 210.0f, /* shake */ 1.70f,
        /* warhead    */ 12.0f, 22.0f, 0.85f,
        /* speed      */ 380.0f, /* accel */ 0.0f, /* max */ 380.0f,
        /* seeker arc */ 1.70f, 7.0f, 0.12f,
        /* seeker     */ 0.52f, 2.0f, 0.9f, /* saam */ false,
        /* tactic     */ TACTIC_SLASH,
        /* fire ctrl  */ 0.80f, 0.80f, 1300.0f, 0.25f, 12, 0.16f, 10.0f,
    },

    // ---- BALLISTA -- the range -----------------------------------------------
    // A missile that LEAVES THE RAIL at 200 -- half the speed of the airframes it
    // is shooting at -- and then builds speed for every second the pilot keeps the
    // target in view, to a ceiling of 480. Alive for thirty-two seconds.
    //
    // THE AIM IS THE ENGINE. Semi-active guidance already killed the round the
    // moment the launcher looked away, which made holding the lock a tax. This
    // makes it an investment as well: twenty seconds of holding somebody in view
    // is a long time to be predictable and it should buy something, and what it
    // buys is a round nothing in the game can outrun. The counter is not speed any
    // more, it is breaking the lock -- which is the counter the class was always
    // supposed to have.
    //
    // IT ALSO SETTLES AN ABSURDITY. The airframe does 340 and the round left at
    // 200, so the sniper outran its own ordnance. It still does at the moment of
    // launch, and stops doing so after about five seconds of holding the aim.
    //
    // SEMI-ACTIVE, AND NO RE-ACQUISITION. It used to be the only seeker in the game
    // that came back after a broken lock, on the reasoning that breaking it should
    // buy a pass rather than a kill. Flown, the two together were too much: the
    // round could be fired, watched briefly, and left to find its own way home
    // while the pilot looked somewhere else entirely -- so the aiming this class is
    // supposed to be about stopped after the trigger. The reach is the same and the
    // round is as patient as it ever was; what it costs now is the pilot's
    // attention for the whole twenty seconds it is in the air.
    //
    // The slowness is the aiming cost, paid AFTER the trigger. At 320 the round
    // arrived almost as soon as it was sent, so the shot was over the moment the
    // lock existed and the aim was worth nothing. At 200 a shot across the arena is
    // twenty seconds in the air: the pilot has to pick a moment that will still be
    // true when it gets there, and the target has every one of those seconds to
    // notice it and break. That is what makes it a decision instead of a hit.
    //
    // SPEED AND REACH ARE ONE NUMBER HERE. The round has to service its own lock
    // range or the LOCK is a lie -- see the reach static_assert below the table --
    // so cutting speed bought life to keep 200 x 32 = 6400 against 4200. The cost
    // is rounds staying airborne much longer, against a pool of MAX_MISSILES and
    // the busiest geometry in the frame. It is measured, not assumed.
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
        /* hull       */ 270.0f, /* shake */ 0.55f,
        // A SNIPER'S ROUND, and this is what the word has to mean in numbers.
        //
        // It was 30, and an AEGIS carries 330 hull: eleven dead centre hits, three
        // rounds to a rack and nine seconds to reload, so about fifty-five seconds
        // of perfect shooting to kill one thing. Measured over a real session the
        // class landed its shots and converted almost nothing, which is why it
        // read as harmless and got picked on.
        //
        // At 120 a rack that connects IS a kill: 2.8 hits on the toughest hull in
        // the game, two on the lightest. That is the trade the class is built
        // around -- three rounds, nine seconds dry, the slowest airframe here and
        // the worst turn at both ends of the throttle, in exchange for the shot
        // that ends it. Miss the rack and you have nothing for nine seconds.
        /* warhead    */ 120.0f, 17.0f, 0.50f,
        // THE ROUND IS THE CLASS. Reach is the privilege and it keeps it -- 4200,
        // untouched -- but reach is worthless if a round that was aimed properly
        // cannot arrive. It used to leave the rail at 200, slower than every ship
        // in the game cruises, and turn at 2.30 against a CHARIOT that turns 2.20:
        // a target that ran could not be caught and a target that turned barely
        // could. Measured, 12.6% of rounds arrived, and the aiming that the whole
        // class is built around bought almost nothing.
        //
        // 320 and 3.20 are what make aim pay: the same six seeds go to 42.4%
        // arriving on HALF the rounds fired. The acceleration stays as the reward
        // for keeping the nose on them, but at 60 it now crosses the fastest ship
        // in the game after 2.3 seconds of held lock rather than 9.
        /* speed      */ 320.0f, /* accel */ 60.0f, /* max */ 560.0f,
        /* seeker arc */ 3.20f, 32.0f, 0.30f,
        /* seeker     */ 0.42f, 2.0f, 0.9f, /* saam */ true,
        /* tactic     */ TACTIC_STANDOFF,
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

#if !defined(_MSC_VER)
SHIP_INVARIANTS(SHIP_AEGIS);
SHIP_INVARIANTS(SHIP_LANCE);
SHIP_INVARIANTS(SHIP_CHARIOT);
SHIP_INVARIANTS(SHIP_BALLISTA);
#endif

#undef SHIP_INVARIANTS

