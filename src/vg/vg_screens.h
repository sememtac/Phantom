#pragma once
#include "vg_config.h"

// ===========================================================================
// Menu screens: ship select, the tournament map, and pause.
//
// Layout lives in this header rather than inside each .cpp because two separate
// things have to agree on it exactly -- the drawing, and the hit tests the state
// machine runs against a tap. Keeping the rectangles in one place is what stops
// a button drifting away from the thing that lights up.
// ===========================================================================

#define MENU_TAP_SLOP   16.0f   // px of travel a contact may drift and still tap

// --- ship select: four stacked cards over a confirm bar --------------------
#define SEL_CARD_X      34
#define SEL_CARD_W      412
#define SEL_CARD_Y0     104
#define SEL_CARD_H      66
#define SEL_CARD_PITCH  74
#define SEL_GO_X        140
#define SEL_GO_Y        410
#define SEL_GO_W        200
#define SEL_GO_H        52

// --- pause -----------------------------------------------------------------
#define PAU_BTN_X       120
#define PAU_BTN_W       240
#define PAU_BTN_H       56
#define PAU_RESUME_Y    212
#define PAU_QUIT_Y      292

// --- tournament map --------------------------------------------------------
#define BRK_GO_X        146
#define BRK_GO_Y        414
#define BRK_GO_W        188
#define BRK_GO_H        48

static inline bool vg_in_rect(float x, float y, int rx, int ry, int rw, int rh) {
    return x >= (float)rx && x < (float)(rx + rw) &&
           y >= (float)ry && y < (float)(ry + rh);
}

// --- hit tests, called by the state machine --------------------------------
int  vg_select_card_at(float x, float y);      // 0..3, or -1
bool vg_select_confirm_at(float x, float y);
bool vg_pause_resume_at(float x, float y);
bool vg_pause_quit_at(float x, float y);
bool vg_bracket_ready_at(float x, float y);

// --- tournament map view state ---------------------------------------------
void vg_bracket_pan(float dx, float dy);
void vg_bracket_focus_player(void);            // centre on the next match

// --- drawing ---------------------------------------------------------------
void vg_draw_select(void);
void vg_draw_pause(void);
void vg_draw_bracket(void);
