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
