#pragma once
#include "generated/bezel_console.h"
#include "vg_console.h"   // the chassis, and the windows it leaves
#include "vg_config.h"

// ===========================================================================
// WHAT THE SCREENS IN THE MACHINE SHARE, and nothing else.
//
// This was vg_screens.h, and it held the layout of four unrelated screens --
// ship select, registration, the tournament map and repair -- in one header that
// every one of them included. It is the same fault as a screen's constant doing
// a machine's job, one level up: the tournament page cannot be laid out without
// opening a file the other three depend on, and a number moved for one of them
// rebuilds all four.
//
// A screen owns its own rectangles now, in its own header beside its own .cpp.
// What is left here is what belongs to no one screen: the two gestures, and the
// glass they are all laid out inside.
//
//   vg_ui       widgets that may not know what a screen IS
//   vg_bezel    the baked art and its slots
//   vg_console  the machine: chassis, headline, glass, keys
//   vg_menu     what the screens bolted into THIS drawing share
//   one .h and one .cpp per screen
//
// The layout still lives in a header rather than inside a .cpp, and for the
// original reason: two separate things have to agree on it exactly -- the
// drawing, and the hit tests the state machine runs against a tap. Keeping a
// screen's rectangles in one place is what stops a button drifting away from the
// thing that lights up. What changed is that "one place" is now one place PER
// SCREEN.
// ===========================================================================

#define MENU_TAP_SLOP   16.0f   // px of travel a contact may drift and still tap

// WHEEL_STEP went to vg_ui.h, with the wheel that spends it. A constant that says
// two things must stay the same is only half of the job; the other half is that
// there is one piece of code doing the stepping.

// THE GLASS, AS A RECTANGLE, for a screen to lay itself out against at compile
// time. vg_console_glass answers the same question at run time, for a caller
// that has a pointer rather than a preprocessor.
//
// THE SCREEN IS INSIDE A MACHINE NOW, and the machine decides where it ends.
// BEZEL_CONSOLE_APERTURE_* is the largest rectangle that fits inside the chassis
// art's screen hole, emitted by tools/bezel_bake.py from the drawing itself. The
// layout is derived from it rather than measured against it, so a redrawn
// chassis moves the menu instead of quietly cropping it.
//
// It cost 34px of height. The panel ran to y 396 and the aperture ends at 365,
// so the plan view used to be drawn onto the bottom bezel.
//
// NAMED FOR THE MACHINE, not for a screen. These were SEL_AP_*, and registration
// reached across for one of them to place its wheels -- which is exactly what
// SEL_GO_SLOT was doing before it became VG_CON_KEY. The glass belongs to the
// drawing, and three screens are laid out in it.
#define VG_GLASS_X0     (BEZEL_CONSOLE_S0_X0 + VG_GLASS_INSET_X)
#define VG_GLASS_Y0     (BEZEL_CONSOLE_S0_Y0 + VG_GLASS_INSET_Y)
#define VG_GLASS_X1     (BEZEL_CONSOLE_S0_X1 - VG_GLASS_INSET_X)
#define VG_GLASS_Y1     (BEZEL_CONSOLE_S0_Y1 - VG_GLASS_INSET_Y)

// The one button in the game. Drawn in the same idiom as the instrument panels:
// sunk into a dark well, framed, with corner ticks. A primary action is marked
// by a brighter frame and a bright key line under the label -- NOT by filling
// the whole rectangle, because a solid slab reads as a block rather than a
// control and throws away the frame that makes the rest of the interface look
// built.
// vg_button moved to vg_ui.h with the other shared widgets.
