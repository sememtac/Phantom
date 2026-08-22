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
// THE vg_hud_ PREFIX NAMES WHAT IS ACTED ON, NOT WHERE THE CODE LIVES, and the
// three functions below are the reason that is worth stating. They warp and shift
// the HUD; they are implemented in vg_raster.cpp because the state they set is the
// per-core rasteriser substate, which is private to that file. None of them is in
// vg_hud.cpp, and vg_hud.cpp's own functions are all called vg_draw_*.
//
// So a grep for vg_hud_ finds these and vg_hud_decay in vg_cockpit.cpp, and finds
// none of the 1000-odd lines that actually draw the panel. That is the cost of the
// convention and it is paid here rather than fixed: moving these three would mean
// publishing Sub and sub() to give two setters a different address, and moving
// vg_hud_warp_at away from warp_pt is exactly what would let the two formulas
// drift -- which the note on it below says must not happen.
// While enabled, every submitted primitive is bent onto a virtual spherical
// surface (see HUD_WARP_K) and subdivided so the curve is actually visible.
// Applied here rather than at the call sites so the whole HUD curves at once
// without a single drawing function knowing about it.
//
// `scale` multiplies HUD_WARP_K, letting the caller drive the amount of bend
// from game state (speed) rather than it being a fixed stylistic constant.
void vg_hud_warp(bool on, float scale);
// Where the bend puts one point at a given scale, jitter excluded and
// independent of the bracket. For a thing that must not be warped itself but has
// to sit where the warped panel would have put it -- the rear-view patch.
void vg_hud_warp_at(float scale, float x, float y, float* ox, float* oy);
// Translate the whole instrument panel by a few pixels. Only affects primitives
// submitted inside the warp bracket, so the world never moves with it. Reset to
// zero when the bracket closes.
void vg_hud_jitter(float dx, float dy);
// Line quality, bracketed the same way as the warp. Antialiasing costs roughly
// an order of magnitude more per pixel than a Bresenham span -- two blends, two
// byte swaps and two bounds tests against a single store -- so it is worth
// paying for instruments, hulls and arena structure, and pointless for dim,
// one-pixel, fast-moving trails where nobody can resolve a step anyway.
//
// Defaults to on; anything that turns it off must turn it back on.
void vg_line_aa_mode(bool on);
// ADDITIVE AND SUBTRACTIVE LINES. `mode` is 2 to add and 3 to subtract (LINE_ADD /
// LINE_SUB); anything else is opaque. While set, a line's colour is a DELTA applied to
// whatever is already in the band, saturating per channel.
//
// This is what lets a panel line be LIT rather than painted: add on the side facing the
// light, subtract on the side in shade, and the frame holds that relationship over a
// dark backdrop and a bright one alike. Costs about what an antialiased pixel costs, so
// it is for structure and instruments and not for the world.
//
// Bracketed like the AA mode: 0 to turn it off, and whatever turns it on turns it off.
void vg_line_blend(int mode);
#define VG_LINE_ADD 2
#define VG_LINE_SUB 3
// Master switch over the above, for judging what the smoothing is actually worth.
// Serial 'q' toggles it.
//
// DEFAULT OFF, and it said "default on" here until somebody asked what turning it off
// would save. The answer was nothing, because it has never been on: s_aa_master is false
// at boot and vg_line_aa_mode(true) is gated on it, so no antialiased line has been drawn
// in any build anybody has measured. A header that describes the opposite of the code is
// worse than no header, and this one had been read and believed.
void vg_rast_aa_master(bool on);
bool vg_rast_aa_master_on(void);
// Screen space, origin top-left. Off-screen geometry is clipped (and fully
// off-screen geometry dropped) at submit time, so the per-band inner loops
// never see wild coordinates.
// Restrict submit-time clipping to a rectangle, in LOGICAL coordinates. Every
// primitive submitted while a viewport is set is clipped to it, so a second
// scene can be drawn into a corner of the screen without touching the first.
//
// Triangles are clipped by their bounding box rather than exactly: a fill whose
// span crosses the edge is trimmed vertically but not horizontally. Good enough
// for the rear-view patch, whose fills are small; worth knowing before using
// this for anything with large faces near an edge.
void vg_rast_viewport(int x, int y, int w, int h);
void vg_rast_viewport_full(void);
// Map a logical rectangle to the panel rectangle it occupies.
void vg_rast_rot_rect(int* x, int* y, int* w, int* h);
// The rear-view patch's backdrop, as a primitive so it draws in order.
void vg_sky_patch_prim(int x, int y, int w, int h);
// THE BACKDROP, on the same terms. Serial 'y'.
//
// `sky` brackets the per-band chart prep, the fill and the rendezvous together, so the
// telemetry cannot say which is expensive. This separates prep from fill and reports a
// checksum of the pixels, because the backdrop has to stay bit-identical through any change
// to that loop -- a replay renders frame for frame.
struct VgSkyCost { uint32_t prep_us, fill_us, sum; };
void vg_sky_bench(VgSkyCost* out);
// THE GLYPH NEST, both ways, over the same fixed page. Serial 'g'.
//
// `gl` cannot answer this on its own: it is a per-frame total and the amount of text on
// screen changes frame to frame, so two captures are two different workloads. This runs the
// plain nest and the hoisted one over identical text in the same build, and compares the
// pixels -- `same` false means they diverged and the change is wrong, whatever it timed.
struct VgGlyphCost { uint32_t ref_us, now_us; int glyphs; bool same; };
void vg_glyph_bench(VgGlyphCost* out);
// THE LINE WALK, both ways, over a fixed fan covering every slope. Serial 'l'.
//
// `ln` is a per-frame total over whatever the scene contained, so two captures are two
// workloads rather than two measurements. `same` false means the walk put a pixel somewhere
// the old one did not, and the timing is irrelevant.
struct VgLineCost { uint32_t ref_us, now_us; int lines; long px; bool same; };
void vg_line_bench(VgLineCost* out);
// THE BLENDED PATH, which is a different loop from the one above and had no bench.
//
// `ref` is what ships. `now` is the same drawing done with px_add/px_sub, the branchless
// four-constant pair the canopy already uses -- held INSIDE the bench and reaching no
// shipping code, so this prices the open target in performance.md without committing to it.
//
// Lines and spans are timed apart because they are different customers with different
// bound checks: a lit HUD line goes through plot_delta and pays three compares a pixel, a
// canopy member is a span fill and pays none. `same` is a memcmp of the two banks over both.
struct VgBlendCost {
    uint32_t line_ref_us, line_now_us;
    uint32_t span_ref_us, span_now_us;
    int      lines;
    long     line_px, span_px;
    bool     same;
};
void vg_blend_bench(VgBlendCost* out);
// Per-type breakdown of the prim stage, and the tint on its own. Diagnostic:
// which KIND of primitive the band raster is spending its time on.
uint32_t vg_rast_aa_us(void);
uint32_t vg_rast_ln_us(void);
uint32_t vg_rast_tri_us(void);
uint32_t vg_rast_oth_us(void);
// ...and `oth` split into its three, because it is a bucket and the three move for entirely
// different reasons: points with speed, glyphs with what the HUD has to say, fills with the
// instruments on screen. Anything left in `oth` after these is a type nobody has claimed.
uint32_t vg_rast_pt_us(void);
uint32_t vg_rast_gl_us(void);
uint32_t vg_rast_fl_us(void);
// The line WORKLOAD, so `ln` can be split into per-line setup and the per-pixel walk. A
// bench cannot pick a representative line length without knowing this one.
uint32_t vg_rast_ln_px(void);
uint32_t vg_rast_ln_n(void);
// The baked canopy alone. Split out of `oth`, which is a bucket that also holds glyphs
// and fills and therefore moves with how busy the fight is.
// WALL CLOCK, all of them. Each is the sum over bands of what the SLOWER core spent on
// that type, so they can be compared with `rast` and with each other. They used to be
// both cores added together, which made `can` plus `prim` exceed the `rast` containing
// them. vg_rast_can_both_us is the old sum, kept for the split's own balance question.
uint32_t vg_rast_can_us(void);
uint32_t vg_rast_can_both_us(void);
// HOW HARD THE WALL WARNING IS RUNNING, 0 to 100. Not a time -- see the note at its
// definition, where it has been three different wrong things.
uint32_t vg_rast_tint_us(void);
// THE RING IS GONE. Its geometry and colour operations lived here: VG_TINT_RINGS,
// vg_tint_active, vg_tint_row_limits, vg_tint_word and vg_tint_prim, applied by the sky
// fill per chunk and by submit PER PRIMITIVE -- every line, point, rect, triangle and
// glyph of every frame asked whether the player was near a boundary.
//
// The warning is the cockpit's own colour table now, which costs nothing per pixel. See
// vg_canopy_alarm. A hull with no drawing has no warning until one is drawn for it, which
// is the author's decision and the reason the ring is deleted rather than kept.
// Hidden-line fills. Off makes vg_tri a no-op; see the note at its definition.
void vg_rast_fills(bool on);
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
// Submit splits in two halves that can be built concurrently -- see the note on
// Sub in vg_raster.cpp. `select` says which half the CALLING CORE is filling;
// `join` closes the frame by making the two slices contiguous in draw order.
// PER-SLICE PEAKS AND CAPS, for sizing the primitive list.
//
// Each slice is its own ceiling, so the whole-frame peak cannot size one. `peak` is the
// high water mark since boot and is never reset -- the number that decides a buffer size
// is the worst moment the game can produce, not the usual one.
int  vg_rast_slices(void);
int  vg_rast_slice_peak(int i);
int  vg_rast_slice_cap(int i);
void vg_prim_select(int group);
void vg_prim_join(void);
int  vg_rast_prim_count(void);
// The most primitives any one frame has used since boot. Sizes the list -- see the
// note at its definition for what it is being measured for.
int  vg_rast_prim_peak(void);
bool vg_rast_overflowed(void);
// Frame cost, split. `blit` on its own conflates two very different things: CPU
// spent rasterising bands, and time stalled waiting on the panel DMA. Only the
// first is ours to optimise, and only the amount by which it EXCEEDS the DMA
// window costs frame time at all.
uint32_t vg_rast_raster_us(void);
uint32_t vg_rast_wait_us(void);
// The third piece, and the one that decides whether any of the rest is worth
// touching: time blocked inside the band push, waiting on the previous
// transfer. High `push` means the panel gates the frame and no amount of CPU
// parallelism will help; low `push` with `over` bands means the raster is
// spilling out of the transfer window and there is something to win.
//
// `over` counts the bands whose CPU exceeded one band's DMA window and `over_us`
// sums the excess. That sum is the CEILING on what splitting band work across
// cores can return -- an upper bound, since a split band still pays its own
// window and the rendezvous.
uint32_t vg_rast_push_us(void);
int      vg_rast_over_bands(void);
uint32_t vg_rast_band_window_us(void);
// The canopy split, taken apart: this core's half, its wait, and where it cut.
uint32_t vg_rast_canhalf_us(void);
uint32_t vg_rast_canwait_us(void);
int      vg_rast_can_split(void);
uint32_t vg_rast_over_us(void);
// This frame's raster cost per band, NUM_BANDS entries, for the SHAPE of the
// overrun. A total cannot tell an even overshoot -- which no scheduling trick
// reaches -- from two heavy bands holding the ships, which is a different and
// much more tractable problem. Valid until the next flush.
const uint32_t* vg_rast_band_us(void);
// The rest of the flush: closing the primitive list, and everything the four
// brackets do not cover. `res` is mostly preemption, so it is a load signal
// rather than a thing to optimise -- but it kept blit from adding up, and a
// counter that does not add up is not finished.
uint32_t vg_rast_join_us(void);
uint32_t vg_rast_res_us(void);
// The join's memmove on its own, and how many primitives it moved -- see the note
// at vg_prim_join for why the caller's bracket is not to be trusted.
uint32_t vg_rast_join_mm_us(void);
int      vg_rast_join_n(void);
// ...and the raster half split by stage, because the primitive COUNT cannot
// distinguish a long antialiased span from a triangle fill covering a third of
// the screen, and neither shows against a fixed backdrop cost.
uint32_t vg_rast_sky_us(void);
uint32_t vg_rast_prim_us(void);
uint32_t vg_rast_scan_us(void);
int      vg_rast_tri_count(void);
// Scale an RGB565 colour per channel by f in [0,1]. Used for distance fade and
// for fading debris out.
uint16_t vg_dim(uint16_t c, float f);
// Blend two colours, t=0 -> a, t=1 -> b. Per-channel, so it survives the
// byte-swapped storage. Cheap enough for per-object use, not per-pixel.
uint16_t vg_mix(uint16_t a, uint16_t b, float t);
// The set turning on and off, as an old tube does it: one bar at the centre that
// grows, not a shutter opening.
//   open  how much of the height is lit, centred -- the bar growing outward
//   wide  how much of the width, for the dot that opens into a line first
//   wash  how far the lit part is still raw white rather than resolved picture
//   dim   how far the rest of it has gone to black
// All 0..1, and (1, 1, 0, 0) is a normal picture. See the note in vg_band.cpp.
// The broadcast transition is vg_tv.h now, as vg_tv_set and vg_tv_active. It was declared
// here because it was defined in vg_band.cpp; both moved together.

// THE CANOPY'S DRAWING API MOVED to vg_canopy_draw.h, with the code. It was declared here
// because it was defined in vg_band.cpp, which is this header's module -- the same reason
// vg_sim.h once declared nine modules' worth of functions. vg_canopy.h is still the DATA
// object; vg_canopy_draw.h is the thing that draws it.
