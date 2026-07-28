#pragma once
#include "vg_config.h"
#include "vg_input.h"

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

// --- callsign and trail colour ---------------------------------------------
#define ENT_COL_W       90
#define ENT_COL_X0      ((SCR_W - 3 * ENT_COL_W) / 2)
#define ENT_WHEEL_Y     104
#define ENT_WHEEL_H     168
#define ENT_TRAIL_X     100
#define ENT_TRAIL_Y     290
#define ENT_TRAIL_W     280
#define ENT_TRAIL_H     46
#define ENT_HUE_X       40
#define ENT_HUE_Y       348
#define ENT_HUE_W       400
#define ENT_HUE_H       38
#define ENT_GO_X        150
#define ENT_GO_Y        408
#define ENT_GO_W        180
#define ENT_GO_H        50

// --- tournament map --------------------------------------------------------
#define BRK_GO_X        250
#define BRK_GO_Y        414
#define BRK_GO_W        170
#define BRK_GO_H        48
#define BRK_REP_X       60
#define BRK_REP_Y       414
#define BRK_REP_W       170
#define BRK_REP_H       48

// --- repair ----------------------------------------------------------------
#define REP_BAR_X       40
#define REP_BAR_Y       120
#define REP_BAR_W       400
#define REP_BAR_H       44
#define REP_SLIDE_X     40
#define REP_SLIDE_Y     236
#define REP_SLIDE_W     400
#define REP_SLIDE_H     46
#define REP_BUY_X       56
#define REP_BUY_Y       346
#define REP_BUY_W       170
#define REP_BUY_H       54
#define REP_BACK_X      254
#define REP_BACK_Y      346
#define REP_BACK_W      170
#define REP_BACK_H      54

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
bool vg_bracket_repair_at(float x, float y);

// Screens carrying interaction state of their own -- a wheel mid-drag, a slider
// being held -- own their input handling instead, because that state has nowhere
// sensible to live in the state machine. Each returns true when it is finished.
bool vg_entry_update(const VgInput* in, bool tap, float tx, float ty);
bool vg_repair_update(const VgInput* in, bool tap, float tx, float ty);
void vg_entry_reset(void);
void vg_repair_reset(void);

// --- tournament map view state ---------------------------------------------
void vg_bracket_pan(float dx, float dy);
void vg_bracket_focus_player(void);            // centre on the next match

// --- drawing ---------------------------------------------------------------
void vg_draw_select(void);
void vg_draw_pause(void);
void vg_draw_bracket(void);
void vg_draw_entry(void);
void vg_draw_repair(void);
