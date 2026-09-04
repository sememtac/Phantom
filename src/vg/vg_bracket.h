#pragma once
#include "vg_menu.h"

// ===========================================================================
// The tournament map: the draw, and the three keys along the bottom of it.
// ===========================================================================

// Three buttons now, so they are narrower. The row still starts at 60 and ends
// at 412: the screen is round, and the bottom corners are cut, which is why the
// original two did not run to the edges either.
#define BRK_REP_X       60
#define BRK_REP_Y       414
#define BRK_REP_W       112
#define BRK_REP_H       48
#define BRK_CRS_X       180
#define BRK_CRS_Y       414
#define BRK_CRS_W       112
#define BRK_CRS_H       48
#define BRK_GO_X        300
#define BRK_GO_Y        414
#define BRK_GO_W        112
#define BRK_GO_H        48

// --- hit tests, called by the state machine --------------------------------
bool vg_bracket_ready_at(float x, float y);
bool vg_bracket_course_at(float x, float y);
bool vg_bracket_repair_at(float x, float y);

// --- view state ------------------------------------------------------------
void vg_bracket_pan(float dx, float dy);
void vg_bracket_focus_player(void);            // centre on the next match

void vg_draw_bracket(void);
