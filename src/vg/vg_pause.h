#pragma once
#include <stdint.h>

// ===========================================================================
// THE PAUSE SCREEN, AND THE CONFIG PAGE BEHIND IT
//
// Not a console screen. This one is drawn over a match rather than bolted into
// the registration terminal, so it keeps vg_button and its own framed keys --
// there is no chassis here to draw the box for it.
// ===========================================================================

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

// The config page. It was the audio page, and it grew a display toggle, which is
// why the button that opens it no longer says AUDIO.
#define PAU_SLD_X       100
#define PAU_SLD_W       280
#define PAU_SLD_H       22
#define PAU_SLD_MUSIC_Y 210
#define PAU_SLD_SFX_Y   290
#define PAU_CHK_Y       348      // the scanline box
#define PAU_CHK_SIZE    26
#define PAU_BACK_Y      404

// What the pause screen is offering. Built as a list rather than tested one at a
// time, so the drawing and the hit test cannot disagree about which entry is
// where -- they walk the same list.
enum PauseItem : unsigned char {
    PAUSE_NONE = 0,
    PAUSE_RESUME,
    PAUSE_CONFIG,
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
bool  vg_pause_scanline_at(float x, float y);
float vg_pause_slider_value(float x);

void vg_draw_pause(void);
