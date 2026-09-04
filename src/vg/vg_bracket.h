#pragma once
#include "vg_menu.h"
#include "generated/bezel_tourney.h"

// ===========================================================================
// The tournament sheet, bolted into the broadcast rig.
//
// It is the one menu that never had a chassis: it drew flat on the bare panel
// with black bands of its own top and bottom and vg_button keys under them. The
// bands never painted -- COL_BLACK is dropped by this renderer -- and the keys
// were three rectangles this header had to keep in step with three hit tests.
//
// Now it is the same machine ship select and registration are in, in its
// BROADCAST form: no glass, because this page is the picture rather than a
// terminal showing you one. See VgConsoleForm.
//
// The keys are holes in the art. Slots come out of the drawing top to bottom
// then left to right, so the big window is slot 0 and REPAIR, COURSE and READY
// are keys 1, 2 and 3 without anything here naming them -- which is the claim the
// console layer was built on and this is the screen that tests it.
// ===========================================================================

// THE WINDOW THE SHEET IS DRAWN IN, from the drawing rather than from a number
// somebody measured off it. A redrawn rig moves the sheet instead of quietly
// cropping it -- the same derivation VG_GLASS_* makes for the terminal, and the
// same lip inset, because it is the same kind of bevel overhanging its own hole.
#define BRK_VIEW_X0   (BEZEL_TOURNEY_S0_X0 + VG_GLASS_INSET_X)
#define BRK_VIEW_Y0   (BEZEL_TOURNEY_S0_Y0 + VG_GLASS_INSET_Y)
#define BRK_VIEW_X1   (BEZEL_TOURNEY_S0_X1 - VG_GLASS_INSET_X)
#define BRK_VIEW_Y1   (BEZEL_TOURNEY_S0_Y1 - VG_GLASS_INSET_Y)
#define BRK_VIEW_W    (BRK_VIEW_X1 - BRK_VIEW_X0 + 1)
#define BRK_VIEW_H    (BRK_VIEW_Y1 - BRK_VIEW_Y0 + 1)

// The three keys, in reading order. Named so the draw and the state machine can
// say which is which; the RECTANGLES are the drawing's and nobody here has them.
#define BRK_KEY_REPAIR 1
#define BRK_KEY_COURSE 2
#define BRK_KEY_READY  3

// THE CHYRON, built once when the page is arrived at rather than every frame.
//
// Results do not change while you are looking at them, and the string is the
// whole tournament so far -- up to fifteen matches, which is not something to
// compose sixty times a second for a banner that takes half a minute to read
// itself out once.
void vg_bracket_chyron(void);

// The line the banner is running, for whoever is putting it up.
const char* vg_bracket_headline(void);

// --- hit tests, called by the state machine --------------------------------
bool vg_bracket_ready_at(float x, float y);
bool vg_bracket_course_at(float x, float y);
bool vg_bracket_repair_at(float x, float y);

// THE CHYRON, built once when the page is arrived at rather than every frame.
//
// Results do not change while you are looking at them, and the string is the
// whole tournament so far -- up to fifteen matches, which is not something to
// compose sixty times a second for a banner that takes twenty seconds to read
// itself out once.
void vg_bracket_chyron(void);

// --- view state ------------------------------------------------------------
void vg_bracket_pan(float dx, float dy);
void vg_bracket_focus_player(void);            // centre on the next match

void vg_draw_bracket(void);
