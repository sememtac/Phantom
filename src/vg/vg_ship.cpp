#include "vg_ship.h"
#include "cfg_hud.h"   // LOCK_TIGHTEN_REF
#include <math.h>

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
        /* speed      */ 340.0f, /* accel */ 0.0f, /* max */ 340.0f, /* needs */ -2.0f, /* reach */ 1.00f, /* coast */ 0.00f, /* trade */ 0.00f, /* stack */ 0.00f,
        /* seeker arc */ 2.50f, 10.0f, 0.22f,
        /* seeker     */ 0.50f, 2.0f, 0.9f, /* saam */ false,
        /* tactic     */ TACTIC_FIGHTER,
        /* weapon     */ WPN_ARAAM,
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
        // THE ROUND IS THE GEOMETRY, and this is where "clean hits only" stops
        // being a slogan about the fuse and becomes one about the flight.
        //
        // It used to leave at 520 -- the fastest launch in the game -- and turn at
        // 1.20, the worst. That reads as a broken seeker: it cannot follow anything
        // that manoeuvres, and it landed 16% of what it fired for the worst time to
        // kill on the roster. The turn rate is NOT the fault and it stays. A
        // straight-line intercept barely has to turn, and a seeker that cannot be
        // rescued mid-flight is exactly what makes the set-up matter.
        //
        // Now it leaves SLOWER than the ship that fired it and earns everything
        // back: 340 on the rail, 220 a second while its nose is on them, up to 900,
        // which is twice the quickest hull in the game. Two and a half seconds to
        // the ceiling covers about 1550 units and LANCE locks at 1600 -- the ramp
        // and the reach are the same distance, so a shot taken at its own range
        // arrives at full speed and a knife-range shot gains nothing at all.
        // 1.2 SECONDS A STACK. The bay is four, so a full salvo is nearly five
        // seconds of unbroken contact -- long, in fights that turn over faster
        // than that. Partial releases of two or three are the ordinary play and a
        // full four is a payoff. The round profile is untouched: these are the
        // same geometry-gated darts, so a salvo on a good line is devastating and
        // one thrown across a turn is four rounds wasted at once.
        /* speed      */ 340.0f, /* accel */ 220.0f, /* max */ 900.0f, /* needs */ 0.98f, /* reach */ 1.00f, /* coast */ 0.00f, /* trade */ 0.00f, /* stack */ 0.95f,
        // FIRE AND FORGET, which is the half of the class the round could not keep.
        //
        // The bargain is that the PILOT earns the shot by lining the geometry up,
        // and is then free to break away -- unlike a semi-active round, which is
        // tethered to whoever fired it. That only works if the round can hold what
        // it left with. At 1.20 it could not: the nimblest hull in the game turns
        // 2.20, so any jink shook it, and measured it landed 11% of what it fired
        // and put 279 rounds into the arena wall.
        //
        // 2.80 turns 27% harder than anything that can be aimed at, so a break has
        // to be genuinely good rather than merely a turn. THE LINEAR IDENTITY DOES
        // NOT LIVE HERE any more, and it is better where it now sits: a target that
        // jinks forces the round to turn, turning costs it the acceleration, and it
        // arrives slow. Set the geometry up and it is fast and unanswerable; make
        // it work for the intercept and it is neither.
        /* seeker arc */ 2.80f, 10.0f, 0.35f,
        /* seeker     */ 0.50f, 2.0f, 0.9f, /* saam */ false,
        /* tactic     */ TACTIC_GEOMETRY,
        // HOLD IS THE VIEWPORT AGAIN, on purpose and for the opposite reason to
        // BALLISTA. This class does not ask for a circle -- it asks you to keep
        // them in front of you for five seconds while the bank fills, and -2.0f is
        // the idiom for "as long as they are on screen". Acquiring still costs a
        // 26 degree cone, so the contact has to be made deliberately.
        /* weapon     */ WPN_SLAAM,
        /* fire ctrl  */ 0.90f, -2.0f, 1600.0f, 0.60f, 4, 0.55f, 6.5f,
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
        /* speed      */ 380.0f, /* accel */ 0.0f, /* max */ 380.0f, /* needs */ -2.0f, /* reach */ 1.00f, /* coast */ 0.00f, /* trade */ 0.00f, /* stack */ 0.00f,
        /* seeker arc */ 1.70f, 7.0f, 0.12f,
        /* seeker     */ 0.52f, 2.0f, 0.9f, /* saam */ false,
        /* tactic     */ TACTIC_SLASH,
        /* weapon     */ WPN_RFAAM,
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
        /* speed      */ 320.0f, /* accel */ 60.0f, /* max */ 560.0f, /* needs */ -2.0f, /* reach */ 0.45f, /* coast */ 0.00f, /* trade */ 1.00f, /* stack */ 0.00f,
        // SHORT LIVED AND EXTREMELY AGILE, which is this class stated as two
        // numbers. The round is a LEASH rather than a launch: it goes where the
        // nose goes for as long as the nose is on them, and then it is gone.
        //
        // 5.0 rad/s is more than twice the nimblest hull in the game, so nothing
        // outruns a guided round by turning. The counter is not to dodge it, it is
        // to break the SHOOTER's aim -- which is the whole of a semi-active weapon
        // and the reason the circle exists.
        //
        // Twelve seconds against thirty-two, and thirty-two was not a limit at
        // all: the round could fly seventeen thousand units against a lock range
        // of four thousand two hundred, so life was decoration. Twelve gives a
        // guided reach of 6240 -- the class reach plus the pursuit allowance and
        // nothing spare. A shot at maximum range must now be guided for nine
        // unbroken seconds, which is exactly when its pilot can least afford to be
        // pointing straight at somebody. That is a better price for a long shot
        // than any damage curve.
        /* seeker arc */ 5.00f, 12.0f, 0.30f,
        /* seeker     */ 0.42f, 2.0f, 0.9f, /* saam */ true,
        /* tactic     */ TACTIC_STANDOFF,
        // THE CIRCLE, AND IT IS THE WHOLE CLASS.
        //
        // lock_hold_cos was -2.0f, the sentinel meaning "hold it while they are
        // anywhere ON SCREEN" -- about 31 degrees to the edge of the viewport and
        // 41 to the corner. That is not an aiming mechanism, it is the absence of
        // one, and it is why a BALLISTA never had to aim hard.
        //
        // 0.95 acquire and 0.94 hold is a circle of about eighteen degrees: keep
        // them inside it and the round arrives, let them leave it and the lock is
        // gone. Hold is a shade wider than acquire so a lock that has been earned
        // does not flicker on the boundary -- it is not more forgiving, it is the
        // same circle with hysteresis.
        // 0.981 IS THE DRAWN RING, exactly. FOCAL * tan(acos(0.981)) is 79 px on
        // the 480 px panel, which is the circle the pilot sees -- there is no
        // margin outside it and no lock to be had by clipping the edge. 0.978 to
        // hold is three thousandths of hysteresis, enough that a target sitting on
        // the line does not strobe the lock and not enough to be a second chance.
        // TIGHTER AGAIN, reported as still guiding too generously. 0.990 is 57 px
        // on the panel where 0.981 was 79 -- about eight degrees. The ring is
        // drawn at exactly this, so what narrowed is the circle itself and not the
        // gap between the circle and the rule.
        /* weapon     */ WPN_SAAM,
        /* fire ctrl  */ 0.990f, 0.988f, 4200.0f, 0.0f, 3, 1.60f, 9.0f,
    },
};

// HOW FAR A ROUND CAN ACTUALLY GET, for the reach test below.
//
// speed * life is the answer for a round that flies at one speed, and it is WRONG
// for one that accelerates -- which is both of the interesting classes. It
// understated a BALLISTA round by a factor of three, and that mattered the moment
// the design called for a SHORT life: the assert refused a life the round could
// comfortably fly, because it was pricing it at the speed it leaves the rail with
// rather than the speed it spends most of the flight at.
//
// The ramp first, then whatever is left of the life at the ceiling.
static constexpr float vg_msl_reach(const ShipSpec& s) {
    return (s.msl_accel <= 0.0f)
         ? s.msl_speed * s.msl_life
         : ((s.msl_speed_max - s.msl_speed) / s.msl_accel >= s.msl_life
            ? s.msl_speed * s.msl_life
              + 0.5f * s.msl_accel * s.msl_life * s.msl_life
            : s.msl_speed * ((s.msl_speed_max - s.msl_speed) / s.msl_accel)
              + 0.5f * s.msl_accel * ((s.msl_speed_max - s.msl_speed) / s.msl_accel)
                     * ((s.msl_speed_max - s.msl_speed) / s.msl_accel)
              + (s.msl_life - (s.msl_speed_max - s.msl_speed) / s.msl_accel)
                * s.msl_speed_max);
}

float vg_lock_cos_at(const ShipSpec* sp, float range, bool hold) {
    const float base = hold ? sp->lock_hold_cos : sp->lock_cos;
    if (!sp || base < -1.0f) return base;          // the viewport idiom, untouched
    if (range <= LOCK_TIGHTEN_REF) return base;

    // The lateral tolerance the class has at the reference range, held constant
    // from there out. tan/atan rather than anything cleverer because this is
    // called a handful of times a frame and it is the honest geometry.
    const float th = acosf(base < -1.0f ? 0.0f : (base > 1.0f ? 1.0f : base));
    const float w  = LOCK_TIGHTEN_REF * tanf(th);
    return cosf(atanf(w / range));
}

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
//
// AND THE TWO WEAPON-SYSTEM TESTS, which are a different kind of check: not that a
// number is sane, but that the class's DECLARED mechanic and the round it actually
// carries are the same claim. They are written as equivalences rather than
// implications on purpose -- a SAAM system without a semi-active round is a circle
// with nothing to guide, and a semi-active round in a class that does not declare
// SAAM is a round nobody can steer. Both are faults, so both sides are tested.
//
// This is the mechanism that was missing. The guide circle drifted onto AEGIS and
// CHARIOT because the instrument read a tuning value and there was nothing anywhere
// that could tell it had happened. Tuning a cone can no longer hand a class
// somebody else's fantasy: it has to be declared, and declaring it wrongly does not
// build.
#define SHIP_INVARIANTS(C)                                                            static_assert(vg_ship_class[C].msl_splash > 0.0f,  #C " splash is a divisor");     static_assert(vg_ship_class[C].msl_speed  > 0.0f,  #C " speed is a divisor");      static_assert(vg_ship_class[C].magazine   > 0,     #C " magazine is a divisor");     static_assert(vg_ship_class[C].reload     > 0.0f,  #C " reload is a divisor");     static_assert(vg_ship_class[C].speed_max  > vg_ship_class[C].speed_min,                          #C " speed span is a divisor");                                      static_assert(vg_msl_reach(vg_ship_class[C]) > vg_ship_class[C].lock_range * 1.4f,                #C " cannot reach its own lock range");                                                                                    static_assert((vg_ship_class[C].wpn == WPN_SAAM) == vg_ship_class[C].msl_saam,                    #C " a SAAM system and a semi-active round are the same claim");     static_assert((vg_ship_class[C].wpn == WPN_SLAAM) == (vg_ship_class[C].msl_stack_time > 0.0f),    #C " an SL-AAM system and a banking lock are the same claim")

#if !defined(_MSC_VER)
SHIP_INVARIANTS(SHIP_AEGIS);
SHIP_INVARIANTS(SHIP_LANCE);
SHIP_INVARIANTS(SHIP_CHARIOT);
SHIP_INVARIANTS(SHIP_BALLISTA);
#endif

#undef SHIP_INVARIANTS

