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

// ===========================================================================
// A WHEEL
//
// There are two in the game and they are the same mechanism to a thumb: the
// three callsign letters, and the ship chooser. What they share is not the
// drawing -- see below -- it is the ARITHMETIC, and the arithmetic is where both
// of them have gone wrong.
//
// WHERE A ROW IS, WORKED OUT ONCE. The draw and the hit test have to agree about
// it, and for one build they did not: the block of names is centred rather than
// the selected row, so the detent is NOT the middle of the window, and a second
// copy of that sum in the drawing put a tap 19px from where the eye said it
// should be. Nobody reports that as a coordinate bug. It is reported as the
// screen feeling unreliable.
//
// A DRAG IS COUNTED IN DETENTS, and that loop was written out twice against one
// shared WHEEL_STEP -- which is the half of "one constant on purpose" that a
// constant cannot enforce by itself. The sign is in here too: dragging DOWN
// rolls the wheel down, which brings earlier items up into view, the way a
// physical wheel behaves. Backwards on one of the two would be the first thing a
// player noticed.
//
// WHAT IS NOT HERE IS THE DRAWING, and that is deliberate. The letter wheel puts
// its selection at scale 6 with two neighbours either side at 3 and 2; the ship
// wheel puts every class at 2 with the selection at 3, a spine down the edge and
// the row under the thumb lit. A widget that drew both would want a scale per
// distance, a colour per distance, a tracking, a spine and a highlight -- which
// is not a widget, it is a configuration language with two users. Each screen
// walks the rows from here and draws its own.
// ===========================================================================

// PX OF DRAG PER DETENT, and it is ONE constant on purpose. The callsign letters
// and the ship wheel are the same mechanism to a thumb, and they would stop being
// the same the first time somebody retuned one of two copies.
#define WHEEL_STEP      26.0f

// A contact that is not on the wheel at all.
#define VG_WHEEL_NONE   99

struct VgWheel {
    int   x, w;      // the strip a contact has to be inside...
    int   y, h;      // ...and its extent. y is the TOP of it, not the detent.
    int   detent;    // where the selection sits, which need not be the middle
    int   pitch;     // px from one row to the next
    int   lo, n;     // the first row offset drawn, and how many are drawn
    float accum;     // px of a drag in progress, since the last detent
};

// The y of row k, k being an offset from the selection: 0 is the selection, -1
// the row above it, +1 the row below.
static inline int vg_wheel_row_y(const VgWheel* wh, int k) {
    return wh->detent + k * wh->pitch;
}

// Which row a contact landed on, or VG_WHEEL_NONE if it missed the wheel.
//
// Clamped to the rows that are actually DRAWN, so a tap in the margin below the
// last one nudges by one rather than by however far the finger was out.
int vg_wheel_row_at(const VgWheel* wh, float x, float y);

// How many detents this frame's drag moved, keeping the remainder. Feed it the
// frame's dy while a contact owns the wheel; positive is forward through the
// list. See the note above about the sign.
int vg_wheel_drag(VgWheel* wh, float dy);

// The contact let go. The remainder is dropped, so the next drag starts from the
// detent rather than from wherever the last one stopped.
void vg_wheel_release(VgWheel* wh);

// A framed key with a dark well, corner ticks, and a lit state under the thumb.
//
// For screens the chassis does NOT frame. Inside the console the metal already
// draws the box and this would be a button on a button -- see vg_console_key.
void vg_button(int x, int y, int w, int h, const char* label,
               bool primary, bool live);
