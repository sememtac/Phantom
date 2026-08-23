// The desktop window, and the raw input read from it.
//
// This file knows about Windows and nothing about the game. The translation from
// a mouse into something the game recognises happens one level up, in
// vg_port_win32.cpp, because that is a decision about FEEL rather than about
// windowing -- see the note on touch synthesis there.
#pragma once
#include <stdint.h>

// scale is an integer multiplier on the 480x480 panel. Integer on purpose: a
// fractional one resamples the picture and this panel is 1 px wide lines drawn
// on a black field, which is the worst thing you can ask a resampler to do.
bool host_window_open(int scale, const char* title);
void host_window_close(void);

// Drains the message queue. False once the user has asked to close the window.
bool host_window_pump(void);
bool host_window_quit_requested(void);

// One frame, in PANEL bytes: 480x480 of the same byte-swapped RGB565 the device
// puts on the wire. Converts, blits, and paces to the device's frame rate.
void host_window_present(const uint16_t* panel);

// ---- raw input -------------------------------------------------------------

// Mouse movement since the last call, in raw device pixels, and clears it. The
// cursor is captured and re-centred every frame while the window has focus, so
// this keeps accumulating however far the pointer is pushed.
void host_mouse_take_delta(float* dx, float* dy);

// VK_* codes.
bool host_key_down(int vk);
bool host_window_focused(void);
