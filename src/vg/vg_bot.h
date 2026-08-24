#pragma once
#include "vg_vec.h"
#include <stdint.h>

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
#define VG_OBS_N 41

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
    // WHICH WAY IS BACK INSIDE, as a unit bearing in view space.
    //
    // OBS_WALL says a wall is coming. It does not say where to go, and without
    // that neither a rule nor a network can turn away from one -- the scripted
    // policy "avoided" the boundary by flying straight ahead more slowly, which
    // is not avoidance, and the board flew into the wall over and over.
    //
    // The enemies never had this problem: vg_ai.cpp asks the arena for an inward
    // normal and steers down it. This is that same answer, moved into the
    // observation so that whatever is flying can use it.
    OBS_WALL_X,
    OBS_WALL_Y,
    OBS_WALL_Z,

    // ---- WHAT THIS SHIP IS, AND WHY IT IS IN AN OBSERVATION AT ALL ----------
    //
    // Everything above is the situation. These eleven are the AIRFRAME, and they
    // do not change during a fight. A network does not need to be told a
    // constant, so putting them here looks like waste. It is not, and it fixes
    // two separate problems that would otherwise both be dead ends.
    //
    // ONE POLICY CANNOT FLY FOUR SHIPS WITHOUT THEM. A network fitted to BALLISTA
    // recordings has no way to know that a CHARIOT turns half again as fast,
    // reaches a third as far and empties its rack in two seconds. It flies all
    // four like the one it saw, badly. Measured on the board: the attract demo
    // had to be pinned to BALLISTA to hide it.
    //
    // AND THE TABLE IS GOING TO KEEP MOVING. Hulls have already tripled once this
    // year and the classes are still being tuned. A policy that memorised one
    // table is wrong the moment the table changes, which is a retraining
    // treadmill nobody will keep up with. A policy that is TOLD the numbers can
    // be trained across a spread of them and generalise to values it never saw,
    // so a tune becomes something it already expects.
    //
    // Each one is divided by a fixed reference rather than by the biggest value
    // in the table. A reference is a constant; the biggest value in the table is
    // itself a tuning knob, and dividing by it would silently rescale every other
    // class whenever one of them changed.
    OBS_SHIP_TURN,        // turn rate
    OBS_SHIP_AGI_SLOW,    // extra turn at idle
    OBS_SHIP_AGI_FAST,    // turn lost at full throttle
    OBS_SHIP_SPEED,       // top speed
    OBS_SHIP_HULL,        // hull points at full
    OBS_SHIP_LOCKRANGE,   // how far it can shoot
    OBS_SHIP_LOCKTIME,    // how long the nose must hold
    OBS_SHIP_MAG,         // rounds per rack
    OBS_SHIP_GAP,         // seconds between two rounds
    OBS_SHIP_RELOAD,      // seconds to refill the rack
    OBS_SHIP_MSLSPEED,    // how fast the round flies
};

// The references the eleven airframe fields are divided by. Fixed numbers, not
// the largest value in the table: see the note above.
#define OBSREF_TURN       2.5f
#define OBSREF_SPEED      500.0f
#define OBSREF_HULL       350.0f
#define OBSREF_LOCKRANGE  4200.0f
#define OBSREF_LOCKTIME   1.0f
#define OBSREF_MAG        12.0f
#define OBSREF_GAP        2.0f
#define OBSREF_RELOAD     10.0f
#define OBSREF_MSLSPEED   550.0f

// What OBS_TGT_RANGE_W is divided by. Not a tuning value -- it is the scale that
// turns the field back into world units, and anything reading the observation has
// to agree with it.
#define BOT_RANGE_REF 2000.0f

// How near the boundary the seat stops fighting and turns inward, as a fraction
// of OBS_WALL. Near the whole margin on purpose: the enemies turn at the full
// margin, and hitting the wall is fatal at any speed.
#define BOT_WALL_TURN 0.85f

// Fill an observation from the PLAYER's seat. The one place that reads the world.
void vg_bot_observe(VgObs* o);

// ...and from an ENEMY's, which is the same question asked from the other side.
//
// THE POINT OF THE WHOLE MODULE. The observation was built to be seat-agnostic --
// every field is a fact a pilot has, expressed in that pilot's own frame -- so a
// policy fitted in one seat means something in the other. This is where that
// claim is paid for.
//
// The work is entirely the CHANGE OF FRAME. The player is the origin and the
// world turns around them; an enemy is a position and a heading inside that
// world. So every bearing here is rotated into the enemy's own axes, where +z is
// its nose, which is exactly what +z means to the player.
void vg_bot_observe_enemy(int index, VgObs* o);

// Whether opponents are flown by the network rather than by vg_ai.cpp.
//
// Off by default. The hand-written AI is the game; this is the experiment, and
// it takes the wheel only when asked.
extern bool vg_enemy_net;

// Turn a policy's control into an enemy's steering. `desired` comes back as a
// direction to turn toward, in view space, and target_speed as a speed. Returns
// false when the network declined, and then the class tactic flies as usual.
bool vg_bot_fly_enemy(int index, const struct Ship* s, Vec3* desired,
                      float* target_speed, float dt);

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

// WHICH PILOT FLIES IT: the trained network, or the hand-written policy.
//
// On by default where a network is compiled in, because the network is the
// point. The desktop's --scripted turns it off, which is how the two are
// compared in the same fight.
//
// THE NETWORK DOES NOT FLY ALL OF IT, and the split is deliberate. It steers
// and sets the throttle. It does NOT decide about the wall or pull the trigger.
// The wall is fatal and the recorded pilot never hit one, so there is nothing in
// the data to learn it from -- a policy trained only on flights that went well
// has never seen the one mistake it cannot survive. The trigger is a rule for
// the same kind of reason: a press is a third of a per cent of the rows.
extern bool vg_bot_net;

// Microseconds the network's forward pass took on the last frame it ran. Zero
// when the scripted policy is flying. Read by the telemetry.
extern uint32_t vg_bot_net_us;

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
