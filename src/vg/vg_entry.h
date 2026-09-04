#pragma once
#include "vg_menu.h"
#include "vg_input.h"

// ===========================================================================
// REGISTRATION IS IN THE SAME MACHINE AS SELECT, so it lives inside the same
// aperture and every one of these came in to meet it. The screen used to have the
// whole panel: title at 30, wheels to 272, the ramp to 396 and NEXT to 458. The
// glass ends at 365 and the banner and the key belong in the plating's own
// windows, so the working height went from 292px to 266.
//
// What gave way was slack, not content. The letter wheels only ever painted
// about 136px of their 168 -- the window is sized to match the ship wheel, not to
// the letters -- so the column kept its size and the space beneath it paid.
// ===========================================================================

#define ENT_COL_W       90
#define ENT_COL_X0      ((SCR_W - 3 * ENT_COL_W) / 2)
#define ENT_WHEEL_Y     (VG_GLASS_Y0 + 1)
#define ENT_WHEEL_H     168
#define ENT_TRAIL_X     100
#define ENT_TRAIL_Y     264
#define ENT_TRAIL_W     280
#define ENT_TRAIL_H     44
#define ENT_HUE_X       40
// The handle stands 9px proud of the ramp at both ends, so the ramp has to stop
// 9px short of the glass or the grip is cut off by the steel.
#define ENT_HUE_Y       320
#define ENT_HUE_W       400
#define ENT_HUE_H       30
// Acquiring the ramp should not demand precision -- the slider is a coarse
// choice and the finger covers most of it.
#define ENT_HUE_PAD     26

// ...EXCEPT DOWNWARD, WHERE THE KEY IS.
//
// The pad is symmetric everywhere it can afford to be, and below the ramp it
// cannot: 26 puts its edge at y 376 and the chassis key hole starts at 378. Two
// pixels, and the key's own hit area is deliberately larger than its hole, so on
// the board a thumb driving the ramp across lit NEXT.
//
// 12 stops the pad at 362, inside the glass and sixteen clear of the plating. The
// contact claim below is the other half of the fix and the more general one; this
// is the half that keeps the two areas from overlapping in the first place.
#define ENT_HUE_PAD_LO  12

// HOW MUCH OF THE COLOUR WHEEL THE SLIDER SPENDS ITS WIDTH ON.
//
// vg_hue_col takes a full turn, so a slider mapped straight onto it ran red - yellow -
// green - cyan - blue - magenta - red and arrived back where it started. The two ends
// were the same colour, which wastes the last third of a 400px bar on a journey home
// nobody asked for and makes the extremes impossible to tell apart.
//
// Two thirds of a turn is red through to pure blue: h=2/3 is exactly (0,0,1), so the bar
// ends on a primary rather than part way through a ramp.
//
// vg.trail_hue stays a REAL HUE in 0..1 and is not rescaled -- the save file, the
// opponents' hues and vg_hue_col all keep one meaning for the number. Only the picker is
// restricted, so a profile saved with a magenta hue still draws magenta; its handle just
// pins to the end of the bar.
#define ENT_HUE_SPAN    0.6667f
// The key is the chassis window, exactly as it is on the select screen: one
// machine, one place the key lives.
// Registration uses the same key hole as select -- one machine, one key. See
// vg_console_key_at(VG_CON_KEY, ...).

// Screens carrying interaction state of their own -- a wheel mid-drag, a slider
// being held -- own their input handling instead, because that state has nowhere
// sensible to live in the state machine. Each returns true when it is finished.
bool vg_entry_update(const VgInput* in, bool tap, float tx, float ty);
void vg_entry_reset(void);

void vg_draw_entry(void);
