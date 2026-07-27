#pragma once
#include <stdint.h>
#include "vg_config.h"

// Two-stage software rasteriser.
//
// Stage 1 (submit): the renderer projects the scene and calls vg_line/point/
// rect/text. Each call clips against the full screen and appends a screen-space
// primitive to a flat list. Nothing is drawn yet.
//
// Stage 2 (flush): for each horizontal band, clear a band-sized buffer in
// INTERNAL SRAM, draw every primitive overlapping that band into it, and blit.
//
// The point of the split is that the band buffer never lives in PSRAM. Line
// rasterisation is a scattered write pattern, and doing it against PSRAM
// thrashes the cache badly enough to dominate the frame. 57.6 KB of internal
// SRAM buys the whole frame at internal-bus speed instead.

bool vg_rast_init(void);
void vg_rast_begin_frame(void);

// While enabled, every submitted primitive is bent onto a virtual spherical
// surface (see HUD_WARP_K) and subdivided so the curve is actually visible.
// Applied here rather than at the call sites so the whole HUD curves at once
// without a single drawing function knowing about it.
//
// `scale` multiplies HUD_WARP_K, letting the caller drive the amount of bend
// from game state (speed) rather than it being a fixed stylistic constant.
void vg_hud_warp(bool on, float scale);

// Screen space, origin top-left. Off-screen geometry is clipped (and fully
// off-screen geometry dropped) at submit time, so the per-band inner loops
// never see wild coordinates.
void vg_line(float x0, float y0, float x1, float y1, uint16_t color);

// Thick line: `w` parallel 1px lines offset along the screen-space normal.
// Costs w primitives, so keep it for things that must read at a glance.
void vg_line_w(float x0, float y0, float x1, float y1, uint16_t color, int w);
void vg_point(int x, int y, uint16_t color);
void vg_fill_rect(int x, int y, int w, int h, uint16_t color);
void vg_rect(int x, int y, int w, int h, uint16_t color);

// Solid triangle. Used for hidden-line rendering: faces are filled in the
// background colour and the edges drawn over them, so a model occludes both its
// own back edges and whatever is behind it, while still reading as vector art.
void vg_tri(float x0, float y0, float x1, float y1, float x2, float y2, uint16_t color);

// 5x7 bitmap font, `scale` pixels per font pixel, 6*scale advance per glyph.
// ASCII 32..90 (space through 'Z'); lowercase folds to uppercase.
void vg_text(int x, int y, const char* s, uint16_t color, int scale);
int  vg_text_width(const char* s, int scale);

// Rasterise every band and push to the panel.
void vg_rast_flush(void);

int  vg_rast_prim_count(void);
bool vg_rast_overflowed(void);

// Scale an RGB565 colour per channel by f in [0,1]. Used for distance fade and
// for fading debris out.
uint16_t vg_dim(uint16_t c, float f);

// Blend two colours, t=0 -> a, t=1 -> b. Per-channel, so it survives the
// byte-swapped storage. Cheap enough for per-object use, not per-pixel.
uint16_t vg_mix(uint16_t a, uint16_t b, float t);
