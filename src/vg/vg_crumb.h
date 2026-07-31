#pragma once
#include <stdint.h>

// ===========================================================================
// BREADCRUMBS
//
// A crash on this part is silent. The panic handler writes to a console that
// dies with the task holding the USB peripheral, so a fault, a watchdog and a
// hang are indistinguishable from the host: the port simply stops.
// esp_reset_reason() tells us WHICH of those it was, and nothing about where.
//
// This is the where. Two bytes and a counter in RTC memory, which is not
// cleared by a reset -- only by losing power -- so whatever the frame was doing
// when it died is still readable at the next boot.
//
// The cost is three stores a frame. That is deliberate: a crash that only
// happens in real play, minutes in, cannot be caught by instrumentation anyone
// would think twice about leaving on.
// ===========================================================================

enum VgCrumb : uint8_t {
    CRUMB_BOOT = 0,
    CRUMB_POLL,      // capture/replay command poll
    CRUMB_INPUT,     // touch, IMU, replay record
    CRUMB_UPDATE,    // vg_game_update -- the simulation
    CRUMB_RENDER,    // vg_render_frame -- building the primitive list
    CRUMB_FLUSH,     // vg_rast_flush -- rasterising and the panel DMA
    CRUMB_SLOTS
};

// Where the frame is now, and which game state it is in. Called four times a
// frame; keep it cheap.
void vg_crumb(uint8_t where, uint8_t state);

// A frame that took far longer than a frame should. A freeze is NOT a crash --
// nothing resets, so the reset reason and the breadcrumb both stay silent about
// it, and to the player the two are indistinguishable. This is the only thing
// that would catch one.
void vg_crumb_stall(uint32_t ms, uint8_t state, uint8_t where);

// Forget everything: no crash, no stall. For after a report has been collected,
// so the next one is not read against a stale worst-case that nothing can beat.
void vg_crumb_reset(void);

// Print the previous run's last position, at boot. Says nothing useful after a
// clean power-on, which is exactly when there is nothing to say.
void vg_crumb_report(void);
