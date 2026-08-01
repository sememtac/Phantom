#pragma once
#include <stdint.h>
#include "vg_vec.h"

// Procedural cosmic backdrop.
//
// The renderer is a sparse vector rasteriser; a nebula is the opposite -- dense,
// area-filling and smoothly varying. Evaluating noise per pixel per frame is far
// out of reach (50-200 cycles/px against a ~10 cycle/px budget), so the noise is
// baked ONCE into a small texture at level start and the per-frame cost is
// reduced to a table lookup.
//
// The fill replaces the band memset rather than adding to it, and lives entirely
// in the per-band rasteriser, which is hidden under DMA. It therefore costs
// nothing in `submit`, which is the stage actually under pressure.
//
// Sharp detail deliberately does NOT come from here: a low-resolution texture
// cannot draw crisp stars, so star clusters stay in the vector point renderer,
// which is nearly free. Soft goes in the texture, sharp stays vector.

enum SkyKind : uint8_t {
    SKY_NEBULA = 0,   // drifting fBm cloud
    SKY_GALAXY,       // barred spiral, warm core and cooler arms
    SKY_CLUSTER,      // knots of unresolved starlight in faint haze

    // Count of the backdrops, and the modulus a match rolls against. Every kind
    // is a combat kind now: the menus draw no backdrop at all, so there is no
    // longer a set of skies that are never picked for a fight.
    SKY_KINDS
};

// No backdrop. The band fill falls back to its memset and the menus show the
// starfield over black. Kept as its own call rather than a kind, because the
// absence of a sky is a state of the module and not one more thing to generate.
void vg_sky_none(void);

const char* vg_sky_name(void);

// Where the match is being held, e.g. "VELA NEBULA". Composed from the same
// seed that built the texture, so the name and the sky are the same place.
const char* vg_sky_place(void);

// 0..1. Below 1 the backdrop is dissolved in by rows rather than dimmed --
// dimming would mean touching every pixel of every frame, which is the one
// thing the fill exists to avoid. Skipped rows are cleared instead, so a
// partial reveal is cheaper than a full one, not dearer.
void  vg_sky_set_reveal(float r);

// Allocates the texture pair. Returns false if internal SRAM is short, in which
// case the renderer falls back to a plain black clear.
bool vg_sky_init(void);
bool vg_sky_ready(void);

void vg_sky_generate(SkyKind kind, uint32_t seed);

// Carry the backdrop round by this frame's world rotation -- the SAME Mat3 the
// stars and the arena ride, which is the whole point.
//
// It used to take scalar pitch and yaw deltas and integrate them into a flat
// (u,v) pan position. Angles do not compose: roll ninety degrees onto a wing,
// pull ninety, and with no yaw input at all the nose ends up seventy-six
// degrees of heading from where that sum says it is. A tiling cloud hid it,
// having nothing in it to recognise. A backdrop with a landmark in it does not,
// and neither does a rear view, which has to INVERT a heading the old form
// never stored.
//
// `bank` is the cosmetic lean only. The real roll is already in R.
void vg_sky_orient(const Mat3& R, float bank);

// Advance the backdrop by this frame's rotation. Angles in radians.
void vg_sky_step(float d_pitch, float d_yaw, float bank);

// Turn the backdrop to match a camera looking aft. The backdrop is not geometry
// and never passes through the camera, so it has to be told.
void vg_sky_set_rear(bool on);

// Where the rear-view patch is, in LOGICAL coordinates, so the fill can put an
// aft sky inside it. Zero width means there is no patch this frame.
void vg_sky_set_patch(int x, int y, int w, int h);

// Paint the patch's aft backdrop into this band. Called from the band raster
// when it meets a PRIM_SKY, so that it lands in submission order.
void vg_sky_fill_patch(uint16_t* band, int band_y0);

// Fill one band from the texture. Replaces the memset entirely.
void vg_sky_fill_band(uint16_t* band, int band_y0);
