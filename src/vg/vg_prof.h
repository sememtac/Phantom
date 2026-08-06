#pragma once
#include <stdint.h>

// Frame-time counters, written where the work happens and read by the telemetry
// line in main.cpp.
//
// A HEADER RATHER THAN `extern` AT THE USE SITE. These were declared inline in
// vg_render.cpp and vg_sfx.cpp, in function scope, which works and is silently
// unsafe: a function-local extern is a promise about a symbol defined somewhere
// else in the program, and nothing checks it against the definition. Change one
// of these to uint64_t in main.cpp and the two translation units disagree about
// its width, the linker matches them on name alone, and the number quietly goes
// wrong -- which is the worst way for a diagnostic to fail, because a diagnostic
// is what you reach for when you already do not trust the numbers.
//
// Microseconds, reset per frame by main.cpp. They measure SUBMIT -- building the
// primitive list -- and not the raster, which happens later under DMA and is
// timed separately.
extern uint32_t g_sub_star, g_sub_arena, g_sub_world, g_sub_hud;

// Just the mixer, so it can be told apart from the I2S write it feeds.
extern uint32_t g_sfx_render_us;

// AUDIO DELIVERY. Two numbers, and the interesting one is BLOCKED TIME.
//
// Since the synth moved to core 0 the audio task is allowed to wait on the codec,
// and it does -- it renders a fixed chunk and hands it over with a real timeout,
// so the hardware paces the loop instead of a clock model guessing at it. That
// inverts the health signal:
//
//   blocked  microseconds per second spent waiting for the DMA ring to have room.
//            HIGH IS GOOD. It means the ring is full, the queue is as deep as the
//            hardware allows, and the producer is comfortably ahead. Near zero
//            means the render cannot keep up and the codec is running on fumes.
//   short    samples the driver would not take even after waiting out the whole
//            timeout. Non-zero is a real fault: the ring is not draining.
//
// This replaced a modelled queue depth, which was measuring the wrong thing. The
// model assumed a short write meant "the ring is full"; in this driver it also
// means "ESP_LOGE", once per call, over USB CDC, from inside the audio task -- so
// the instrument that was meant to find the stall was causing a bigger one.
//
// Written on core 0 by the audio task, read and reset on core 1 by the telemetry.
// That race is benign in the same way the frame counters' is: a torn read costs
// one wrong diagnostic line out of a report every two seconds, and adding a lock
// to a diagnostic would put the audio task behind the renderer, which is the one
// thing the move to core 0 was for.
extern uint32_t g_audio_blocked_us;  // time spent waiting on the ring
extern uint32_t g_audio_short;       // samples refused even after the wait

// THE MIX'S HEADROOM, which is a different question from delivery and has to be
// asked separately -- the crackle on acceleration was delivery and the one on a
// collision is not.
//
// `peak` is the largest absolute sum the mixer produced BEFORE the soft rail, in
// units where 1.0 is full scale. It is the honest number: what the voices actually
// came to, not what came out. A collision was measured at 1.94.
//
// `knee` counts samples the soft rail acted on at all -- so it says how much of a
// window was loud enough to be shaped, which is the difference between a peak that
// brushed the curve once and one that sat on it.
extern float    g_synth_peak;        // largest |sum| before the rail, 1.0 = rail
extern uint32_t g_synth_knee;        // samples the soft rail shaped

// The HUD's own split. `hud` is the largest single item left in submit and it is
// half a dozen unrelated instruments, so one number for it cannot say which. What
// is left over after these two is everything else in vg_draw_hud, and the
// telemetry works it out by subtraction rather than timing it a third time.
extern uint32_t g_hud_radar, g_hud_throttle;

// THE GRID'S TWO HALVES, AND THEY NOW RUN ON DIFFERENT CORES.
//
// Added to choose the split point and kept because it is what judges it. The arena
// grid was the largest single item on submit's critical path, and its two loops are
// independent -- hoops around the tube, rails along it -- so they were separated:
// hoops on core 1 with the world, rails on core 0 with the instruments.
//
// So these two are no longer parts of one total. They are the two cores' shares, and
// the useful reading is whether they BALANCE against what else each core carries:
//
//   core 1   starfield + hoops + world objects
//   core 0   rails + instruments
//
// `sub` on the telemetry line is the slower of the two, so the gap between them is
// what is still recoverable. Measured after the split at hoops 812 and rails 595 --
// note the rails got slower moving cores, from 426, most likely flash-cache
// contention with the audio task, which also lives on core 0. A finer split therefore
// pays less than the arithmetic suggests.
extern uint32_t g_arena_hoop, g_arena_rail;

// THE TWO HALVES' WALL TIMES, which is what the split has to be balanced against and is
// not what was balanced against the first time.
//
// `sub` is the slower of the two, so the only number that decides where work should go is
// each half's TOTAL. g_sub_hud is not that: it brackets vg_draw_hud alone, and group B
// also carries the rear-view patch, the lock box, the missile markers and the overlays --
// about 900 us that no counter named. Balanced against the part instead of the whole, the
// arena grid went onto the core that was already busier, and a comparison taken on a
// lighter scene reported it as a saving.
//
//   a   the kick to the await: starfield, hoops, world objects, course gate
//   b   the whole of submit_instruments: rails, and every instrument
//
// The GAP between them is what is recoverable, and its sign says which way to move.
extern uint32_t g_sub_a, g_sub_b;
