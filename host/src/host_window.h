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

// Mouse movement since the last call, in raw device counts, and clears it.
//
// NOTHING STEERS WITH THIS ANY MORE -- the stick is the pointer's POSITION now,
// not a running total of its motion. Kept because the accumulator has to be
// drained by somebody, and because a relative control may want it back.
void host_mouse_take_delta(float* dx, float* dy);

// Capture is for FLYING: the pointer is hidden and fenced, and it is warped to
// the middle once on the way in so the stick starts at neutral. It is NOT pulled
// back to the centre every frame any more -- doing that destroyed the one thing
// the stick now reads, which is where the pointer actually is.
//
// A menu wants the opposite: a visible pointer that stays where the player put
// it, so the port turns capture off there.
void host_window_set_capture(bool on);

// HOW FAR THE POINTER MAY GET FROM THE MIDDLE OF THE PICTURE, in LOGICAL pixels.
// Zero or less fences it to the whole picture, which is what a menu wants.
//
// This is the stick's mechanical stop. Fencing at exactly the displacement that
// means full deflection is what makes the control feel like a stick rather than
// like a pointer that happens to steer: the hand runs out of travel at the same
// moment the ship runs out of turn, and there is no dead region past the stop to
// wind back through.
void host_window_set_fence(float half_logical);

// Put the pointer in the middle of the picture -- the stick to neutral.
void host_mouse_centre(void);

// Put the pointer at a LOGICAL point in the picture.
//
// The stick is driven by raw counts, and this is what keeps the pointer showing
// where the stick has got to instead of wandering off on its own: every frame the
// cursor is placed at the thumb. It is what makes the fence a real stop -- the
// hand meets the edge of the box at the same moment the stick meets full
// deflection, rather than the two drifting apart until they mean nothing.
void host_mouse_place(float lx, float ly);

// Where the pointer is, in LOGICAL game pixels rather than window pixels, so the
// caller never has to know the window scale. False when it is outside the client
// area, which the fence makes impossible while the window has focus.
bool host_mouse_logical(float* x, float* y);

// VK_* codes.
bool host_key_down(int vk);
bool host_window_focused(void);
