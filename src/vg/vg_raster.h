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

// The baked cockpit frame, from the author's drawing via tools/canopy_bake.py. A
// primitive rather than a pass, because WHERE it lands in the order is the point: over
// the world, under the instruments. It applies the drawing as a CHANGE to the finished
// picture, so the frame lights what is behind it rather than painting over it.
void vg_canopy_prim(void);

// THE FRAME FLEXING WITH THE THROTTLE. `k` is 0..1; 0 is rigid.
//
// Cheap because the table is runs, and a run moves by moving its endpoints -- two table reads
// and an add per block, nothing per pixel. Set it once per frame before the flush, from the
// same throttle the instruments' warp uses, so the frame and the panel mounted on it move
// together. See CANOPY_WARP_* in cfg_hud.h and the note at its definition.
void vg_canopy_warp(float k);

// THE FRAME TRAILING THE SHIP. `turn` is the cosmetic bank, and the offset comes from how fast
// it is CHANGING -- so the frame swings during the onset and the release of a turn and sits
// centred through a steady one. Call once a frame, before the flush. Costs one index add per
// panel row. See CANOPY_LAG_* in cfg_hud.h.
// `scale` is the airframe's own shake character -- ShipSpec::shake, which is already 1.70 on a
// CHARIOT and 0.55 on a BALLISTA. The frame of a light hull should be visibly looser than the
// frame of a heavy one, and that ordering is already tuned, so this rides it rather than
// keeping a second table that could disagree with it.
void vg_canopy_lag(float yaw, float pitch, float roll, float scale);

// THE COCKPIT COMING ONLINE, at the top of a match.
//
// The view opens black, and it arrives a REGION at a time: the whole region flashes white, the
// world dissolves out of that white, and the frame's members in it run hot and cool to their
// authored level. The order is the artist's, painted into the green channel of the drawing and
// baked as a zone per block -- see tools/canopy_bake.py and CANOPY_INTRO_* in cfg_hud.h.
//
// `reset` when a match is BUILT and `begin` when the player takes the seat. Those are NOT the
// same moment and the distinction is load-bearing: a match is built from enter_intro, at the top
// of the cutscene, seconds before there is a cockpit at all.
//
// `update` once a frame with the frame's dt, before the flush. It returns true while the sequence
// is still running, so a caller can hold whatever it wants held; it runs on its own clock and does
// not need the world stopped -- and it looks better if the world is live behind it.
//
// `cued` is a LATCH, set once the RUNNING sequence reaches CANOPY_INTRO_HUD_AT, and it is what the
// instruments hang off so the boot chain follows the cockpit rather than a clock of its own. It has
// to be a latch and not a progress comparison, because the state cannot be inferred from a timer:
// vg_hud_decay is reached from vg_world_step, which the attract loop, the title and the cutscene
// all drive. Inferred from a progress value that reads 1.0 when nothing is running, the condition
// is satisfied by default -- and the cockpit's power-on sound played over the title screen.
//
// `lit` is how many regions have come on so far. It is reported rather than acted on because
// what wants it is a sound, and the rasteriser has no business talking to the mixer.
//
// `flex` is the multiplier to apply to BOTH vg_canopy_warp's k and vg_canopy_lag's scale. It is 0
// while the sequence runs, because the world gate reads the unwarped column and a flexing frame
// would put the black somewhere other than where the panel ends, then ramps to 1 so the cockpit
// takes up its resting bulge instead of snapping into it.
// LOOKING AFT. Set once a frame, before the flush.
//
// Suppresses the cockpit frame and nothing else. The canopy is the front of the ship, so
// drawing it over a view out of the back makes the picture contradict itself.
//
// The intro's world gate is NOT suppressed. It shares this primitive, and it is what holds
// the world black while the cockpit arrives -- so turning the primitive off would show the
// whole world at once, mid-sequence.
void  vg_canopy_rear(bool on);

void  vg_canopy_intro_reset(void);
void  vg_canopy_intro_begin(void);
bool  vg_canopy_intro_update(float dt);
bool  vg_canopy_intro_active(void);
bool  vg_canopy_intro_cued(void);
int   vg_canopy_intro_lit(void);
float vg_canopy_intro_flex(void);

// WHAT THE DRAWING COSTS, measured rather than predicted. Serial 'k'.
//
// The canopy only draws inside a match, so the frame counter cannot be read without
// someone at the controls -- and what it reports is one band's slower half plus the
// rendezvous, which no table can be judged against. This runs the whole pass on one core
// and reports it flat, with the counts it got through, so the baker's estimate has
// something to be right or wrong about. See the note at its definition.
// `intro_us` is the cockpit intro's CEILING, with every zone mid-dissolve at once -- which the
// staggered sequence never reaches. It is here so that the intro's dip is a number.
struct VgCanopyCost { uint32_t us, warp_us, intro_us; int blocks, flat_px, lit_px; };
void vg_canopy_bench(VgCanopyCost* out);

// THE BACKDROP, on the same terms. Serial 's'.
//
// `sky` brackets the per-band chart prep, the fill and the rendezvous together, so the
// telemetry cannot say which is expensive. This separates prep from fill and reports a
// checksum of the pixels, because the backdrop has to stay bit-identical through any change
// to that loop -- a replay renders frame for frame.
// `tint_us` is the same fill with the boundary tint ON, which is the one part of the
// backdrop nobody has ever measured: `tnt` on the telemetry line reads 0 unless the ship is
// inside ARENA_TINT_RANGE of a wall, and nobody captures telemetry while about to die.
struct VgSkyCost { uint32_t prep_us, fill_us, tint_us, sum; };
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
uint32_t vg_rast_can_us(void);
uint32_t vg_rast_tint_us(void);

// Tint at the source: ring geometry and colour ops for the boundary tint,
// applied by the sky fill per chunk and by submit per primitive. The
// full-frame pass they replace died of the lit sky -- see vg_band.cpp.
#define VG_TINT_RINGS 12
bool     vg_tint_active(void);
void     vg_tint_row_limits(int sy, int* lim);   // lim[VG_TINT_RINGS + 1]
uint32_t vg_tint_word(uint32_t v, int ring);

uint16_t vg_tint_prim(uint16_t c, float x, float y);

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

// How far a red gradient has closed in from the edge of the screen: 0 is off and
// 1 covers the whole frame. Set once per frame by the render layer, which is what
// knows how close the wall is.
void vg_rast_tint(float k);

// The set turning on and off, as an old tube does it: one bar at the centre that
// grows, not a shutter opening.
//   open  how much of the height is lit, centred -- the bar growing outward
//   wide  how much of the width, for the dot that opens into a line first
//   wash  how far the lit part is still raw white rather than resolved picture
//   dim   how far the rest of it has gone to black
// All 0..1, and (1, 1, 0, 0) is a normal picture. See the note in vg_band.cpp.
void vg_rast_tv(float open, float wide, float wash, float dim);
bool vg_rast_tv_active(void);
