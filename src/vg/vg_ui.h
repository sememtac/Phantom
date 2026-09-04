#pragma once
#include <stdint.h>
#include "vg_raster.h"

// ===========================================================================
// THE WIDGETS EVERY SCREEN USES
//
// Small things that belong to no one screen: centred text, the live pointer, a
// button. They lived in vg_screens.h because ship select happened to be where
// each was first needed, which made a specific screen's header a dependency of
// the console layer and of four other files that wanted one line out of it.
//
// The rule for this file is that nothing in it may know what a screen IS.
// ===========================================================================

// A line of text centred on the panel. This was copied into four files
// byte-for-byte -- entry, overlay, repair and screens -- which is what a
// three-line helper does when there is nowhere to put it.
static inline void vg_centred(int y, const char* s, uint16_t col, int scale) {
    vg_text((SCR_W - vg_text_width(s, scale)) / 2, y, s, col, scale);
}

static inline bool vg_in_rect(float x, float y, int rx, int ry, int rw, int rh) {
    return x >= (float)rx && x < (float)(rx + rw)
        && y >= (float)ry && y < (float)(ry + rh);
}

// ===========================================================================
// WHERE THE FINGER IS, FOR THE WHOLE INTERFACE
//
// A control that does not acknowledge a press feels broken before it feels
// slow: you cannot tell whether the machine took the input or whether you
// missed. Every button in the game had that fault, so the fix belongs in one
// place rather than in each screen.
//
// Recorded once a frame in vg_state_update, which is the single entry every
// state passes through, and read by whatever is drawing. That is why no draw
// function had to grow a VgInput parameter: a button already knows its own
// rectangle, so it can ask this whether the press is inside it.
//
// It is the LIVE contact, not a tap. A tap is an event that has already
// finished; this is the finger still being down, which is the thing a lit key
// is reporting.
// ===========================================================================
void vg_press_set(bool held, float x, float y);
bool vg_press_in(int x, int y, int w, int h);

// The contact itself, for a control whose shape is not a rectangle. The ship
// wheel resolves a press to a ROW through its own hit function, so it needs the
// point rather than a yes or no. Returns false when nothing is held.
bool vg_press_get(float* x, float* y);

// A framed key with a dark well, corner ticks, and a lit state under the thumb.
//
// For screens the chassis does NOT frame. Inside the console the metal already
// draws the box and this would be a button on a button -- see vg_console_key.
void vg_button(int x, int y, int w, int h, const char* label,
               bool primary, bool live);
