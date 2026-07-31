#pragma once
#include "cfg_display.h"

// ===========================================================================
// HUD layout and touch zones.
// ===========================================================================

// --- strokes ---------------------------------------------------------------
// Panel frame thickness. AmberConsole specifies 2px, but a 2px bar carried
// through the spherical warp becomes a thin diagonal quad, and the rasteriser
// can land it on a single pixel row -- or drop it entirely -- where the bend is
// steepest. 3px survives that.
#define HUD_STROKE           3

// --- radar -----------------------------------------------------------------
// A half-ellipse dome across the bottom: a circular plan view seen in steep
// perspective. Forward is the top of the dome, the flat chord is the player's
// own 3-9 line. Fighters and incoming missiles only -- asteroids are terrain.
#define RADAR_CX             240.0f
#define RADAR_CY             452.0f
#define RADAR_RX             152.0f
#define RADAR_RY             78.0f
#define RADAR_RANGE          1300.0f  // world units at the rim

// --- throttle --------------------------------------------------------------
// A vertical strip on the LEFT edge, under the left thumb. Contacts that BEGIN
// inside the zone own the throttle; everything to the right steers and fires.
#define THROTTLE_X0          14
#define THROTTLE_W           58
#define THROTTLE_TOP         100
#define THROTTLE_BOT         (SCR_H - 100)

// The touch zone is deliberately wider than the drawn slider so a thumb does not
// have to be accurate to grab it.
#define THROTTLE_ZONE_X1     118

// To ACQUIRE the throttle a contact must land near the slider vertically too.
// The x-zone alone spans the full screen height, so a stray contact anywhere
// down the left edge -- including the bottom corner, which raw (0,0) maps to --
// used to read as "thumb slammed to idle" and silently zeroed the throttle.
// Generous enough (60 px past each end of the drawn control) to still slam.
#define THROTTLE_ZONE_Y0     40
#define THROTTLE_ZONE_Y1     440

// Once a contact owns the throttle it may drag anywhere in the x-zone, but a
// real thumb cannot teleport: at ~65 fps it moves tens of pixels per frame, not
// hundreds. A candidate further than this from its last position is a different
// (or spurious) contact and must not inherit the control.
#define THROTTLE_MAX_JUMP    160.0f

// --- fire button -----------------------------------------------------------
// Missiles launch from a hardware button, not a screen tap. The left thumb owns
// the throttle and the right finger is steering, which leaves the left INDEX
// finger free on the top-edge buttons -- so firing no longer interrupts either
// control.
//
// BOOT (GPIO 0) only. It shipped accepting both GPIOs because the physical
// left/right arrangement was unknown; GPIO 18 turned out to be the +/- key.
//   0x01 = BOOT   0x02 = +/-   0x03 = either
#define FIRE_BUTTON_MASK     0x01
// The other one. Menus only -- currently cycles the ship class on the title card.
#define ALT_BUTTON_MASK      0x02

// --- gestures --------------------------------------------------------------
// A contact that lifts quickly without travelling counts as a tap, and fires.

// --- caution annunciators --------------------------------------------------
// Both alerts flash the same way, so the cadence is defined once, here, rather
// than twice in cfg_world.h and cfg_combat.h. Those keep the DISTANCES, which
// really are a property of the world and of combat; this is a property of the
// panel.
//
// The rate is the range: slow when the thing first matters, fast when it is about
// to happen, and floored.
//
// The floor is 0.5s, which is 2 Hz, and it has been raised three times to get
// there. Aviation human-factors standards put attention-getting flash rates at
// roughly 3-5 Hz, so 0.35s (2.9 Hz) was inside that band -- but those figures are
// for a small lamp on a panel, and photosensitivity guidance caps LARGE flashing
// areas at about 3 Hz. This alert is a filled block on a screen held at arm's
// length, which is the large-area case, so the lower limit is the one that
// applies. A real cockpit also puts the urgency in an aural alert rather than in
// the flash rate, and this game has no sound yet.
//
// 1 Hz at the far edge matches the convention for a caution; 2 Hz at the near
// edge is a warning without being a strobe.
#define ALERT_FLASH_SLOW     1.00f    // seconds per flash at the far edge, 1 Hz
#define ALERT_FLASH_FAST     0.50f    // ...and closest, 2 Hz. Never faster.
#define ALERT_FLASH_DUTY     0.50f    // fraction of the period the block is lit

// --- rear view -------------------------------------------------------------
// A patch in the top right, which is the one part of the panel with nothing in
// it: the throttle owns the left edge, the radar the bottom, and the comms and
// broadcast strips run across the middle.
//
// It shows the same field of view as the main window, at a quarter of the size.
// That is deliberate rather than convenient -- hold the patch and its picture
// fills the screen, and a patch with its own wider field would appear to zoom in
// at the moment the player is trying to judge a closing shot.
#define REAR_W               145
#define REAR_H               44
// Clear of the missile rack, which hud_panel puts at x=440, y=140. The square
// patch this replaced ran to x=442 and y=164 and sat on the corner of it.
#define REAR_X               (SCR_W - SCR_SAFE - REAR_W)
// Below the speed readout rather than beside it. SPD is centred and its label
// block ends at y=55, so at y=48 the mirror sat level with it and the two read
// as one crowded row even though they never touched.
#define REAR_Y               60
#define REAR_CX              (REAR_X + REAR_W * 0.5f)
#define REAR_CY              (REAR_Y + REAR_H * 0.5f)
// Same ANGULAR scale as the main window, set by the width. So the picture does
// not change size when the patch is held and fills the screen -- it is the same
// view, uncropped. Vertically that makes the patch a letterbox on it, roughly 20
// degrees against the main window's 61, which is what a mirror is: a wide, short
// band of what is behind you.
#define REAR_FOCAL_K         ((float)REAR_W / (float)SCR_W)

// Touch zone, generous around the drawn patch: it is a button held under a
// thumb, not a control that needs accuracy. A contact here does NOT steer --
// see the partition in vg_input.cpp. Kept off the missile rack.
#define REAR_ZONE_X0         (REAR_X - 24)
#define REAR_ZONE_Y0         (REAR_Y - 30)
#define REAR_ZONE_X1         (REAR_X + REAR_W + 24)
#define REAR_ZONE_Y1         (REAR_Y + REAR_H + 30)
