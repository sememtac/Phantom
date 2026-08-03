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

// AUDIO DELIVERY, in SAMPLES rather than microseconds, and not a frame-time
// measurement at all. A crackle has exactly two mechanical causes and they want
// opposite fixes, so the instrument's whole job is to tell them apart:
//
//   starve  the codec ran out of samples and played the silence the driver
//           clears for it. This is a HOLE in the waveform, and it is what a
//           click sounds like. Non-zero means the producer was late by more
//           than the queue was deep -- deepen the queue, or find the stall.
//   drop    the DMA ring refused samples we had already rendered, so a chunk of
//           waveform went in the bin and the two sides of the cut do not join.
//           Non-zero means we are producing faster than the codec consumes.
//
// If BOTH are zero and it still crackles, delivery is clean and the fault is in
// the mix -- which is the answer that saves the most time, because it rules out
// this whole seam.
//
// `lead_min` is the useful number even when the other two are zero: it is how
// close the queue came to running dry, so it says how much margin there is
// before starve stops being zero.
//
// Written on core 0 by the audio task, read and reset on core 1 by the telemetry.
// That race is benign in the same way the frame counters' is: a torn read costs
// one wrong diagnostic line out of a report every two seconds, and adding a lock
// to a diagnostic would put the audio task behind the renderer, which is the one
// thing the move to core 0 was for.
extern uint32_t g_audio_starve;    // samples of silence the codec had to invent
extern uint32_t g_audio_drop;      // samples rendered and then thrown away
extern int32_t  g_audio_lead;      // samples believed to be queued right now
extern int32_t  g_audio_lead_min;  // shallowest the queue got since the report
extern uint32_t g_audio_gap;       // biggest single pass, in samples
