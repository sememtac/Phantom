#pragma once
#include "vg_vec.h"

struct VgInput;

// ===========================================================================
// THE PLAYER'S SEAT, FLOWN BY SOMETHING THAT IS NOT A PLAYER.
//
// WHY THIS EXISTS AND WHY IT IS SHAPED LIKE THIS.
//
// The enemy AI in vg_ai.cpp cannot be pointed at the player's seat, and the
// reason is structural rather than lazy: the player IS THE ORIGIN. Every enemy
// is a position in the player's view space, the arena moves around the player,
// and an enemy decides by writing a direction into its own `fwd`. The player has
// no `fwd` to write -- they have a stick, and the world turns underneath them.
//
// So flying the player's seat means producing a VgInput: pitch, yaw, throttle,
// a trigger. That is a different job from vg_ai.cpp's, and it is also EXACTLY
// THE JOB A NETWORK WOULD DO -- which is why the seam is here rather than
// somewhere convenient.
//
// TWO HALVES ON PURPOSE, and the split is the whole point of the module:
//
//   vg_bot_observe  turns the world into a fixed-size vector of numbers
//   vg_bot_act      turns that vector into a control input
//
// Nothing in `act` may read the world. It gets the observation and nothing else.
// That constraint costs a little convenience now and buys the only thing that
// matters later: a policy that is a pure function of an observation can be
// replaced by a trained one WITHOUT touching the game, and can be trained
// off-device against recorded observations rather than against a running game.
//
// It is also what makes the two seats comparable. The same observation can be
// filled from either side of a fight, so a policy learned in one seat is
// meaningful in the other.
// ===========================================================================

// HOW MANY NUMBERS A PILOT SEES. Fixed, and it is a contract: change it and
// every recorded observation and every trained policy that read the old layout
// are wrong, silently, because they are all just arrays of floats.
//
// Kept small deliberately. Everything here is something a pilot could actually
// know from the canopy and the panel -- there is no privileged state, no reading
// the opponent's intentions, and nothing that would let a policy learn to cheat
// in a way a player could not copy.
#define VG_OBS_N 27

// Roughly -1..1, all of it, because that is what a network wants and because a
// human reading a dump of one should be able to see at a glance which numbers
// are extreme. Ranges that have no natural ceiling are divided by the thing that
// gives them meaning -- range by the ship's own lock range, speed by its own
// envelope -- so the vector says the same thing whichever class is flying.
struct VgObs {
    float v[VG_OBS_N];
    bool  has_target;     // false when there is nothing alive to fight
};

// The indices, named. A bare float array is what the policy wants and a
// nightmare for everybody else, so this is the one place the layout is written
// down. Anything appended goes at the END, before VG_OBS_N is raised.
enum {
    OBS_TGT_X = 0,     // unit bearing to the target, in view space
    OBS_TGT_Y,
    OBS_TGT_Z,
    OBS_TGT_RANGE,     // / this ship's lock range, clamped at 1
    OBS_TGT_CLOSURE,   // closing rate / combined top speed. + is closing
    OBS_TGT_ASPECT,    // how far the target is pointed AT us, cos
    OBS_TGT_OFFBORE,   // ...and how far we are pointed at them, cos
    OBS_TGT_HULL,      // their remaining hull, 0..1
    OBS_TGT_VX,        // their velocity in view space / combined top speed
    OBS_TGT_VY,
    OBS_TGT_VZ,
    OBS_OWN_HULL,      // 0..1
    OBS_OWN_SPEED,     // where in its own envelope, 0..1
    OBS_OWN_THROTTLE,  // what was ASKED for, which is not the same thing
    OBS_OWN_ROUNDS,    // fraction of the rack left
    OBS_OWN_RELOADING, // 1 while dry
    OBS_OWN_LOCK,      // lock progress 0..1, 1 when locked
    OBS_MSL_IN,        // 1 if something is tracking us
    OBS_MSL_X,         // ...its bearing, view space
    OBS_MSL_Y,
    OBS_MSL_Z,
    OBS_MSL_RANGE,     // / the range at which it becomes worth answering
    OBS_WALL,          // clearance to the boundary, 0 at the wall, 1 far from it
    OBS_ROLL,          // own roll angle / pi -- the horizon, which a pilot can see
    // RANGE IN WORLD UNITS, over BOT_RANGE_REF. OBS_TGT_RANGE is divided by the
    // ship's OWN lock range, which is right for "am I in my envelope" and wrong
    // for everything physical: 10% of an AEGIS lock is 160 units and 10% of a
    // BALLISTA's is 420, so a threshold expressed in it means a different
    // distance on every hull. Collisions, break-off and the width of a pass are
    // all absolute, and this is the number they are judged with.
    OBS_TGT_RANGE_W,
    // DOES MY WEAPON NEED ME TO KEEP LOOKING AT THEM. 1 for a semi-active class,
    // 0 for fire-and-forget.
    //
    // The first fact about the SHIP rather than the situation to enter the
    // observation, and it is here because a policy that does not know this
    // cannot fly a BALLISTA at all: it fires, manoeuvres, and every round it has
    // ever launched goes ballistic behind it. Measured -- a thousand simulated
    // seconds of scripted BALLISTA did exactly zero damage.
    //
    // It is also the first step of the thing that makes a trained policy survive
    // tuning: feed it the ship's own numbers and it can generalise across them,
    // instead of memorising one table and being wrong the moment the table moves.
    OBS_OWN_SAAM,
    // HOW MANY OF MY OWN ROUNDS ARE STILL FLYING, over the rack size.
    //
    // A pilot knows this -- they fired them, and for a semi-active class it is the
    // single most important thing on the panel: it is the difference between "I
    // may manoeuvre freely" and "manoeuvring now throws away everything I have
    // spent". Without it a policy cannot tell those two situations apart, and the
    // scripted one could not: it evaded every incoming round, correctly for a
    // dogfighter and fatally for a sniper, and killed its own shots every time.
    OBS_OWN_INFLIGHT,
};

// What OBS_TGT_RANGE_W is divided by. Not a tuning value -- it is the scale that
// turns the field back into world units, and anything reading the observation has
// to agree with it.
#define BOT_RANGE_REF 2000.0f

// Fill an observation from the PLAYER's seat. The one place that reads the world.
void vg_bot_observe(VgObs* o);

// ...and decide. Reads nothing but the observation.
//
// SCRIPTED FOR NOW, and it is written to be a fair opponent rather than a good
// one: it does what the hand-written enemy tactics do, from the other side of
// the origin. It exists to make a fight happen with nobody holding the board, so
// that there is something for a policy to be trained against and something for
// it to be measured against once there is one.
void vg_bot_act(const VgObs* o, VgInput* in, float dt);

// Whether the seat is being flown by the bot at all. Off unless something turns
// it on -- the desktop's --bot flag today.
extern bool vg_bot_on;

// Back to a clean sheet: called when a match starts, so the bot's own smoothing
// and commitment timers do not carry across from the last one.
void vg_bot_reset(void);

// ===========================================================================
// THE TAP: WHAT THE SEAT SAW, AND WHAT IT DID ABOUT IT.
//
// Called once a frame while a ship is being flown, with the observation and the
// control that was actually used -- WHOEVER produced it. That last part is the
// whole value: the same hook records the scripted policy, a trained one, and a
// HUMAN. Training a network to fly like the author needs the author's own
// flying paired with what they could see, and this is the only place both exist
// at once.
//
// A FUNCTION POINTER, NULL BY DEFAULT, because writing files is not something
// src/vg does -- the board has no business with a dataset, and one null check a
// frame is what that costs. The desktop points it at a writer.
typedef void (*VgBotTap)(const VgObs* o, const VgInput* in);
extern VgBotTap vg_bot_tap;
