#pragma once

// ===========================================================================
// Flight model, controls and hull integrity.
// ===========================================================================

// --- speed -----------------------------------------------------------------
// The central tactical trade. Top speed outruns a missile (340) outright, but
// nothing useful can be done up there: the turn rate collapses and a lock takes
// far longer to acquire than the geometry will hold. Fast is how you SURVIVE
// and disengage; slow is the only way to FIGHT.
//
// THESE ARE REFERENCE VALUES ONLY -- they are AEGIS's numbers, kept here for
// input calibration and for anything that needs a scale before a ship is known.
// The values the simulation actually uses come from ShipSpec (vg_ship.h), so a
// CHARIOT tops out at 460 and a BALLISTA at 340.

// Deliberately sluggish. Committing to speed -- in either direction -- has to
// take a couple of seconds, or the choice between fighting and running carries
// no risk and gets made instantly and constantly.
#define THROTTLE_LERP        2.0f     // throttle response (1/sec)
// The HUD warp tracks the same command far faster, so the canopy answers the
// slider immediately even though the ship does not.
#define THROTTLE_VIS_LERP    9.0f

#define TURN_RATE            1.9f     // rad/sec at full stick deflection
#define BANK_MAX             0.45f    // cosmetic roll at full yaw, radians
#define BANK_LERP            5.0f

// Turn rate scales inversely with speed. This is the whole reason the throttle
// exists as a combat control: slow down to out-turn a missile, speed up to
// outrun it.

// --- steering --------------------------------------------------------------
// 0 = virtual joystick. Press anywhere right of the throttle zone to set an
//     origin, then hold the finger displaced from it; the ship keeps turning
//     while held and self-centres on release. Default, because sustained turns
//     are the point of a flight sim and it does not care how you hold the board.
// 1 = trackball. Rotation tracks raw finger movement, so holding still stops the
//     turn and you have to keep swiping.
// 2 = tilt (the original accelerometer scheme, kept for comparison).
#define STEER_MODE           0

#define STEER_RANGE          115.0f  // px of displacement for full deflection
#define STEER_DEADZONE       8.0f    // px
#define STEER_LERP           16.0f   // input smoothing (1/sec)
// Past full deflection, drag the origin along with the finger. Without this a
// long swipe strands the origin off-screen and the control is lost entirely.
#define STEER_RECENTER       1
#define TRACKBALL_RAD_PER_PX 0.0045f // STEER_MODE 1 only

// Pointer-style pitch: the nose follows the finger, so dragging toward the top
// of the screen aims up. Flip to -1.0f for inverted / classic-stick feel.
#define STEER_PITCH_SIGN     (1.0f)

// --- tilt (STEER_MODE 2 only) ---------------------------------------------
#define TILT_DEADZONE        0.06f    // g, ignored around the calibrated neutral
#define TILT_FULL            0.45f    // g of tilt for full deflection
#define TILT_LERP            12.0f    // input smoothing (1/sec)
#define TILT_SWAP_AXES       0        // 1 = yaw from ay, pitch from ax
#define TILT_YAW_SIGN        (1.0f)   // flip if left/right is reversed
#define TILT_PITCH_SIGN      (1.0f)   // flip if up/down is reversed

// --- hull ------------------------------------------------------------------
// An absolute pool of points, per ship class (see vg_ship.h), not a fraction --
// so an AEGIS and a CHARIOT can differ. AEGIS carries 110.
//
// There is NO self-repair. Damage is permanent for an entire tournament and the
// only way to undo it is to pay credits for it between rounds, which is what
// makes the economy the difficulty curve rather than a side system. Removing
// regen also closed the stalemate case: every hit now counts for good, so a
// match strictly progresses toward a conclusion and needs no clock.
#define HEALTH_LOW           0.30f    // meter blinks below this FRACTION of max

// Missile damage is not here -- it comes from the launching ship's warhead and
// scales with how close the fuse went off (ShipSpec::msl_damage / graze_floor).
//
// COLLISION IS ALWAYS FATAL, so there is no collision damage number any more.
// See vg_kill_player() in vg_game.cpp -- one rule for the wall, an asteroid and
// another ship, on any hull, at any speed.
//
// There used to be three numbers here: 26 for an asteroid, 42 for a ram, fatal
// for the wall. Touching a rock was therefore a setback on a full hull and death
// on a low one, and the player had to learn which contacts they could afford.
// Now there is nothing to learn.
//
// This is what makes the boundary a real edge and gives a high-speed escape a
// cost, because you are least able to turn exactly when you cover ground
// fastest. It is also what makes a suicide run a real threat instead of a trade
// the enemy loses.

#define SHIP_RADIUS          9.0f

// --- airframe buzz ---------------------------------------------------------
// A fine, continuous vibration at the top of the throttle. Speed is otherwise
// only a number and a mote streak: nothing about the view tells you the ship is
// working. Deliberately small -- this has to read as strain at the edge of the
// envelope, not as damage, and anything large enough to notice consciously is
// large enough to make the reticle useless.
//
// Applied to the CAMERA, so the world shakes and the panel-mounted instruments
// do not, which is the right way round for something bolted to the airframe.
#define SPEED_SHAKE_AT       0.80f    // throttle at which it starts
#define SPEED_SHAKE_MAX      2.7f     // px at full

// The instruments feel it too. Smaller than the airframe shake and running on
// its own clock, so the panel is clearly not rigidly bolted to the view -- what
// sells it is the two disagreeing. A HUD that shook in lockstep with the world
// would just look like one bigger shake.
#define HUD_SHAKE_MAX        1.7f     // px at full throttle

// --- trails ----------------------------------------------------------------
// Every fighter drags a coloured ribbon of where it has been. This is what makes
// trail colour a mechanic rather than decoration: at range a contact is four
// pixels of wireframe, but its trail is a long stroke in a hue you can name --
// so colour becomes IFF, which is the one job hue is allowed to do here.
//
// Length is driven by THROTTLE, the way a real contrail thickens when an engine
// is working. Each sample stores the power setting it was laid down at, and a
// segment is skipped once age has faded it below the ink floor -- so a ship at
// idle leaves a stub, a ship at full throttle streams a long ribbon, and a burst
// of speed stays visible in the tail after the ship has already slowed.
//
// That also bounds the cost: the buffer is long, but only the segments bright
// enough to see are ever projected, and the band rasteriser only stays free
// while it hides under the DMA blit.
// Length matters more than it looks. Your OWN trail streams straight back along
// -z, so it is behind the camera and invisible -- turning only swings it
// sideways, to z near zero, where it is still clipped. A point laid down t
// seconds ago only comes round in FRONT of you once you have turned more than
// half a circle since laying it, i.e. once turn_rate * t exceeds pi. At 1.9
// rad/s a 1.8s tail reached 3.4 rad and barely grazed that, which is why the
// player's ribbon never appeared. 4.5s reaches 8.5 rad -- more than a full
// revolution -- so a hard turn now wraps the trail right across the canopy.
//
// Segments behind the near plane reject on a single z compare, so the extra
// length is nearly free until it is actually on screen.
#define SHIP_TRAIL           140      // sample points per ship
#define SHIP_TRAIL_DT        0.055f   // seconds between samples (~7.7s of tail)

// A contrail is CONTINUOUS. Throttle sets how brightly it burns, never whether
// it exists -- an engine at idle still leaves a thin line. Segments used to be
// dropped once age and power faded them below a floor, and because power varies
// along the ribbon that punched holes in the middle of it wherever the pilot had
// eased off. So the floor is now a brightness clamp, not a skip.
#define SHIP_TRAIL_IDLE      0.30f    // emission strength at zero throttle
#define SHIP_TRAIL_MIN       0.09f    // dimmest a segment may get -- never zero

// --- roll ------------------------------------------------------------------
// Radians per second at full stick, with the + key held. Deliberately brisk:
// roll is an expert control that nobody is forced to touch, and a timid one
// would be worse than none -- the whole point is to break the plane of a turn
// faster than an opponent can read it.
#define ROLL_RATE            2.9f
