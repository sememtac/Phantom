#pragma once
#include "vg_menu.h"
#include "vg_input.h"

// ===========================================================================
// Repair: the hull bar, the slider over a partial amount, and what it costs.
// The screen's own argument is at the top of vg_repair.cpp.
// ===========================================================================

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

// Screens carrying interaction state of their own -- a slider being held -- own
// their input handling instead, because that state has nowhere sensible to live
// in the state machine. Returns true when it is finished.
bool vg_repair_update(const VgInput* in, bool tap, float tx, float ty);
void vg_repair_reset(void);

void vg_draw_repair(void);
