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
    // Sub-phases of the flush, added when a watchdog reset said "died in
    // flush" and flush turned out to contain four different things that can
    // die. The state byte carries the band index for these, not the state.
    CRUMB_FWAIT,     // waiting on the previous DMA transfer
    CRUMB_FDRAW,     // drawing a band's primitives
    CRUMB_FSCAN,     // scanlines, tint, tv over a finished band
    CRUMB_FPUSH,     // queueing the band to the panel
    // AND THE TAIL, added for the same reason the flush sub-phases were: a watchdog
    // that reported "flush-push, band 14" was read as the push, and band 14 is simply
    // the LAST crumb a frame writes -- so it named everything from the final push to
    // the next loop's dog feed. That is the flush tail, the warp build, the telemetry
    // and the rest of loop(). Three regions reported as one, and the fix aimed at the
    // push changed nothing because the push was never where it died.
    CRUMB_FEND,      // after the last band: capture end, the flush's own tail
    CRUMB_TAIL,      // loop() after the flush: warp build, housekeeping
    CRUMB_TELEM,     // ...and the two-second telemetry write, on its own
    CRUMB_SLOTS
};

// Where the frame is now, and ONE BYTE OF DETAIL WHOSE MEANING DEPENDS ON WHERE:
// the game state for the main-loop phases, the band index for FDRAW/FSCAN/FPUSH,
// and nothing at all for BOOT and FWAIT. The parameter is not called `state` any
// more because that name is what got it printed as one -- see detail_str in the
// .cpp, and the two crash records it made unreadable.
//
// Called four times a frame; keep it cheap.
void vg_crumb(uint8_t where, uint8_t detail);

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
