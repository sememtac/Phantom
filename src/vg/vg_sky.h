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
    // is a combat kind: there is no longer a sky that combat never picks, which
    // is what SKY_MENU and SKY_COURSE used to be. The menus draw one of these
    // three like everywhere else -- see vg_sky_menu.
    SKY_KINDS
};

// The menus' own backdrop: one FIXED venue, not a random one. The title screen
// is somewhere the player keeps coming back to, and a different sky each time
// would make it read as a different place rather than as home.
//
// Cheap to call repeatedly. It remembers whether the menu sky is the one
// currently in the texture, so moving between the title, the bracket and the
// repair screen costs nothing; only coming back from a venue rebuilds it.
void vg_sky_menu(void);

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

// THE VENUE'S OWN LIGHT: the mean colour of the whole panorama, 0..1 a channel.
// Worked out once, when the sky is generated. Neutral white before that.
//
// It exists for the opaque cockpit, which replaces the pixel it draws and so inherits
// nothing from the scene -- the light delta it replaced was lit by the backdrop for
// free, because adding light to a picture IS a relationship with it.
void vg_sky_ambient(float* r, float* g, float* b, float* lum);

// ...AND THE LIGHT FROM ASTERN, which is what falls on the inner faces of the cockpit
// frame and therefore what lights it. Changes as the ship turns; the venue mean above
// does not. See the note at the definition.
void vg_sky_light_behind(float* r, float* g, float* b, float* lum);

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
// when it meets a PRIM_SKY, so that it lands in submission order. `rows` is how
// many rows the caller actually owns from band_y0 down: the row split hands each
// core part of a band, and this fill derives its rows from the patch rectangle,
// so without the count it would overrun the share it was given.
void vg_sky_fill_patch(uint16_t* band, int band_y0, int rows);

// Fill one band from the texture. Replaces the memset entirely.

// ...and the same fill in two pieces, so two cores can share one band. The prep
// does the chart work once and must complete before either core fills; the fill
// touches only the rows it is given. r0 and r1 MUST BE EVEN or the row pairing
// straddles the boundary and the split stops being bit-identical.
void vg_sky_band_prep(int band_y0);

// The chart, for a RANGE of bands, so the two cores can share it. [b0, b1).
//
// The bands do not depend on each other -- each re-chains its lift from the frame
// accumulator -- so this is safe to call concurrently on disjoint ranges. Call
// vg_sky_prep_begin once first, on one core: it does the frame's shared bank trig, which
// every band reads and neither should be writing.
void vg_sky_prep_begin(void);
void vg_sky_prep_bands(int b0, int b1);

// Pin the sampled view, for vg_sky_bench only. The attract camera never stops turning, so
// without this two checksums a second apart describe two different pieces of sky and cannot
// be compared. See the note at sky_sample.
void vg_sky_bench_pin(bool on);
void vg_sky_fill_rows(uint16_t* band, int band_y0, int r0, int r1);
