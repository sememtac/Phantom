#pragma once

// ===========================================================================
// Flight model, controls and hull integrity.
// ===========================================================================

// --- speed -----------------------------------------------------------------
// The central tactical trade. Top speed outruns a missile (200) outright, but
// nothing useful can be done up there: the turn rate collapses and a lock takes
// far longer to acquire than the geometry will hold. Fast is how you SURVIVE
// and disengage; slow is the only way to FIGHT.
#define SPEED_MIN            100.0f   // world units/sec at zero throttle
#define SPEED_MAX            420.0f   // ...and at full throttle

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
#define AGILITY_SLOW_BONUS   0.75f    // extra turn-rate fraction at idle
#define AGILITY_FAST_MALUS   0.30f    // turn-rate fraction lost at full throttle

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
// A single meter, not discrete lives. It self-repairs after a spell out of
// combat, but ONLY while above the floor -- drop below and the damage is
// permanent, so a bad fight leaves you crippled for the rest of the run.
#define HEALTH_REGEN_DELAY   5.0f     // seconds clear of combat before repair
#define HEALTH_REGEN_RATE    0.075f   // fraction per second (~13s for a full bar)
#define HEALTH_REGEN_FLOOR   0.30f    // below this the hull will not self-repair
#define HEALTH_LOW           0.30f    // meter blinks below this
#define THREAT_COMBAT_RANGE  700.0f   // a tracking missile this close = in combat

#define DMG_MISSILE          0.34f
#define DMG_ASTEROID         0.26f
#define DMG_RAM              0.42f
// Fatal. Flying into the world boundary ends the run outright, which is what
// gives the high-speed escape option a real cost -- you are least able to turn
// exactly when you are covering ground fastest.
#define DMG_WALL             1.0f

#define SHIP_RADIUS          9.0f
