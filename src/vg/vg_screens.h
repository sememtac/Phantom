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
// A STACK, not fixed slots. The number of entries depends on where the pause
// came from -- SKIP exists only in a mode that can be skipped -- and hard-coded
// Y positions would need a new pair of constants for every combination. The
// stack is centred, so adding an entry later moves everything and breaks nothing.
#define PAU_BTN_X       120
#define PAU_BTN_W       240
#define PAU_BTN_H       50
#define PAU_BTN_GAP     12
#define PAU_STACK_CY    300      // the stack's centre, whatever its height

// The audio page.
#define PAU_SLD_X       100
#define PAU_SLD_W       280
#define PAU_SLD_H       22
#define PAU_SLD_MUSIC_Y 210
#define PAU_SLD_SFX_Y   290
#define PAU_BACK_Y      360

// What the pause screen is offering. Built as a list rather than tested one at a
// time, so the drawing and the hit test cannot disagree about which entry is
// where -- they walk the same list.
enum PauseItem : unsigned char {
    PAUSE_NONE = 0,
    PAUSE_RESUME,
    PAUSE_AUDIO,
    PAUSE_SKIP,      // only in a mode that can be skipped
    PAUSE_QUIT
};

// Fills `out` (4 entries is enough) and returns how many there are.
int  vg_pause_items(bool skippable, PauseItem* out);
// The rect of entry `i` of `n`.
void vg_pause_rect(int i, int n, int* x, int* y, int* w, int* h);
// Which entry is under the point, or PAUSE_NONE.
PauseItem vg_pause_item_at(float x, float y, bool skippable);
// Audio page: which slider is under the point, and the value 0..1 for an x.
bool  vg_pause_music_at(float x, float y);
bool  vg_pause_sfx_at(float x, float y);
bool  vg_pause_back_at(float x, float y);
float vg_pause_slider_value(float x);

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
#define ENT_HUE_Y       342
#define ENT_HUE_W       400
#define ENT_HUE_H       54
// Acquiring the ramp should not demand precision -- the slider is a coarse
// choice and the finger covers most of it.
#define ENT_HUE_PAD     26
#define ENT_GO_X        150
#define ENT_GO_Y        408
#define ENT_GO_W        180
#define ENT_GO_H        50

// --- tournament map --------------------------------------------------------
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

bool vg_bracket_ready_at(float x, float y);
bool vg_bracket_course_at(float x, float y);
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

// The one button in the game. Drawn in the same idiom as the instrument panels:
// sunk into a dark well, framed, with corner ticks. A primary action is marked
// by a brighter frame and a bright key line under the label -- NOT by filling
// the whole rectangle, because a solid slab reads as a block rather than a
// control and throws away the frame that makes the rest of the interface look
// built.
void vg_button(int x, int y, int w, int h, const char* label,
               bool primary, bool live);

void vg_draw_select(void);
void vg_draw_pause(void);
void vg_draw_bracket(void);
void vg_draw_entry(void);
void vg_draw_repair(void);
