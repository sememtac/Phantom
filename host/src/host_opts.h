// Things the player can set on the command line.
//
// Kept in their own header because they cross the seam: host_main parses them,
// and the port reads them while it is dressing the mouse up as a finger.
#pragma once

// LOGICAL PIXELS PER MOUSE COUNT.
//
// The steering finger moves this far for every count the mouse reports, and the
// game asks for STEER_RANGE -- 115 px -- to reach full deflection. So full
// deflection costs (115 / sens) counts of hand movement.
//
// This was 1.0 at first, on the reasoning that one mouse pixel to one panel
// pixel was parity with the glass. It is not. 115 px of finger travel is about
// 9 mm across a 2.16 inch panel; 115 counts of a 1000 DPI mouse is about 3 mm of
// hand. The same NUMBER, a third of the movement, and the ship snapped to full
// deflection on a twitch.
//
// At 0.10 full deflection wants about 1150 counts, which is roughly 3 cm of hand
// at 1000 DPI. That is a deliberate movement rather than a flick, and it leaves
// room to be precise near the centre where the 8 px deadzone lives.
//
// It was 1.0, then 0.15, and both were still too quick in the hand. Each step
// down was measured against the same complaint rather than guessed at, and the
// number is exposed precisely because the right one depends on a mouse this
// code cannot see.
//
// Mouse DPI varies by a factor of ten between devices, so no single number is
// right for everybody. --sens overrides it.
extern float g_host_mouse_sens;
