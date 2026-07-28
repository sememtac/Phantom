#pragma once

// ===========================================================================
// Credits.
//
// A persistent meta-currency that buys DURABILITY, not power. Credits survive
// death and carry into the next tournament, but they are spent down and they
// never make a ship better -- so the meta layer does not soften the game the way
// meta-progression usually does. A good run funds the next one; that is all.
//
// Winning pays more the deeper you get, so depth beats repetition. Repair is
// priced so that early on you genuinely cannot afford to top up: the first
// round's purse is worth about thirty hull points against a hundred-point ship.
// ===========================================================================

// Purse by round: R16, QF, SF, final.
#define CREDIT_R16           100
#define CREDIT_QF            175
#define CREDIT_SF            300
#define CREDIT_FINAL         600

// Condition bonus, as a fraction of the base, scaled by hull remaining. Flying
// well therefore pays twice -- you earn more AND need less repair -- but the
// flat base is large enough that one bad match does not spiral the run.
#define CREDIT_CONDITION_K   0.50f

// Flat, on every ship. No per-class adjustment is needed: a bigger pool costs
// proportionally more to restore simply because there is more of it, so AEGIS's
// 110 points cost 57% more to top up than CHARIOT's 70. Hull size is therefore
// economically neutral and a class's real advantage is its falloff profile.
#define CREDIT_PER_HULL      4

// Without a ceiling a patient player banks five thousand, full-repairs every
// round forever, and the whole system evaporates. About one strong run's
// earnings: you can always fund a good tournament and never twenty.
#define CREDIT_CAP           1500

// You start broke. The first few tournaments are meant to be the ones where you
// cannot afford to repair at all.
#define CREDIT_START         0
