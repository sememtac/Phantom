#pragma once
#include <stdint.h>

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

    // Count of the IN-GAME backdrops, and the modulus a match rolls against.
    // Anything at or past this is never picked for combat.
    SKY_KINDS,

    // Menu only: a supermassive black hole, lensed disc and all. The backdrop
    // the player actually sits and looks at, so it is allowed to be the loudest
    // of the set -- combat backdrops are deliberately restrained so they never
    // compete with a contact at range.
    SKY_MENU
};

const char* vg_sky_name(void);

// Allocates the texture pair. Returns false if internal SRAM is short, in which
// case the renderer falls back to a plain black clear.
bool vg_sky_init(void);
bool vg_sky_ready(void);

void vg_sky_generate(SkyKind kind, uint32_t seed);

// Advance the backdrop by this frame's rotation. Angles in radians.
void vg_sky_step(float d_pitch, float d_yaw, float bank);

// Fill one band from the texture. Replaces the memset entirely.
void vg_sky_fill_band(uint16_t* band, int band_y0);
