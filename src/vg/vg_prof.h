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
