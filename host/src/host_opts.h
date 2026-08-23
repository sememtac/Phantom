// Things the player can set on the command line.
//
// Kept in their own header because they cross the seam: host_main parses them,
// and the port reads them while it is dressing the mouse up as a finger.
#pragma once
// LOGICAL PIXELS OF STICK PER RAW MOUSE COUNT.
//
// The steering thumb moves this far for every count the mouse reports, and the
// game asks for STEER_RANGE -- 115 px -- to reach full deflection. So full
// deflection costs (115 / sens) counts of hand movement.
//
// RAW COUNTS, WHICH IS THE POINT. A count is what the device reported, before
// Windows applies pointer speed and "enhance pointer precision". That last one is
// acceleration: it moves the pointer further for a fast hand than a slow one over
// the same distance of desk, so a flick would reach full deflection where a
// deliberate push of the same length did not. A flight control cannot be
// calibrated against that, because there is no single number to calibrate.
//
// At 0.10 full deflection wants about 1150 counts, which is roughly 3 cm of hand
// at 1000 DPI. That is a deliberate movement rather than a flick, and it leaves
// room to be precise near the centre where the 8 px deadzone lives.
//
// It was 1.0, then 0.15, then 0.10, and each step down was measured against the
// same complaint rather than guessed at.
//
// READING THE POINTER'S POSITION INSTEAD WAS TRIED, on the reasoning that an
// absolute mapping cannot be accelerated. The mapping cannot; the hand that
// drives it still is. And bounding the stick by the window bounds the HAND by the
// window, which is a flick's worth of desk at any size that fits on one -- so the
// travel this number buys had nowhere to live. The pointer is placed on the thumb
// each frame now and shows where the stick is; it does not decide.
//
// Mouse DPI varies by a factor of ten between devices, so no single number is
// right for everybody. --sens overrides it.
extern float g_host_stick_sens;
