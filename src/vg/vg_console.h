#pragma once
#include <stdint.h>
#include "vg_bezel.h"

// ===========================================================================
// THE CONSOLE: EVERY MENU IS THE SAME MACHINE
//
// Three screens are bolted into this terminal now -- callsign registration, ship
// select, and the tournament table next -- and they are the same machine. The
// chassis, the running headline, the keys and the curve of the glass belong in
// one place, and what a screen supplies is its headline, what it draws inside
// the glass, and the words on its keys.
//
// WHERE THINGS GO COMES FROM THE DRAWING. The artist paints each hole in the
// colour of the job it does and tools/bezel_bake.py carries that through, so a
// screen asks for "the headline" and "key 0" rather than being handed
// coordinates. That is the whole point of the split: the tournament art will have
// several keys along the bottom and a bigger window above them, and none of this
// file has to change for it.
//
// THE BRACKET. Everything a screen draws between open and close is ON THE GLASS
// and bends with it. The headline and the keys are drawn outside that bracket,
// because they sit in windows cut into the plating, and a lit inset that bows
// with the tube reads as a decal stuck on it.
//
//   vg_console_open(&BEZEL_CONSOLE, "SELECT SHIP", nullptr);
//   ... the screen draws in vg_console_glass() ...
//   vg_console_key(1, "ENTER");
//   vg_console_close();
//
// The chassis is painted by close(), LAST, so it masks whatever ran past a hole
// -- with the drawing's own outline, chamfers and all. Nothing in a screen has to
// know the shape of the aperture.
// ===========================================================================

// WHICH HOLE THE KEY IS IN. Slot 0 is the big window every screen lays itself out
// in, so the first key is slot 1. It was SEL_GO_SLOT and lived with the ship
// select screen, which registration then reached across for -- a screen's
// constant doing a machine's job. A drawing with several keys names them from
// here upward.
#define VG_CON_KEY      1

// THE GLASS IS THE HOLE, INSET. The chassis's lip overhangs its own aperture, so
// ink drawn hard against the edge of the hole lands under the metal. Seven
// pixels across and one down is what the select screen was using as a hand-picked
// constant before the console layer existed, and it is right for the same reason
// on any drawing: it is the overhang, not a margin of taste.
#define VG_GLASS_INSET_X 7
#define VG_GLASS_INSET_Y 1

// --- what the machine looks like -------------------------------------------
//
// These are the CONSOLE'S, not one screen's. They were named SEL_* and lived in
// vg_screens.h because ship select was the only tenant; the tournament table will
// spend every one of them and should not have to say SEL_ to do it.
//
// How hard the display bends, as a multiple of the cockpit's own HUD_WARP_K.
// The curve has to be readable as curvature and stop well short of reading as a
// fault, and the ceiling on that is lower than a ladder of screenshots suggests.
// 2.6 is plainly broken -- the wheel box skews and its names leave their column.
// 1.8 survives a still frame and does not survive being looked at: the tilt on
// the wheel names and the bow on the panel border are the first things the eye
// goes to, which is the wrong thing to notice on a screen you are reading.
//
// 1.0 is the cockpit's own bend, and borrowing it turned out to be the wrong
// argument: a HUD is glanced at over a moving world, and this is a page of text
// somebody reads. The same curve that is invisible in flight is legible here as
// wobble, and the words pay for it.
//
// 0.5 leaves the borders visibly bowed -- a straight line is long enough to show
// a gentle curve -- while the arc across one word is under a pixel.
#define VG_CON_WARP  0.5f

// The chord length the curve is cut into on this screen. The cockpit's 64 leaves
// the 266px panel border as five straight pieces with visible joints; 18 reads as
// a curve, at one primitive per chord.
#define VG_CON_SEG   24.0f

// THE FIDUCIAL GRID. Spacing and arm length of the alignment crosses tiled
// across the glass. 74 gives five across and three down inside the aperture,
// which is enough to read as a pattern and few enough that they stay chrome --
// at half this they start to look like content.
#define VG_CON_TICK_STEP   74
#define VG_CON_TICK_ARM    3

// How fast the sweep crosses the glass, in px a second. Slow: it is a sign of
// life, not an animation, and anything quick enough to follow with the eye is
// something the eye then has to keep following.
#define VG_CON_SWEEP_RATE  38.0f

// How far past the aperture the sweep is drawn at each end. The warp pulls a
// point inward in proportion to its distance from the centre, and the ends of a
// full-width line are the furthest points on it -- drawn edge to edge the sweep
// stops short of both edges and reads as cut off. The chassis trims the excess.
#define VG_CON_SWEEP_OVER  26

// How often the glass is offered a fault, in buckets a second. One in six of
// them takes it, so a tear lands every few seconds. Rare is the entire setting:
// a panel that glitches constantly is a style, one that is clean and then is not
// is broken.
#define VG_CON_FAULT_RATE  1.4f

#define VG_CON_TICKER_RATE 46.0f

// The clear space between one pass of the banner and the next. Three characters
// at the title's own scale: enough that the two readings do not run together,
// short enough that the glass is never empty.
#define VG_CON_TICKER_GAP  54

// Put up the chassis's headline and open the glass. `note` is a second line in
// the headline window, or null for a one-line headline at the larger size.
void vg_console_open(const VgBezel* b, const char* headline, const char* note);

// Close the glass, then paint the chassis over everything.
void vg_console_close(void);

// The rectangle a screen may lay itself out in: slot 0, the big window.
// Returns false if the drawing has no such hole.
bool vg_console_glass(int* x, int* y, int* w, int* h);

// A KEY IN A HOLE. Draws the label in slot `n` and lights it while held.
//
// The chassis already drew the box, so a key is its label, the line under it that
// marks the primary action, and the one thing a key must do: change while it is
// pressed.
void vg_console_key(int n, const char* label);

// STOP THE CURVE FOR ONE OBJECT, and say where the curve would have put it.
//
// Some things must not be bent vertex by vertex, and each fails differently:
//
//   A DENSE RUN OF THIN FILLS -- a colour ramp -- leaves the warp as a strip of
//   quads, two triangles each. Two hundred columns became eight hundred
//   primitives and overflowed the instrument slice, which drops whatever was
//   submitted last: the chassis vanished.
//
//   A TWO-PIXEL RULE cut into chords loses a pixel where the chords meet, so a
//   key's underline came out unevenly thick.
//
//   A BITMAP FONT unspaces. The glyph stays upright while its origin moves, so
//   the gaps between letters stop being equal.
//
// All three want to be drawn FLAT and moved bodily to where the tube would have
// put them -- the trade vg_hud_warp_at already makes for the rear-view patch.
//
// Pass dx/dy to be told the displacement at (cx, cy); pass null for a thing that
// sits in the PLATING rather than on the glass, which does not move at all.
// Always pair with vg_console_bend.
void vg_console_flat(float cx, float cy, int* dx, int* dy);
void vg_console_bend(void);

// Does this point hit key `n`? The area is DELIBERATELY BIGGER than the hole --
// see the note in the implementation. Use this for the tap test so that what
// lights and what registers are the same rectangle.
bool vg_console_key_at(int n, float x, float y);
