#pragma once
#include <stdint.h>

// THE SET TURNING ON AND OFF -- the broadcast transition, lifted out of vg_band.cpp.
//
// It was 208 lines in the middle of the raster half, and it belonged to none of it: four
// statics nothing else reads, one call site, and no shared state with the primitives, the
// canopy or the scanlines it sat between. What made it safe to move is that its call site
// is behind vg_tv_active() -- so the frame reaches this module only during a transition,
// and a cross-unit call on a path that runs for half a second costs nothing.
//
// RENAMED ON THE WAY OUT, from vg_rast_tv/vg_rast_tv_active. The prefix and the file are
// not allowed to disagree twice: see the note on vg_hud_ in vg_raster.h, which is the same
// fault left in place because moving those functions was worse than living with it. Here
// there was nothing to weigh -- the code was moving anyway.

// The transition's three controls, all 0..1 and clamped. Called from vg_render.cpp, which
// drives them off the state clock.
//
//   open  how much of the height is picture at all, centred -- the aperture
//   wide  ...and of the width: the dot before the line
//   wash  how far the lit part is washed toward white
//   dim   how far what remains is faded toward black
void vg_tv_set(float open, float wide, float wash, float dim);

// Whether any of the four is away from its resting value. The band pass is skipped
// entirely when this is false, which is what keeps the effect off the frame's bill.
bool vg_tv_active(void);

// One band, in place. Call only when vg_tv_active() -- it does not check.
void vg_tv_band(uint16_t* band, int by0);
