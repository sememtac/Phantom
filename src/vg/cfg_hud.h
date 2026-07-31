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
