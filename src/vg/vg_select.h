#pragma once
#include "vg_menu.h"
#include "vg_ui.h"
#include "vg_ship.h"   // SHIP_CLASSES: the wheel is as long as the roster

// ===========================================================================
// --- ship select: a wheel on the left, a detail panel on the right ----------
//
// The left column is the SAME WHEEL the callsign screen uses (vg_entry.cpp), and
// deliberately so: the gesture is learned once, the wheel has no fixed capacity
// so a fifth ship is a table row rather than a layout, and a vertical drag on the
// LEFT is the throttle gesture in flight -- the menu teaches the control.
//
// ===========================================================================

// THE SCREEN STAYS DIVIDED: the left strip is the ship wheel and nothing else
// draws over it. What changed is the SIZE of the reading matter in the right
// column, and it changed because of the device.
//
// The panel is 2.16in on the diagonal at 480x480, which is 314 ppi. A scale 1
// glyph is 7px, and 7px there is 0.57mm -- a point and a half. That is not small
// type, it is type nobody can read, and it never showed on the desktop because
// the window is scaled up three times.
//
// Scale 2 is 1.13mm and legible. The right column is 243px of usable width, which
// is TWENTY characters at that scale against weapon descriptions of 25 to 31 --
// so the descriptions WRAP. Wrapping is what keeps the division: the alternative
// was to run the header across the whole glass, and the whole glass includes the
// wheel.
#define SEL_WHEEL_X     VG_GLASS_X0
// WIDER, AND THE PANEL PAID FOR IT. The names are tracked now and a tracked
// BALLISTA is 117px against the 96 it used to be, so the window had to grow with
// them or the gain would have been spent on the margins.
//
// 148 and a panel starting at 188 is the most the roster allows, and the limit is
// not the wheel: BALLISTA's WPN row measures 243px and the panel's inside is 244.
// One more pixel of wheel and that line closes its gap; six more and it clips.
// THE WHEEL TOOK WIDTH BACK FROM THE PANEL, because the panel stopped needing
// it. The weapon rows used to carry thirty-one characters of prose and now carry
// twelve to fifteen, so the column that was sized for the longest sentence is
// sized for the longest NAME instead -- and the wheel, which has to be read at
// arm's length on a 314 ppi panel, is where the width does more good.
//
// 176 and a panel at 216 is the balance. The wheel holds BALLISTA at scale 3 with
// ten pixels either side; the panel holds ACTIVE GUIDANCE on one line beside its
// tab, which is fifteen characters and exactly what the column has room for.
#define SEL_WHEEL_W     176
#define SEL_PANEL_X     216
#define SEL_PANEL_W     (VG_GLASS_X1 - SEL_PANEL_X)
#define SEL_PANEL_Y     VG_GLASS_Y0
#define SEL_PANEL_H     (VG_GLASS_Y1 - VG_GLASS_Y0)
// A WINDOW, not a column. 168px is the callsign wheel's own height
// (ENT_WHEEL_H) and it is the same number on purpose: two wheels that behave
// identically should look identically sized. A full-height frame around three
// visible rows reads as a list with gaps in it.
#define SEL_WHEEL_H     196
#define SEL_WHEEL_Y     (SEL_PANEL_Y + (SEL_PANEL_H - SEL_WHEEL_H) / 2)
#define SEL_WHEEL_MID   (SEL_WHEEL_Y + SEL_WHEEL_H / 2)   // the detent row
#define SEL_WHEEL_PITCH 50      // px between neighbours on the wheel

// EXTRA SPACE BETWEEN THE LETTERS OF A SHIP NAME.
//
// A name is the one string on this screen that is read as a SHAPE rather than as
// a sentence -- you are picking it out of a list of four, not reading it -- and
// the curve was closing the gaps on one side of the arc enough to weld letters
// together. 3px at the wheel's scale 2, 2 at the panel's scale 3, which is less
// because a bigger glyph already carries a bigger bearing.
// Down from 3 with the names a size larger: tracking exists to keep the curve
// from welding letters together, and a bigger glyph carries a bigger bearing of
// its own, so the same job needs less of it.
// SEL_TITLE_TRACK, its opposite number on the panel, went with the panel --
// see SHIPVIEW_TITLE_TRACK. The two are the same argument at two sizes.
#define SEL_NAME_TRACK  2

// HOW MANY NAMES THE WHEEL SHOWS AT ONCE, selection included.
//
// Every class, while the roster is small enough -- a chooser that hides two of
// your four options is asking you to remember them. Capped so that a longer
// roster still shows a WINDOW rather than a list: past five the wheel goes back
// to being a wheel, with more beyond the edges.
#define SEL_WHEEL_SHOWN 5

// WHERE THE DETENT ACTUALLY SITS, and it is NOT the middle of the window.
//
// The block of names is centred, not the selected row: with an even roster the
// rows run one above and two below, and pinning the detent to the middle left the
// block visibly low. An odd roster puts it back in the middle by itself.
//
// Shared, because the DRAW and the HIT TEST have to agree about it. They did not
// for one build, and a tap landing 19px from where the eye says it should is the
// kind of fault nobody reports as a coordinate bug -- it just feels unreliable.
#define SEL_WHEEL_N   ((SHIP_CLASSES < SEL_WHEEL_SHOWN) ? SHIP_CLASSES : SEL_WHEEL_SHOWN)
#define SEL_WHEEL_LO  (-((SEL_WHEEL_N - 1) / 2))
#define SEL_WHEEL_DETENT (SEL_WHEEL_MID - ((2 * SEL_WHEEL_LO + SEL_WHEEL_N - 1) * SEL_WHEEL_PITCH) / 2)
// THE ROW MARKER: a short inverse-video bar hard against the left edge, one per
// row. It was the selection's alone -- the same 6px mark the cards carried before
// the wheel existed -- and 42 is the height of the detent block, so the selected
// one spans exactly the two rules that bracket it.
#define SEL_SPINE_W     6
#define SEL_SPINE_H     42


// THE TITLE AND THE ENTER KEY ARE IN THE CHASSIS, not on the screen.

//
// The console has a lit window above the aperture and another below it, and they
// are where a terminal puts its banner and its one key. Neither draws a frame of
// its own any more: the metal already is the frame, and a second box inside it
// read as a button sitting on a button.
// How fast the banner crosses its window, in px a second. Slow enough to read a
// word at a time and not so slow that it looks stuck.
// The console's own tuning -- the curve, the fiducials, the sweep and the
// ticker -- lives in vg_console.h with the code that spends it.


// The key lives in drawing slot 1. See vg_console_key.

// --- hit tests, called by the state machine --------------------------------
// Which wheel row a contact hit, as an offset from the detent: 0 the selection,
// -1 the row above, +1 below. VG_WHEEL_NONE when the contact missed the wheel.
int  vg_select_row_at(float x, float y);

// THE WHEEL ITSELF, because the state machine drives the drag.
//
// It is not that the screen has given up owning its own control. Stepping the
// selection is a change to the GAME -- vg_game_select_ship re-arms the player --
// so it happens where the game state lives, and the geometry it is stepped
// against belongs here with the drawing. Handing over the wheel is the smallest
// thing that lets both be true.
VgWheel* vg_select_wheel(void);
bool vg_select_confirm_at(float x, float y);

void vg_draw_select(void);
