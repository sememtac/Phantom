#include "vg_canopy_draw.h"
#include "vg_raster.h"
#include "vg_raster_int.h"
#include "vg_port.h"
#include "vg_canopy.h"
#include "vg_config.h"
#include "vg_sim.h"   // vg_frand01
#include <Arduino.h>
#include <string.h>
#include <math.h>
#include "vg_cockpit.h"

// ===========================================================================
// THE CANOPY, LIFTED WHOLE OUT OF vg_band.cpp
//
// One drawing, one colour table, one warp, one coming-online sequence, and the row pass
// over all of it. It was 1,112 lines in the middle of the raster, and the map called it a
// system trapped inside another one.
//
// WHY WHOLE AND NOT IN PIECES. The intro alone was tried and abandoned: it would have
// forced s_can_lut, canopy_lut(), s_can_ready and s_intro_on into public view to move 311
// lines, which is publishing a module's internals to relocate part of it. Taken whole the
// boundary falls where the coupling already stops, and three functions cross it --
// vg_canopy_rows, vg_canopy_split_at, and the API that was always public.
//
// AND IT COSTS WHAT THE BACKDROP COSTS. The row-split task already dispatched across a
// translation unit to vg_sky_fill_rows and vg_sky_prep_bands, so a canopy on the far side
// of one is not a new shape. The per-band call is the only thing that crosses; the span
// loops, the block walk and px_add are all still in one unit with each other, which is
// what the register work in 2026-08-04 was protecting.
// ===========================================================================

// HOW RED THE COCKPIT IS, 0 clear and 1 hard against the wall. The only wall warning there
// is now: the ring that used to sit beside this is deleted, above.
static float s_alarm  = 0.0f;

// ===========================================================================
// THE BAKED CANOPY
//
// The author draws the frame on a grey field and this applies it as a CHANGE to the
// finished picture: brighter than the background adds light, darker takes it away. So
// the frame is lit BY the scene rather than painted over it, and it holds that
// relationship over a dark nebula and a bright one alike -- which is exactly what an
// opaque line cannot do, and what the sky gamma made obvious.
//
// COLUMNS, because the panel is turned a quarter turn: panel_x is the picture's y and
// panel_y is 479 minus its x, so one band is 32 columns of the drawing and a column
// layout keeps each band's data contiguous. LEFT HALF ONLY -- the drawing is symmetric
// to within four levels of antialiasing noise, so column x serves 479-x as well and
// nothing needs flipping inside a run, since mirroring in x leaves the y-runs alone.
//
// Empty pixels are not stored: a frame covers ~7% of the screen, so the table is runs
// of used pixels and skips the rest. 9.5 KB in FLASH, where there are megabytes free,
// rather than the 21 KB of internal SRAM that is actually scarce.
//
// The grey levels come through a 256-entry table built once, so the per-pixel work is
// a load and a saturating add rather than any arithmetic on the hue.
static uint16_t s_can_lut[256];
static bool     s_can_ready = false;

// WHICH DRAWING IS BEING FLOWN.
//
// It used to be the CANOPY_* macros, read straight out of the generated header, and that
// shape allowed exactly one cockpit to exist -- a macro cannot be selected at runtime. Every
// hull can have its own now. Nothing is owned or copied: a canopy lives in flash and this
// points at it.
//
// NULL UNTIL SELECTED, and this file no longer includes a drawing at all.
//
// It cannot: a generated header defines its tables as `static`, so including one here and
// again wherever the per-hull table lives would put two copies of every drawing in flash.
// vg_canopy_set.cpp owns them; this holds a pointer it is handed.
//
// Which means every entry point has to tolerate not having one yet. vg_game_init selects at
// boot so it never happens in practice, but "never happens in practice" is not something to
// dereference a pointer on -- and a missing cockpit should be a missing cockpit, not a crash.
static const VgCanopy* s_can = nullptr;
// The panel being looked through, worked out from the zone map when a drawing is selected
// and never faulted. Beside s_can because it is a property OF the drawing -- see
// canopy_find_centre.
static int8_t s_centre_z = -1;

// IS THE ARRIVAL SEQUENCE RUNNING. Declared HERE, beside the drawing, and not down with the
// rest of the intro's state where it used to live -- because the two are coupled and the
// coupling is a safety property: canopy_intro_step reads s_can->zones under this flag alone,
// so a true flag and a null drawing is a fault. vg_canopy_use is the only place that can
// break that pair, and it is a few lines below.
static bool s_intro_on = false;

// LOOKING AFT, so the cockpit must not be drawn.
//
// The canopy is the front of the ship. Draw it over a view out of the back and the picture
// says two things at once: the frame claims you are looking forward while everything inside
// it is behind you. From a playtest, and it is a continuity fault rather than a cosmetic one.
//
// It suppresses the FRAME ONLY, not the intro's world gate. Those share one primitive, and
// the gate is what holds the world black region by region while the cockpit comes online --
// so switching the whole primitive off in rear view would show the entire world at full
// brightness in the middle of the sequence. That is the same trap that lifting this
// primitive out of vg_draw_hud was fixing.
static bool s_can_rear = false;

void vg_canopy_rear(bool on) { s_can_rear = on; }

const VgCanopy* vg_canopy_current(void) { return s_can; }

// THE WALL WARNING, ON THE COCKPIT INSTEAD OF ON THE VIEW.
//
// The ring tint is a per-pixel pass over its own area and costs about 1100 us at the wall,
// measured. This costs NOTHING per pixel: the cockpit's members are already written every
// frame, and their colour comes from a 256-entry table, so turning the frame red is a
// different table rather than more work.
//
// Quantised to sixteen steps so a steady approach rebuilds the table a few times instead
// of sixty a second. The table is 256 entries of trivial arithmetic, so even that is
// nearly free -- the quantising is about not thrashing s_can_ready.
// s_alarm is declared further up, where the ring's own level used to sit beside it.
static int   s_alarm_q = 0;
static bool  s_alarm_white = false;

// TWO THINGS, because they are two different signals and the first attempt conflated them.
//
// `k` is how red the frame is, from the clearance. `white` is a STROBE: the frame goes to
// white for a fraction of a second and comes back. The author's words, after flying the
// first version and not seeing it: "the flashing i have in mind is not the canopy flashing
// its opacity, rather, it's the color flashing from split second of white to the current
// tint".
//
// That is why the first attempt was invisible. It swung the red LEVEL between a floor and
// full, and vg_mix(COL_HUD, COL_DANGER, k) cannot reach white at either end -- so the whole
// effect was a slight change of saturation on an already-red frame, which is precisely the
// "which shade means I am dead" problem it was meant to solve.
//
// The strobe is a bool and the level is quantised, so the table rebuilds on either edge of
// a flash and on a step of the ramp. At the top rate that is about eighteen rebuilds a
// second of 256 trivial entries, which is nothing beside a per-pixel pass.
void vg_canopy_alarm(float k, bool white) {
    if (k < 0.0f) k = 0.0f; else if (k > 1.0f) k = 1.0f;
    const int q = (int)(k * 16.0f + 0.5f);
    if (q == s_alarm_q && white == s_alarm_white) return;
    s_alarm_q     = q;
    s_alarm       = (float)q * (1.0f / 16.0f);
    s_alarm_white = white;
    s_can_ready   = false;        // the colour table is stale; canopy_lut rebuilds it
}

static void canopy_lut(void) {
    const int bg = (int)s_can->bg;
    // What the frame is made of right now: its own amber, pulled toward the danger red as
    // the wall closes. One mix for the whole table rather than per pixel.
    // The frame's own amber, pulled toward the danger red by the clearance, and then all
    // the way to white for the length of a strobe. One mix for the whole table rather than
    // anything per pixel.
    uint16_t base = (s_alarm > 0.0f) ? vg_mix(COL_HUD, COL_DANGER, s_alarm) : COL_HUD;
    if (s_alarm_white) base = CANOPY_ALARM_WHITE;
    for (int g = 0; g < 256; g++) {
        const float f = (g > bg)
                      ? (float)(g - bg) / (float)(255 - bg)
                      : (float)(bg - g) / (float)bg;
        // NATIVE order, swapped once here. Palette colours are stored in panel order
        // and blend_px works in native, so leaving this unswapped would put the delta's
        // red into the blue channel -- a bug that would have looked like a design
        // decision rather than a mistake.
        const uint16_t c = vg_dim(base, f);
        s_can_lut[g] = (uint16_t)((c >> 8) | (c << 8));
    }
    s_can_ready = true;
}

// A RUN OF ONE DELTA, which is where the cost of this actually lives.
//
// Two masked adds instead of three channel extracts. R+B share a word because nothing
// can carry between them once G is masked out, and each field gets a spare bit above it
// to catch the overflow: bit 16 above R, bit 5 above B, bit 11 above G. If the guard bit
// is set the field saturated, so fill it -- no compares against 31 and 63, no
// per-channel shifting back and forth.
//
// Subtraction is the same trick run backwards: set the guard bits first, and a field
// that borrows through its guard has underflowed and clamps to zero.
//
// The delta is hoisted out of the loop, which is the other half of the win and the
// reason the table stores runs of one level rather than a level per pixel.
// A GUARD BIT FILLS ITS OWN FIELD, which is what makes this branchless without costing
// more than the branches did.
//
// For a field of width w with its guard bit immediately above it, `m - (m >> w)` turns a
// set guard into a solid field of ones and a clear guard into nothing. Red sits at bits
// 11..15 under a guard at 16, so 0x10000 - 0x800 is 0xF800: exactly the red field. Blue
// at 0..4 under bit 5 gives 0x20 - 1 = 0x1F. Both guards at once still work, because the
// shifted fields do not overlap. Green is width 6 under bit 11, so it shifts by 6.
//
// The point is not the two cycles of branch. It is that the branchy form needed seven
// mask constants -- 0x1F, 0xF800, 0x7E0 and their complements -- and sixteen registers
// could not hold them alongside two interleaved pixels. The compiler was reloading them
// from the stack and fetching ~0xF800 out of the literal pool EVERY iteration. Four
// constants fit; seven did not.
//
// The input is not masked to 16 bits: every path below masks with 0xF81F or 0x07E0
// anyway, so the high bytes the swap leaves behind cannot reach the result.

// Subtraction is the same trick run backwards: set the guards first, and a field that
// borrowed through its guard has underflowed. Here a STILL-SET guard is the good case, so
// the same expression becomes a keep-mask and clears the guard on its way out -- which is
// why this one needs no final masking at all.

// TWO PIXELS PER ACCESS, because this loop is bound by memory and not by arithmetic.
// Taking eight cycles of maths out of it earlier changed nothing at all -- the work was
// already hidden behind the load and the store -- so the only thing left to remove is
// the number of accesses. One 32-bit load and one 32-bit store per pair halves them.
//
// TWO MORE ATTEMPTS HAVE SINCE BEEN MEASURED AND REJECTED, both over the same recorded
// session with tools/replay_cost.py, whose noise floor on this counter is one microsecond:
//
//   The byte swaps lifted out of px_add and done once per PAIR instead of once per pixel.
//   Seven operations of about twenty-two are swapping, so this looked like the obvious
//   win. It cost 32 us, +1.1%. The compiler was already folding the swap into the field
//   masks better than the hand-written version could.
//
//   The whole flight table copied from flash into internal SRAM, on the theory that 11 KB
//   read through the cache every frame was the missing time. It saved 3 us, -0.1%, for
//   11 KB of the scarcest memory on the part.
//
// So the 36 cycles a flat pixel costs are neither the arithmetic nor the table: they are
// the read-modify-write of the band itself. That is the floor, and the only way under it
// is to touch fewer pixels. AREA is the lever, and it belongs to the artist, not to this
// file -- which is what the estimate in tools/canopy_bake.py has always said. It now has
// three measurements behind it instead of an assertion.
//
// The pixel maths stays per-pixel on purpose. Packing both into one 32-bit add would
// need a spare bit above red to catch its carry, and red sits at the top of its half,
// so the carry lands in the neighbouring pixel's blue. That is a real trap and not
// worth the two cycles it would save.
//
// Xtensa will not do an unaligned 32-bit load, so a span starting on an odd pixel does
// that one singly first.
#define SPAN_BODY(FN)                                                        \
    const uint32_t drb = (uint32_t)d & 0xF81Fu, dg = (uint32_t)d & 0x07E0u;  \
    int i = 0;                                                               \
    if (n > 0 && (((uintptr_t)q & 3u) != 0u)) {                              \
        q[0] = (uint16_t)FN(q[0], drb, dg);                                  \
        i = 1;                                                               \
    }                                                                        \
    for (; i + 1 < n; i += 2) {                                              \
        uint32_t* w = (uint32_t*)(void*)(q + i);                             \
        const uint32_t v = *w;                                               \
        *w = FN(v & 0xFFFFu, drb, dg) | (FN(v >> 16, drb, dg) << 16);         \
    }                                                                        \
    for (; i < n; i++) q[i] = (uint16_t)FN(q[i], drb, dg);

// ALWAYS INLINE, and not as a matter of taste.
//
// These were inlined by choice of the compiler, and taking them OUT of line was measured at
// 175 us of loss when it was tried on purpose -- a flat block averages twenty pixels, which is
// not enough to pay for a call. Then the warp grew canopy_rows_t past whatever size threshold
// GCC weighs, and it stopped inlining them on its own: the rigid instantiation came out at 234
// bytes calling span_sub, and the pass went 4065 -> 4190 us.
//
// So the decision is stated rather than left to a heuristic that cannot know what it costs
// here. A measurement that has already been taken should not have to be taken again because an
// unrelated function got longer.
static inline __attribute__((always_inline))
void span_add(uint16_t* q, int n, uint16_t d) { SPAN_BODY(px_add) }
static inline __attribute__((always_inline))
void span_sub(uint16_t* q, int n, uint16_t d) { SPAN_BODY(px_sub) }

// A PIXEL AT A TIME, for the antialiased edge of a shape.
//
// The flat span above is the cheap path and carries most of the area, but it earns that
// by hoisting the delta, which only works where the level is constant. At the edge of a
// member every pixel is a different level, and stored as flat spans those were 78% of
// all the blocks while carrying 18% of the pixels -- four bytes of header to deliver one
// byte of level, and a header decode is worth several pixels.
//
// So a stretch of them is one block with a level per pixel. The delta comes back inside
// the loop, but the pairing survives: two pixels in one 32-bit access still works when
// their deltas differ, because the maths is per-pixel either way.
#define SPAN_LIT_BODY(FN)                                                    \
    int i = 0;                                                               \
    if (n > 0 && (((uintptr_t)q & 3u) != 0u)) {                              \
        const uint32_t d = lut[lv[0]];                                       \
        q[0] = (uint16_t)FN(q[0], d & 0xF81Fu, d & 0x07E0u);                 \
        i = 1;                                                               \
    }                                                                        \
    for (; i + 1 < n; i += 2) {                                              \
        const uint32_t d0 = lut[lv[i]], d1 = lut[lv[i + 1]];                 \
        uint32_t* w = (uint32_t*)(void*)(q + i);                             \
        const uint32_t v = *w;                                               \
        *w = FN(v & 0xFFFFu, d0 & 0xF81Fu, d0 & 0x07E0u)                     \
           | (FN(v >> 16, d1 & 0xF81Fu, d1 & 0x07E0u) << 16);                \
    }                                                                        \
    for (; i < n; i++) {                                                     \
        const uint32_t d = lut[lv[i]];                                       \
        q[i] = (uint16_t)FN(q[i], d & 0xF81Fu, d & 0x07E0u);                 \
    }

// NOT INLINED, and that is the whole point of it.
//
// Inlined, all four span variants and the block walk are live in one function at once,
// and Xtensa has sixteen visible registers. The compiler ran out: the disassembled pair
// loop was 79 instructions for two pixels, and among them were three stack reloads, two
// rematerialised constants and a literal-pool fetch -- every iteration. The arithmetic
// was never the cost; the spilling was.
//
// Out of line, this loop gets the register file to itself. A call costs a handful of
// cycles once per block, and a block averages thirty-odd pixels.
// A STRETCHED literal block, resampled rather than over-read.
//
// A literal block carries one level byte per DRAWING pixel. Magnified, its panel length is
// longer than its data, and the plain span above would read past the end of its own levels
// into the next block's -- which are frequently much brighter. That is the bright pixels
// bleeding through the frame at full zoom: not a blend fault, an array read one block too far.
//
// Fixed point, 16.16: `step` is drawing pixels per panel pixel and `i0` is where the clipped
// front starts. Only the graded edges take this path and only while the warp is on, so it
// stays a plain per-pixel walk -- pairing it would save little and this is 4,792 pixels.
#define SPAN_LIT_RS_BODY(FN)                                                     uint32_t idx = i0;                                                           for (int i = 0; i < n; i++) {                                                    const uint32_t d = lut[lv[idx >> 16]];                                       q[i] = (uint16_t)FN(q[i], d & 0xF81Fu, d & 0x07E0u);                         idx += step;                                                             }

// THE TABLE COMES IN AS AN ARGUMENT, because the intro needs a different one per zone.
//
// It was a file static, read straight from the macro bodies. The intro flashes a region white
// by giving that region its own colour table, so the table has to vary per BLOCK -- and these
// are shared with the flight path, which must not pay for it. As an argument it does not: the
// call already passes three registers and the loop reads through a register base rather than a
// literal address, which on Xtensa is no worse. Measured on the canopy bench before and after.
static __attribute__((noinline))
void span_lit_add_rs(uint16_t* q, int n, const uint8_t* lv, const uint16_t* lut, uint32_t i0, uint32_t step)
{ SPAN_LIT_RS_BODY(px_add) }
static __attribute__((noinline))
void span_lit_sub_rs(uint16_t* q, int n, const uint8_t* lv, const uint16_t* lut, uint32_t i0, uint32_t step)
{ SPAN_LIT_RS_BODY(px_sub) }

static __attribute__((noinline)) void span_lit_add(uint16_t* q, int n, const uint8_t* lv, const uint16_t* lut) { SPAN_LIT_BODY(px_add) }
static __attribute__((noinline)) void span_lit_sub(uint16_t* q, int n, const uint8_t* lv, const uint16_t* lut) { SPAN_LIT_BODY(px_sub) }

// THE STEP WITHOUT THE DIVIDE, which is the one division the block walk still does.
//
// About 1,650 of them a frame, and only sixty distinct (len, n0) pairs among the lot: a graded
// edge is a few pixels wide and the magnification is small, so the same short block stretched
// by the same amount recurs the whole way down the drawing. Four in five fall inside len <= 8
// and n0 <= 12, and 384 bytes of flash answer those without the divider.
//
// The entries ARE what the divide produces, truncation included, so the table and the fallback
// cannot disagree about a block -- and they must not, because this indexes the level array a
// literal block reads per pixel.
#define CANOPY_STEP_L  8u
#define CANOPY_STEP_N  12u
struct CanopyStepTab { uint32_t v[CANOPY_STEP_L][CANOPY_STEP_N]; };
static constexpr CanopyStepTab canopy_step_build(void) {
    CanopyStepTab t = {};
    for (uint32_t l = 1; l <= CANOPY_STEP_L; l++)
        for (uint32_t n = 1; n <= CANOPY_STEP_N; n++)
            t.v[l - 1][n - 1] = (l << 16) / n;
    return t;
}
static constexpr CanopyStepTab s_steptab = canopy_step_build();

// Rows [r0, r1) of the band, so the pass can be halved across the cores. Each panel
// row is an independent column of the drawing -- nothing carries between them -- which
// is what makes this splittable at all, and it is by far the biggest of the three
// row-split jobs: measured at ~4 ms against the backdrop's 2.5 and the scanlines' 1.8.
// ===========================================================================
// THE SPHERE, the same one the instruments are drawn on.
//
// A radial warp is NOT separable: k = 1 + K*scale*r2 scales both axes by an amount that
// depends on both coordinates, so a y-map plus a column offset -- which is what this was --
// can only ever produce a shear and a zoom. It read as the frame growing and bending rather
// than bulging.
//
// Done properly in two halves, because the renderer iterates panel ROWS and can only lay runs
// along panel x within one:
//
//   the y axis is EXACT. dx is fixed for the whole column, so dx*dx hoists and each block
//   endpoint costs a subtract, two multiplies, two adds and a round -- about seven operations.
//   Both endpoints go through it and the length is the difference, so runs still touch.
//
//   the x axis is an inverse. For each output row, which drawing column lands here -- built by
//   walking the forward map, which is monotone for any |K| the panel uses. Evaluated at the
//   screen's own centre line, so a point's sideways displacement is taken as constant down its
//   column. That is the one approximation, and it is the same one the rear-view patch makes.
//
// Rebuilt only when the quantised amount changes, so the 480 forward evaluations and the
// inversion are paid a handful of times across a throttle sweep, not per frame.
static float   s_w_zk   = 0.0f;    // zoom * K * scale / R2
static float   s_w_zoom = 1.0f;
static float   s_w_zbase[SCR_H];   // zoom + zoom*K*scale*dx^2/R2, per SOURCE column
static int16_t s_wc[SCR_H];        // the bow's per-column shift, laid on top of the sphere
// WHICH DRAWING COLUMN A PANEL ROW SAMPLES, which is the other half of coming closer.
//
// Stretching the drawing's y alone made the frame taller, not nearer. A magnification needs
// both axes, and the other axis IS the column index -- so it is a lookup per panel row, 480 a
// frame, and free. Inverse-mapped because this samples rather than places: a frame magnified
// by s puts drawing column d at panel position cx + (d-cx)*s, so the panel row at p wants
// cx + (p-cx)/s.
//
// Nearest neighbour, so at a magnification of a tenth roughly every tenth column is read
// twice. On a cockpit frame that reads as the members thickening slightly, which is what
// something approaching does anyway. -1 means this row is off the drawing entirely.
static int16_t s_wcol[SCR_H];

// WHERE A WARPED BAND BALANCES, which the baked table cannot know.
//
// VgCanopy::split is computed by the baker from where the drawing's work falls, and it is right
// only while each panel row reads its own column. Warping breaks that: the inverse column map
// duplicates some columns and skips others, so the work slides along the band and the baked
// balance point stops being the middle of it. A band costs whichever half is slower, so the
// error is paid in full and it grows with ZOOM -- which is the setting the look depends on.
//
// Rebuilt with the maps, from the same per-column costs the baker used. s_colcost is walked
// out of the table once, on the first warp.
static uint8_t  s_wsplit[NUM_BANDS];
static uint16_t s_colcost[SCR_H];
static bool     s_colcost_ready = false;

// WHICH COLUMNS CAN REACH A BORDER AT ALL, and it is not a runtime question.
//
// A border is extended only where the drawing already reached it, and whether it did is a
// property of the BAKED column: the first block either starts at the drawing's zero or it does
// not, and the last block either runs to the far side or stops short. The warp MOVES those
// runs; it cannot create them. So the answer holds for the life of the drawing, and
// canopy_edges was asking it 480 times a frame -- walking the whole block list to find the last
// block and then throwing the walk away.
//
// Five columns of the 240 have a first block at zero and fourteen have a last one reaching the
// far side. The other 92% walked the list to draw nothing.
//
// Two bits and the last block's offset, built in the cost walk because that walk already reads
// every block of every column -- the same trade s_colmask makes against the zone map.
#define CANOPY_EDGE_NEAR  1u
#define CANOPY_EDGE_FAR   2u
static uint8_t  s_coledge[SCR_H];
static uint16_t s_colend[SCR_H];   // byte offset of the column's last block

static void canopy_colcost(void) {
    for (int c = 0; c < SCR_H; c++) {
        uint32_t cost = 0;
        uint8_t  edge = 0;
        uint16_t last = 0;
        if (c < (int)s_can->cols) {
            const uint8_t* p = &s_can->data[s_can->ofs[c]];
            const uint8_t* e = &s_can->data[s_can->ofs[c + 1]];
            int lastend = -1;                   // negative until a block has been seen
            while (p + 3 <= e) {
                const uint8_t h = p[0];
                const int y0  = ((h & 1) << 8) | p[1];
                const int len = ((h & 2) << 7) | p[2];
                if (lastend < 0 && y0 == 0) edge |= CANOPY_EDGE_NEAR;
                lastend = y0 + len;
                last    = (uint16_t)(p - s_can->data);
                p += 3 + ((h & 0x80) ? len : 1);
                cost += (uint32_t)len + 1;      // a header is worth about one pixel
            }
            if (lastend >= SCR_W) edge |= CANOPY_EDGE_FAR;
        }
        s_colcost[c] = (uint16_t)(cost > 0xFFFFu ? 0xFFFFu : cost);
        s_coledge[c] = edge;
        s_colend[c]  = last;
    }
    s_colcost_ready = true;
}
static int     s_wq = -1;                 // the quantised amount the maps were built for
static bool    s_warp_on = false;

// SELECTING A DRAWING, and it is down here rather than beside s_can because of what it has to
// invalidate: three caches, two of which are the warp's and are declared just above.
//
// All three are derived from the drawing and none of them can survive a change of it. The
// colour table is built from `bg`, which is per drawing -- two cockpits with different
// background levels turn the same stored grey into different amounts of light, so a stale
// table draws the new one at the wrong brightness. The per-column costs and the warp maps are
// built from where the drawing's work falls, which is the whole point of them.
// NULL IS A HULL WITH NO COCKPIT, and that is a supported state rather than a mistake.
//
// This used to ignore null and keep whatever was selected last, on the reasoning that a wrong
// cockpit is something an artist can see where an empty frame just looks broken. That was
// written when a default drawing was assumed. There is no default: a canopy is authored per
// hull, the reference drawing belongs to the CHARIOT, and the other three fly without one
// until somebody draws them.
//
// Everything downstream already tolerates it -- canopy_rows, the warp, the bench and the
// PRIM_CANOPY case all return early on a null drawing -- so what was missing was only the
// ability to SAY none.
void vg_canopy_use(const VgCanopy* c) {
    if (c == s_can) return;
    s_can           = c;
    s_centre_z      = -1;   // worked out lazily; this drawing's panels are elsewhere
    s_can_ready     = false;
    s_colcost_ready = false;
    s_wq            = -1;
    // A SEQUENCE CANNOT BE RUNNING AGAINST A DRAWING THAT IS GONE.
    //
    // canopy_intro_step reads s_can->zones and is guarded only by s_intro_on, so leaving the
    // flag set while selecting nullptr arms exactly the fault that vg_canopy_intro_reset just
    // had to be fixed for. No caller does that today, and no caller should have to know not to.
    if (!c) s_intro_on = false;
}

// THE FRAME LAGS THE SHIP, which is what makes it read as being inside something.
//
// A pixel offset from the DIFFERENCE between the turn now and a smoothed copy of it, not from
// the turn itself. Displacement then appears while the turn is starting and stopping and
// settles back to nothing through a steady one -- inertia rather than a permanent lean, which
// would just look like the drawing is off centre.
//
// Whole pixels, so it quantises itself like everything else here, and it moves the COLUMN the
// row samples: under the quarter turn a bank is movement along logical x, and logical x is the
// drawing's column. One index add per row.
// ALL THREE AXES, and each is a different shape of offset.
//
// Under the quarter turn the drawing's x is a panel COLUMN and its y is a panel row's x, so:
//
//   yaw   moves the frame along logical x  -> the column each row samples. One index add.
//   pitch moves it along logical y         -> every block's start. One add, per column.
//   roll  is a rotation, which no single offset can be -- but a small rotation about the
//         centre is a SHEAR, and a shear is a per-column offset that grows with the column.
//         So it is the same add as pitch with a term in c. One multiply per column, none per
//         block and none per pixel.
//
// Each rides the difference between the command and a smoothed copy of it, so the frame swings
// as a movement starts and stops and sits still through a held one. A canopy that stayed
// displaced through a sustained roll would be a canopy coming loose from the ship.
// A MASS ON A SPRING, not a difference.
//
// It used to be (command - smoothed) * gain. That eases on the way IN and cannot ease on the
// way out: lifting a finger drops the command to zero in one frame, the smoothed copy has not
// moved, so the difference -- and the frame with it -- jumps to its extreme instantly. Reported
// as the canopy snapping back and the ship losing its weight, which is exactly what a
// first-order difference does. The offset was a function of the instantaneous command, so any
// step in the command was a step in the picture.
//
// Now the displacement is STATE. The command's rate of change drives it, a spring pulls it
// home and damping settles it, so nothing the player does moves the frame in one frame -- only
// its acceleration changes. Releasing the stick gives an equal and opposite kick and the frame
// coasts back at the spring's own pace, overshooting slightly, which is what weight looks like.
//
// SPRING sets how long the return takes -- 0.030 is a period of about 36 frames, six tenths of
// a second. DAMP sets how much it overshoots: this pair is a damping ratio near 0.64, so it
// comes back with one soft rebound rather than ringing or arriving dead.
static float s_lag_q[3]  = { 0.0f, 0.0f, 0.0f };   // last command, per axis
static float s_lag_v[3]  = { 0.0f, 0.0f, 0.0f };   // velocity
static float s_lag_x[3]  = { 0.0f, 0.0f, 0.0f };   // displacement
static int   s_lag_px = 0;         // columns, from yaw
static int   s_lag_py = 0;         // pixels along the drawing's y, from pitch
static float s_lag_sh = 0.0f;      // pixels of y per column, from roll

void vg_canopy_lag(float yaw, float pitch, float roll, float scale) {
    const float cmd[3] = { yaw, pitch, roll };
    float out[3];
    for (int i = 0; i < 3; i++) {
        const float a = cmd[i] - s_lag_q[i];       // the ship's angular acceleration, near enough
        s_lag_q[i] = cmd[i];
        s_lag_v[i] += a * CANOPY_LAG_DRIVE
                    - s_lag_x[i] * CANOPY_LAG_SPRING
                    - s_lag_v[i] * CANOPY_LAG_DAMP;
        s_lag_x[i] += s_lag_v[i];
        out[i] = s_lag_x[i];
    }

    // The CLAMP scales with the airframe too. Scaling only the swing would have every hull
    // reach the same limit and arrive there at a different speed, which flattens exactly the
    // difference this is for -- a CHARIOT would clip where a BALLISTA never gets close.
    const float lim  = CANOPY_LAG_MAX * scale;
    const float gain = CANOPY_LAG_PX * scale;

    float dx = out[0] * gain;
    float dy = out[1] * gain;
    float dr = out[2] * gain * CANOPY_LAG_ROLL;
    if (dx >  lim) dx =  lim; else if (dx < -lim) dx = -lim;
    if (dy >  lim) dy =  lim; else if (dy < -lim) dy = -lim;
    if (dr >  lim) dr =  lim; else if (dr < -lim) dr = -lim;

    s_lag_px = (int)(dx + (dx < 0.0f ? -0.5f : 0.5f));
    s_lag_py = (int)(dy + (dy < 0.0f ? -0.5f : 0.5f));
    // Pixels of y at the frame's edge, spread linearly across the columns.
    s_lag_sh = dr / (float)(SCR_H / 2);
    if (s_lag_px || s_lag_py || s_lag_sh != 0.0f) s_warp_on = true;
}

// The sequence's own clock. Declared here because vg_canopy_warp is where the flex it
// suppresses is set, and defined below with the rest of the intro.
static float s_settle_t = -1.0f;          // < 0 means not settling

void vg_canopy_warp(float k) {
    if (!s_can) return;                    // the maps are built from the drawing
    if (k < 0.0f) k = 0.0f;
    if (k > 1.0f) k = 1.0f;
    const int q = (int)(k * CANOPY_WARP_STEPS + 0.5f);
    s_warp_on = (q != 0) || s_lag_px || s_lag_py || (s_lag_sh != 0.0f);
    if (q == s_wq) return;
    s_wq = q;

    const float a    = (float)q / (float)CANOPY_WARP_STEPS;
    const float zoom = 1.0f + CANOPY_WARP_ZOOM * a;
    const float R2   = (float)SCR_CX * SCR_CX + (float)SCR_CY * SCR_CY;
    const float krn  = HUD_WARP_K * CANOPY_WARP_SPHERE * a / R2;
    s_w_zoom = zoom;
    s_w_zk   = zoom * krn;

    // The forward map along x at the centre line, then inverted. Monotone, so one walk.
    static int16_t fwd[SCR_H];
    for (int x = 0; x < SCR_H; x++) {
        const float dx = (float)x - (float)SCR_CX;
        fwd[x] = (int16_t)(int)((float)SCR_CX + dx * zoom * (1.0f + krn * dx * dx) + 0.5f);
        // ...and what a block endpoint in this column will need, less its dy terms.
        s_w_zbase[x] = zoom + s_w_zk * dx * dx;
    }
    {
        int x = 0;
        for (int xp = 0; xp < SCR_H; xp++) {
            while (x < SCR_H - 1 && fwd[x] < xp) x++;
            int best = x;
            if (x > 0 && (xp - fwd[x - 1]) < (fwd[x] - xp)) best = x - 1;
            s_wcol[xp] = (int16_t)best;
        }
    }
    // And where each band now balances, from the columns the rows actually read.
    if (!s_colcost_ready) canopy_colcost();
    for (int b = 0; b < NUM_BANDS; b++) {
        uint32_t rowc[BAND_H], total = 0;
        for (int r = 0; r < BAND_H; r++) {
            const int lx = SCR_H - 1 - (b * BAND_H + r);
            const int sc = (lx >= 0 && lx < SCR_H) ? s_wcol[lx] : -1;
            rowc[r] = (sc >= 0) ? s_colcost[sc] : 0u;
            total  += rowc[r];
        }
        int at = ROW_SPLIT;
        if (total) {
            uint32_t run = 0, best = 0xFFFFFFFFu;
            for (int r = 1; r < BAND_H; r++) {
                run += rowc[r - 1];
                const uint32_t gap = (run > total - run) ? run - (total - run)
                                                        : (total - run) - run;
                if (gap < best) { best = gap; at = r; }
            }
        }
        s_wsplit[b] = (uint8_t)at;
    }

    // The bow, on top of the sphere, for an author who wants more bend than the panel has.
    const float cx = (float)(SCR_H - 1) * 0.5f;
    for (int c = 0; c < SCR_H; c++) {
        const float t = ((float)c - cx) / cx;          // -1 .. 1
        s_wc[c] = (int16_t)(int)(CANOPY_WARP_BOW * a * t * t * (t < 0.0f ? -1.0f : 1.0f));
    }
}

// WARP OR NO WARP, settled at compile time.
//
// Asked per block, the flag cost 83 us of a 4042 us pass for a question whose answer is the
// same for the whole frame -- the same shape of waste the sky fill's tint flag was. Two
// instantiations, and the rigid one carries no warp code at all.
// One endpoint onto the sphere. `zbase` carries the column's dx term; only dy is left.
static inline int warp_y(int y, float zbase) {
    const float dy = (float)y - (float)SCR_CY;
    return (int)((float)SCR_CY + dy * (zbase + s_w_zk * dy * dy) + 0.5f);
}

// THE TWO EDGES, on their own, so the block walk does not carry them.
//
// Only the FIRST and LAST block of a column can touch a border, so this finds those two and
// extends them -- and by living out here it takes its state, its level lookups and its extra
// branches out of canopy_rows_t. That mattered more than the walk it used to repeat: carrying
// them inline grew the rigid instantiation to 1185 bytes with fifty stack accesses, and cost 67
// us on a path where none of this code even runs.
//
// The extensions never overlap the blocks they come from -- [0, at) and [end, SCR_W) sit
// outside them -- so it does not matter that this runs before the walk rather than around it.
//
// Extended only where the drawing reached the border. Background beyond a member is not
// something to stretch.
static __attribute__((noinline))
void IRAM_ATTR canopy_edges(uint16_t* row, const uint8_t* p, const uint8_t* e, int wofs, float zbase, int c) {
    if (p + 3 > e) return;

    // NEITHER BORDER, NOTHING TO DO, which is most of the screen. Both flags read as set until
    // the table is built, so the tests below answer for themselves exactly as they always did.
    const uint8_t edge = s_colcost_ready ? s_coledge[c]
                                         : (uint8_t)(CANOPY_EDGE_NEAR | CANOPY_EDGE_FAR);
    if (!edge) return;

    // The near edge, from the first block.
    if (edge & CANOPY_EDGE_NEAR) {
        const uint8_t h  = p[0];
        const int     y0 = ((h & 1) << 8) | p[1];
        if (y0 == 0) {
            int at = warp_y(0, zbase) + wofs;
            if (at > SCR_W) at = SCR_W;
            if (at > 0) {
                const uint16_t d = s_can_lut[p[3]];
                if (h & 0x40) span_sub(&row[0], at, d);
                else          span_add(&row[0], at, d);
            }
        }
    }

    if (!(edge & CANOPY_EDGE_FAR)) return;

    // The far edge, from the last block, which the table points straight at. The walk survives
    // only for the frames before that table exists.
    const uint8_t* last = nullptr;
    if (s_colcost_ready) {
        last = &s_can->data[s_colend[c]];
    } else {
        while (p + 3 <= e) {
            last = p;
            const uint8_t h = p[0];
            const int len = ((h & 2) << 7) | p[2];
            p += 3 + ((h & 0x80) ? len : 1);
        }
    }
    if (!last) return;
    const uint8_t h   = last[0];
    const int     y0  = ((h & 1) << 8) | last[1];
    const int     len = ((h & 2) << 7) | last[2];
    if (y0 + len < SCR_W) return;                 // it stopped short of the border
    int end = warp_y(y0 + len, zbase) + wofs;
    if (end < 0) end = 0;
    if (end >= SCR_W) return;
    const uint16_t d = s_can_lut[(h & 0x80) ? last[3 + len - 1] : last[3]];
    if (h & 0x40) span_sub(&row[end], SCR_W - end, d);
    else          span_add(&row[end], SCR_W - end, d);
}

// ===========================================================================
// THE CANOPY COMING ONLINE
//
// The match opens black with the instruments already lit, and the view arrives a region at a
// time. Per region, keyed off the ZONE the artist painted into the green channel:
//
//   the whole region flashes WHITE -- every pixel of it, not just the frame's members,
//   the world then dissolves in out of that white,
//   and the frame's blocks in that zone begin drawing at their authored level.
//
// THE GREEN SHAPE IS THE FLASH. That is the author's own word for it -- "the shapes act as
// mask" -- and the first version got it wrong: it flashed the frame's BLOCKS, which is the
// tenth of the region the cockpit actually covers, so a region lighting up read as a few
// members brightening instead of a piece of the view coming on.
//
// It also produced the overblown colour the author saw. Flashing the blocks meant lerping the
// panel's orange toward white in a per-zone table, which passes through a hot salmon at half
// flash -- and that was being ADDED to an already-bright view, so the channels clipped at
// different points and the hue swung. There is no per-zone colour table any more. The region
// flash is a flat fill and the frame draws its authored level throughout, which cannot clip.
//
// The blocks still have to be withheld until their zone comes up: the gate runs BEFORE them in
// the row, so a block drawn early would sit glowing on the black. That is what the intro table
// is for -- its runs are cut at zone borders, so switching a zone on switches whole blocks on.
// The flight table is not cut that way and its zone tags mean nothing; see the generated header.
//
// The frame is held RIGID throughout. The gate and the frame have to agree pixel for pixel or
// the black would not end where the panel does, and the cheapest way to guarantee that is to
// have both read the same unwarped column. CANOPY_INTRO_SETTLE then ramps the flex in at the
// end, because the resting warp is a long way from flat and the cockpit would otherwise jump
// the moment the sequence released.
// THE MEMBERS LIGHT UP TOO, and this part started as a bug the author liked.
//
// The first version flashed the frame's BLOCKS instead of the region, by lerping the panel's
// orange toward white in a per-zone table. That was the wrong AREA -- and it also blew the hue
// out, because a near-white delta added onto an already-bright view clips the three channels at
// different points and the colour swings on the way. The author saw both, and asked for the
// second one back on top of the corrected first one.
//
// So it is deliberate now, and it has somewhere to live: not the white hold, where a member
// drawn additively onto white is invisible, but the DISSOLVE. As world cells come through, the
// members over them are hot and cooling, so the frame lights up as its region resolves and
// settles to its authored level afterwards.
//
// The saturation IS the effect. Nothing here reaches for a hue sweep, because the clip does it
// for free: red is five bits and saturates first, green has six and holds longer, so a rising
// white delta over a lit world passes through magenta and amber before it whites out. Note that
// CANOPY_INTRO_LIT_PEAK below is what governs how much of that is seen -- at 1.0 the members
// spend their first moments fully white, which is the part with no colour in it at all.
// s_intro_on is declared up beside s_can -- see the note there.
static float   s_intro_t    = 0.0f;
static uint8_t s_izon[VG_CANOPY_MAX_ZONES];   // 0 held, 255 fully dissolved to the world
static uint8_t s_ilive[VG_CANOPY_MAX_ZONES];  // whether this zone's blocks are drawn at all
static uint16_t s_ifill[VG_CANOPY_MAX_ZONES]; // a held pixel: black before the flash, white after
static uint8_t s_iglow[VG_CANOPY_MAX_ZONES];  // 255 white-hot members, 0 their authored level
static bool    s_icued     = false;       // the instruments' cue, latched -- see vg_canopy_intro_cued
static uint8_t s_iq[VG_CANOPY_MAX_ZONES];     // the quantised glow each table was built for
static uint16_t s_ilut[VG_CANOPY_MAX_ZONES][256];

// A zone's member colours, from white-hot down to the authored level.
//
// Quantised by the caller, so this runs a couple of dozen times across a region's cool-down
// rather than once a frame per zone.
static void canopy_ilut(int z) {
    const uint32_t t = (uint32_t)s_iglow[z];        // 0..255 toward white
    for (int g = 0; g < 256; g++) {
        const uint32_t v = s_can_lut[g];            // panel order
        const uint32_t n = (v >> 8) | ((v << 8) & 0xFF00u);
        const uint32_t r = (n >> 11) & 31u, gg = (n >> 5) & 63u, b = n & 31u;
        const uint32_t R = r  + (((31u - r)  * t) >> 8);
        const uint32_t G = gg + (((63u - gg) * t) >> 8);
        const uint32_t B = b  + (((31u - b)  * t) >> 8);
        const uint32_t o = (R << 11) | (G << 5) | B;
        s_ilut[z][g] = (uint16_t)(((o >> 8) | (o << 8)) & 0xFFFFu);
    }
    s_iq[z] = s_iglow[z];
}

// A DITHER, NOT A FADE, and it is both cheaper and a better fit.
//
// Holding a region is a store per pixel. Cross-fading one would be three multiplies per pixel,
// about 15 cycles against a handful, and across a 77,000-pixel zone that is milliseconds for a
// transition that lasts a third of a second.
//
// So the reveal is an ordered 4x4 threshold: a pixel gives up its hold once its cell's value is
// under the zone's reveal. Pixels come through in a fixed scatter instead of the whole region
// changing together, which reads as a picture being acquired rather than a lamp being turned
// down -- and it is the same language the backdrop's own reveal already speaks.
static const uint8_t BAYER4[16] = {
     0,  8,  2, 10,
    12,  4, 14,  6,
     3, 11,  1,  9,
    15,  7, 13,  5,
};

// One panel row of the gate. `lx` is the true screen column, never a warped one.
//
// Runs cover the whole column with no gap, so this walks them all and does nothing for a zone
// that is already fully in. Three bytes a run and about three runs a column, so the walk itself
// is free and the cost is entirely in the pixels it holds.
//
// A held pixel takes the zone's fill -- black before its flash, white during it -- so the same
// loop serves both. That is the whole reason the flash is affordable across a region rather than
// only across the frame: a flat fill is the cheapest thing this file does.
// ---------------------------------------------------------------------------
// A PANEL TAKING A HIT
//
// Per panel, one float. The gate below already fills a region's screen pixels with a
// colour, dithered as it dissolves -- so the flash costs nothing new, and the static is
// the same walk with a different value per pixel.
// ---------------------------------------------------------------------------

static float    s_hit_t[VG_CANOPY_MAX_ZONES];   // seconds left on this panel
static bool     s_gate_on = false;              // the intro is running, or a panel is hit
static uint32_t s_hit_seed = 0x9E3779B9u;       // advanced per frame, so static crawls

// WHITE FOR THE FIRST PART, STATIC FOR THE REST, as a fraction of the hit's life. The
// flash says WHERE and the static says the panel is still hurt -- one is a moment and the
// other is a condition, and a flash that lasted the whole time would read as a lamp.
#define CANOPY_HIT_TIME   1.60f
#define CANOPY_HIT_FLASH  0.82f

static inline uint32_t hit_hash(uint32_t x, uint32_t y, uint32_t s) {
    uint32_t h = x * 0x9E3779B9u ^ y * 0x85EBCA6Bu ^ s;
    h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12;
    return h;
}

// ---------------------------------------------------------------------------
// FAULTY PANELS
// ---------------------------------------------------------------------------

// How many panels are failing, worst first. A panel that has failed stays failed for the
// match: the hull does not heal, so neither does the cockpit.
static uint8_t s_fault_n   = 0;
static uint8_t s_fault_z[VG_CANOPY_MAX_ZONES];

// WHICH PANEL IS THE MIDDLE ONE, from the zone map, once per drawing.
//
// The centroid of each region against the centre of the screen. The artist paints regions
// and never has to say which is the windscreen -- and a drawing whose panels move gets the
// right answer without anyone remembering to update a constant.
// WHICH ZONES A COLUMN CONTAINS, one bit each, and WHICH ZONES WANT THE GATE.
//
// The gate was all-or-nothing: one faulty panel out of four and every column of every
// frame walked the whole zone map to find it. The map covers the entire screen, so that
// is a second full-screen pass to draw a quarter of one.
//
// A column that contains none of the interesting zones can be skipped outright. Built
// from the same walk that finds the centre, because it is the same walk.
static uint16_t s_colmask[SCR_H];
static bool     s_colmask_ready = false;
static uint16_t s_gate_mask     = 0;

static void canopy_find_centre(void) {
    s_centre_z = -1;
    s_colmask_ready = false;
    for (int i = 0; i < SCR_H; i++) s_colmask[i] = 0;
    if (!s_can || s_can->zones <= 0) return;

    uint32_t n[VG_CANOPY_MAX_ZONES] = { 0 };
    uint32_t sx[VG_CANOPY_MAX_ZONES] = { 0 }, sy[VG_CANOPY_MAX_ZONES] = { 0 };

    for (int lx = 0; lx < SCR_H; lx++) {
        const uint8_t* p = &s_can->zdata[s_can->zofs[lx]];
        const uint8_t* e = &s_can->zdata[s_can->zofs[lx + 1]];
        while (p + 3 <= e) {
            const uint8_t h  = p[0];
            const int     y0 = ((h & 1) << 8) | p[1];
            const int     ln = ((h & 2) << 7) | p[2];
            const int     z  = (h >> 2) & 15;
            p += 3;
            if (z >= s_can->zones || ln <= 0) continue;
            s_colmask[lx] |= (uint16_t)(1u << z);
            n[z]  += (uint32_t)ln;
            sx[z] += (uint32_t)lx * (uint32_t)ln;
            sy[z] += (uint32_t)(y0 + ln / 2) * (uint32_t)ln;
        }
    }

    float best = 1e30f;
    for (int z = 0; z < s_can->zones; z++) {
        if (!n[z]) continue;
        const float cx = (float)sx[z] / (float)n[z] - (float)SCR_H * 0.5f;
        const float cy = (float)sy[z] / (float)n[z] - (float)SCR_W * 0.5f;
        const float d  = cx * cx + cy * cy;
        if (d < best) { best = d; s_centre_z = (int8_t)z; }
    }
    s_colmask_ready = true;
}

// WHICH ZONES THE GATE HAS ANYTHING TO SAY ABOUT, as a bitmask over zones.
//
// Every zone during the intro -- the reveal touches all of them -- and otherwise only the
// ones that are hit or faulty. Recomputed wherever s_gate_on is, because they answer the
// same question at two resolutions.
static void canopy_gate_mask(void) {
    if (s_intro_on) { s_gate_mask = 0xFFFFu; return; }
    uint16_t m = 0;
    for (int z = 0; z < VG_CANOPY_MAX_ZONES; z++)
        if (s_hit_t[z] > 0.0f) m |= (uint16_t)(1u << z);
    for (int i = 0; i < s_fault_n; i++) m |= (uint16_t)(1u << s_fault_z[i]);
    s_gate_mask = m;
}

// HOW BROKEN THE HULL HAS TO BE before a panel goes, and how many go by the end.
//
// Nothing until the hull is genuinely in trouble: a cockpit that starts failing at the
// first scratch spends the whole match crying wolf, and the player stops reading it.
#define CANOPY_FAULT_START  0.55f    // hull fraction at which the first panel goes
#define CANOPY_FAULT_END    0.12f    // ...and at which as many as can be are gone

void vg_canopy_damage(float hull_frac) {
    if (!s_can || s_can->zones <= 0) return;
    if (s_centre_z < 0) canopy_find_centre();

    // Everything but the middle one may fail.
    const int usable = (s_can->zones > 1) ? s_can->zones - 1 : 0;
    if (!usable) return;

    float t = (CANOPY_FAULT_START - hull_frac)
            / (CANOPY_FAULT_START - CANOPY_FAULT_END);
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    // ROUNDED UP, so the first panel fails AT the threshold rather than a third of the way
    // past it. With four zones and one reserved, (int)(t * 3) needs t >= 1/3 before it
    // reaches one -- which put the first failure at 41% hull while the constant said 55%.
    int want = (int)(t * (float)usable + 0.9999f);
    if (t <= 0.0f) want = 0;

    // MONOTONIC. Panels are added and never removed, because repairs happen between
    // matches and the hull fraction wobbles upward inside one for no reason the player
    // would credit.
    while (s_fault_n < want && s_fault_n < usable) {
        int free_z[VG_CANOPY_MAX_ZONES];
        int n = 0;
        for (int z = 0; z < s_can->zones; z++) {
            if (z == s_centre_z) continue;
            bool taken = false;
            for (int i = 0; i < s_fault_n; i++) if (s_fault_z[i] == z) taken = true;
            if (!taken) free_z[n++] = z;
        }
        if (!n) break;
        s_fault_z[s_fault_n++] = (uint8_t)free_z[(int)(vg_frand01() * (float)n) % n];
        s_gate_on = true;
        canopy_gate_mask();
    }
}

void vg_canopy_hit_clear(void) {
    s_fault_n  = 0;
    s_centre_z = -1;
    for (int i = 0; i < VG_CANOPY_MAX_ZONES; i++) s_hit_t[i] = 0.0f;
    s_gate_on = s_intro_on;
    canopy_gate_mask();
}

void vg_canopy_hit(void) {
    if (!s_can || s_can->zones <= 0) return;
    // NOT ALREADY MARKED. Refreshing a panel that is already out would waste the hit and
    // read as nothing happening; spreading it is what makes a second hit cost more view
    // than the first.
    int free_z[VG_CANOPY_MAX_ZONES];
    int n = 0;
    for (int z = 0; z < s_can->zones; z++)
        if (s_hit_t[z] <= 0.0f) free_z[n++] = z;
    if (!n) return;
    const int z = free_z[(int)(vg_frand01() * (float)n) % n];
    s_hit_t[z] = CANOPY_HIT_TIME;
    s_gate_on  = true;
    canopy_gate_mask();
}

void vg_canopy_hit_step(float dt) {
    bool any = false;
    for (int z = 0; z < VG_CANOPY_MAX_ZONES; z++) {
        if (s_hit_t[z] > 0.0f) {
            s_hit_t[z] -= dt;
            if (s_hit_t[z] < 0.0f) s_hit_t[z] = 0.0f;
            else                   any = true;
        }
    }
    // Crawls rather than holding still: a static field that does not move is a texture.
    s_hit_seed = s_hit_seed * 1664525u + 1013904223u;
    // FAULTS COUNT TOO, and leaving them out was a bug that made the whole progression
    // invisible: vg_canopy_damage set this true when a panel failed, this line cleared it
    // on the very next frame, and the while loop in there never ran again because the fault
    // was already recorded. A faulty panel drew for one frame and then never again.
    s_gate_on  = any || s_intro_on || (s_fault_n > 0);
    canopy_gate_mask();
}

static __attribute__((noinline))
void IRAM_ATTR canopy_gate(uint16_t* row, int lx, int py) {
    const uint8_t* p = &s_can->zdata[s_can->zofs[lx]];
    const uint8_t* e = &s_can->zdata[s_can->zofs[lx + 1]];
    const uint8_t* bay = &BAYER4[(py & 3) << 2];
    while (p + 3 <= e) {
        const uint8_t h  = p[0];
        const int     y0 = ((h & 1) << 8) | p[1];
        const int     n  = ((h & 2) << 7) | p[2];
        const int     z  = (h >> 2) & 15;
        p += 3;
        if (z >= s_can->zones) continue;

        // NOT WHILE THE COCKPIT IS ARRIVING.
        //
        // The intro owns every region until it finishes. A panel cannot crack before it
        // exists, and both branches below run AHEAD of the reveal -- so a fault left over
        // from the last match, or a round landing during the sequence, painted static over
        // a region the intro had not lit yet and went on flashing behind it.
        //
        // The reveal is the only thing that draws while s_intro_on. Damage resumes the
        // frame it ends.
        if (!s_intro_on) {
            // A FAULTY PANEL, which is a condition rather than an event: mostly it works,
            // and then it does not. The flicker is on the panel's OWN phase so two faulty
            // panels do not blink together, which would read as the whole cockpit strobing.
            bool faulty = false;
            for (int i = 0; i < s_fault_n; i++) if (s_fault_z[i] == z) faulty = true;
            if (faulty) {
                const uint32_t ph = hit_hash((uint32_t)z, 0u, s_hit_seed & ~0xFFu);
                // Out for a short slice of each cycle, and occasionally for a long one.
                const uint32_t beat = (s_hit_seed >> 3) + ph;
                const bool out = ((beat & 0x3Fu) < 7u) || ((beat & 0x3FFu) < 40u);
                if (out) {
                    uint16_t* qf = &row[y0];
                    for (int i = 0; i < n; i++) {
                        const uint32_t r = hit_hash((uint32_t)lx, (uint32_t)(y0 + i), s_hit_seed);
                        if ((r & 0xFFu) < 190u)
                            qf[i] = (r & 0x100u) ? 0xFFFF : 0x0000;
                    }
                    continue;
                }
            }

            // A HIT PANEL IS DRAWN INSTEAD OF THE REGION'S NORMAL STATE, and takes the
            // whole run: white while the flash lasts, then static that thins as it heals.
            // The world behind it is simply gone for that long, which is the point -- the
            // player has lost that piece of the view.
            const float ht = s_hit_t[z];
            if (ht > 0.0f) {
                uint16_t* qh = &row[y0];
                if (ht > CANOPY_HIT_TIME * CANOPY_HIT_FLASH) {
                    for (int i = 0; i < n; i++) qh[i] = 0xFFFF;
                } else {
                    // Thinning: as the hit heals, fewer pixels are taken, so the view comes
                    // back through the static rather than switching back on.
                    const uint32_t keep = (uint32_t)(255.0f * (ht / (CANOPY_HIT_TIME * CANOPY_HIT_FLASH)));
                    for (int i = 0; i < n; i++) {
                        const uint32_t r = hit_hash((uint32_t)lx, (uint32_t)(y0 + i), s_hit_seed);
                        if ((r & 0xFFu) < keep)
                            qh[i] = (r & 0x100u) ? 0xFFFF : 0x0000;
                    }
                }
                continue;
            }
        }

        const uint32_t rev  = s_izon[z];
        if (rev >= 255u) continue;                  // this region is all the way in
        const uint16_t fill = s_ifill[z];
        uint16_t* q = &row[y0];
        if (rev == 0u) {
            for (int i = 0; i < n; i++) q[i] = fill;  // held: black, or the flash
        } else {
            // 0..16, not 0..15. `rev >> 4` tops out at 15, which leaves one cell of the
            // sixteen still held at the very end of the dissolve -- a sixteenth of the region
            // popping through in one frame. *17>>8 reaches 16, which is past every cell.
            const uint32_t th = (rev * 17u) >> 8;
            for (int i = 0; i < n; i++)
                if ((uint32_t)bay[(y0 + i) & 3] >= th) q[i] = fill;
        }
    }
}

// IN IRAM, and the reason is a measurement, not a preference. The bench runs this pass in
// 9,387 us warped and never leaves it; in-game the band loop cycles four phases through a
// 16 KB instruction cache fifteen times a frame, and this -- the largest tenant -- paid
// most of the rent: placing it here was measured at -1,602 us of raster over the course
// baseline, sky and scanlines included, because they stopped being evicted too. Placing
// draw_band as well was measured at ZERO, to the microsecond: the walkers do not care.
// The 7.5 KB it costs is financed by the sky texture's move to PSRAM, which freed 32.
template <bool WARP, bool INTRO>
static void IRAM_ATTR canopy_rows_t(uint16_t* band, int by0, int r0, int r1) {
    if (!s_can_ready) canopy_lut();

    // HOISTED OUT OF THE ROW LOOP, all of it. Which table is a compile-time choice still, so
    // the branch folds; the drawing behind it is a runtime pointer now, and reading it once per
    // call rather than once per row is what keeps that free.
    const VgCanopy* const cp = s_can;
    const uint16_t* const T_OFS  = INTRO ? cp->iofs  : cp->ofs;
    const uint8_t*  const T_DATA = INTRO ? cp->idata : cp->data;
    const int  T_COLS   = (int)cp->cols;
    const bool T_MIRROR = (cp->mirror != 0);

    for (int py = by0 + r0; py < by0 + r1; py++) {
        int lx = SCR_H - 1 - py;                       // this panel row IS a column
        if (WARP) {
            // CLAMPED TO THE EDGE, NOT DROPPED.
            //
            // Shifting or magnifying moves the frame, and a row that lands off the drawing used
            // to be skipped -- which leaves the screen edge empty on the side the frame moved
            // away from, so the cockpit appears to slide and tear rather than to move. Clamping
            // repeats the edge column instead, which is what a texture sampler does at its
            // border and reads as the frame continuing past the screen. Where that column is
            // background, nothing is drawn and nothing is smeared.
            lx += s_lag_px;                       // the frame trailing the turn
            if (lx < 0) lx = 0; else if (lx >= SCR_H) lx = SCR_H - 1;
            lx = s_wcol[lx];
            if (lx < 0) lx = 0; else if (lx >= SCR_H) lx = SCR_H - 1;
        }
        // Mirrored only when the drawing is symmetric, which the baker decides and
        // records. An asymmetric frame stores every column and is read straight
        // through -- a cockpit is allowed to be lopsided.
        int c = (T_MIRROR && lx >= T_COLS) ? (SCR_W - 1 - lx) : lx;
        // Clamped for the same reason, and it also covers a drawing narrower than the screen.
        if (c < 0) c = 0; else if (c >= T_COLS) c = T_COLS - 1;

        const uint8_t* p = &T_DATA[T_OFS[c]];
        const uint8_t* e = &T_DATA[T_OFS[c + 1]];
        uint16_t* row = &band[(py - by0) * SCR_W];

        // THE WORLD FIRST, then the frame on top of it. Everything behind the cockpit is
        // already drawn by the time this primitive runs and the instruments come after it, so
        // this is the one point in the band where blacking the view hides the world without
        // touching the panel. Rigid, so it uses the raw column and lands where the frame does.
        // RUNTIME, NOT THE TEMPLATE PARAMETER. The gate is wanted during the intro and
        // whenever a panel is hit, and a hit happens while the warp is on -- which the
        // intro never is. Keying it off INTRO would have needed a fourth instantiation of
        // this template for warp-and-gate; one predictable branch a row is cheaper.
        // ...AND ONLY FOR COLUMNS THAT CONTAIN A ZONE IT CARES ABOUT. One faulty panel
        // used to cost a full-screen walk of the zone map every frame to find it. The
        // mask is exact, so this skips nothing that would have drawn; until it is built
        // the test passes and the behaviour is what it always was.
        const int glx = SCR_H - 1 - py;
        if (s_gate_on && (!s_colmask_ready || (s_colmask[glx] & s_gate_mask)))
            canopy_gate(row, glx, py);
        // The gate ran; the frame does not. See s_can_rear -- in rear view the world still
        // has to arrive region by region, and the cockpit still has to be absent.
        if (INTRO && s_can_rear) continue;

        // Three bytes of header, then either one level for the whole block or a level
        // per pixel. Nine bits each of start and length, so the odd bit of both rides
        // in the flag byte. The side is in the header too: a block never crosses the
        // background, so nothing here tests light against shade per pixel.
        const int   wofs  = WARP ? (s_wc[c] + s_lag_py
                                    + (int)((float)(c - SCR_H / 2) * s_lag_sh)) : 0;
        // dx is the same for every block in this column, so its share of the sphere hoists.
        const float zbase = WARP ? s_w_zbase[c] : 0.0f;

        // CLAMPING THE OTHER AXIS. The column clamp above covers logical x; this covers logical
        // y, which is a panel row's x. The two need different treatment: a column is SAMPLED, so
        // repeating it is one index clamp, while the runs along a row are a sparse list and the
        // ones touching a border have to be EXTENDED to it -- or the frame ends in mid-air and
        // reads as clipped. Out of line, so the walk below never sees it.
        if (WARP) canopy_edges(row, p, e, wofs, zbase, c);

        // WHERE THE LAST BLOCK ENDED, carried across the walk. -1 is a y no block starts at,
        // which is what the first block of a column needs.
        int pend = -1, pwarp = 0;

        while (p + 3 <= e) {
            const uint8_t h   = p[0];
            const int     y0  = ((h & 1) << 8) | p[1];   // the picture's y is panel x
            const int     len = ((h & 2) << 7) | p[2];
            p += 3;

            // WHOSE TURN IT IS. The zone is only meaningful in the intro table, where a block
            // never spans two of them, so a zone that has not come up yet skips whole blocks.
            //
            // The region's own flash is a flat fill and is already down by the time this runs;
            // what the zone picks here is how hot its MEMBERS still are. Two separate things, and
            // conflating them is what went wrong the first time -- see the note at the intro state.
            const uint16_t* lut = s_can_lut;
            if (INTRO) {
                const int z = (h >> 2) & 15;
                if (z >= s_can->zones || !s_ilive[z]) {
                    p += (h & 0x80) ? len : 1;
                    continue;
                }
                lut = s_ilut[z];
            }

            int at = y0, n = len, skip = 0, n0 = len;
            if (WARP) {
                // Both ends through the same map, and the length is the difference -- so the
                // run still ends exactly where the next one starts.
                //
                // WHICH IS ALSO WHY THE START CAN BE KEPT. Three blocks in ten begin exactly
                // where the one before them ended, and warp_y is a pure function of y down a
                // column -- zbase is fixed for the whole of it and s_w_zk for the whole frame --
                // so the end carried forward IS what the call would return, bit for bit. The
                // gap-freeness above is the same property read the other way round: it wants
                // the two values equal, and this stops computing the second one.
                const int w0 = (y0 == pend) ? pwarp : warp_y(y0, zbase);
                const int w1 = warp_y(y0 + len, zbase);
                pend  = y0 + len;
                pwarp = w1;
                at = w0 + wofs;
                n  = w1 + wofs - at;
                n0 = n;                       // before clipping: what the step is measured on
                // THE TABLE'S OWN BOUNDS NO LONGER APPLY. Baked, every block was inside the
                // row by construction and the baker verifies it; moved at runtime, that
                // guarantee is gone and a block can hang off either edge. Trimming the FRONT
                // also has to advance the level array, or a literal block would paint its
                // remaining pixels with the colours of the ones that were clipped.
                if (at < 0)            { skip = -at; n -= skip; at = 0; }
                if (at + n > SCR_W)    { n = SCR_W - at; }
                if (n <= 0) { if (h & 0x80) p += len; else p++; continue; }
            }

            if (h & 0x80) {                              // a level per pixel
                if (WARP && n0 != len) {
                    // Stretched or squeezed: walk the levels at the ratio rather than one
                    // for one, or the block reads past its own data. See span_lit_add_rs.
                    const uint32_t step =
                        ((uint32_t)(len - 1) < CANOPY_STEP_L && (uint32_t)(n0 - 1) < CANOPY_STEP_N)
                            ? s_steptab.v[len - 1][n0 - 1]
                            : ((uint32_t)len << 16) / (uint32_t)n0;
                    const uint32_t i0   = (uint32_t)skip * step;
                    if (h & 0x40) span_lit_sub_rs(&row[at], n, p, lut, i0, step);
                    else          span_lit_add_rs(&row[at], n, p, lut, i0, step);
                } else {
                    if (h & 0x40) span_lit_sub(&row[at], n, p + skip, lut);
                    else          span_lit_add(&row[at], n, p + skip, lut);
                }
                p += len;
            } else {                                     // one level, delta hoisted
                const uint16_t d = lut[*p++];
                if (h & 0x40) span_sub(&row[at], n, d);
                else          span_add(&row[at], n, d);
            }
        }
    }
}

// WHERE THE SEQUENCE IS UP TO.
//
// The zones come up in the order the artist painted, which is already the zone INDEX: the baker
// sorts the green values it found and stores each block's position in that order, so zone 0 is
// simply first and this needs no table of its own.
void vg_canopy_intro_begin(void) {
    // AN INTACT CANOPY, because this is a new match and the last one's damage is not
    // this one's. Faults never heal by design, so nothing else was ever going to clear
    // them: vg_canopy_hit_clear ran once at boot, and a second match inherited the
    // panels the first one lost -- flashing through the intro and carrying on after it
    // over a hull at full health.
    //
    // Here rather than in the state code because both match-start paths call this, and a
    // third would have had to remember.
    vg_canopy_hit_clear();

    // NO COCKPIT, NO SEQUENCE -- BUT THE CHAIN STILL HAS TO RUN.
    //
    // The sequence is the cockpit arriving a region at a time, and with no drawing there are no
    // regions and no zone map to hold the world black with. So it does not play.
    //
    // The cue is latched anyway, and that is the part that matters: the instruments hang off it
    // -- draw_instruments is vg_cockpit.cued -- so returning early here left a hull with no canopy
    // showing no cockpit AND no instruments, for ever. The chain degrades to what the game did
    // before there were canopies: the panel catches, then the player is ready, then the radio.
    if (!s_can) {
        s_intro_on = false;
        s_gate_on  = false;   // nothing to reveal and nothing broken
        s_gate_mask = 0;
        s_icued    = true;
        s_settle_t = -1.0f;
        for (int i = 0; i < 3; i++) { s_lag_q[i] = s_lag_v[i] = s_lag_x[i] = 0.0f; }
        s_lag_px = s_lag_py = 0;
        s_lag_sh = 0.0f;
        return;
    }
    s_intro_on = true;
    s_gate_on  = true;   // the reveal IS the gate; set here so frame one has it
    s_gate_mask = 0xFFFFu;
    s_intro_t  = 0.0f;
    s_settle_t = -1.0f;
    s_icued    = false;
    if (!s_can_ready) canopy_lut();
    for (int z = 0; z < s_can->zones; z++) {
        s_izon[z] = 0; s_ilive[z] = 0; s_ifill[z] = 0;   // held, and held BLACK
        s_iglow[z] = 255;                                // and white-hot the moment it lights
        canopy_ilut(z);
    }
    // Nothing left over on the spring, or the frame would start the sequence already leaning.
    for (int i = 0; i < 3; i++) { s_lag_q[i] = s_lag_v[i] = s_lag_x[i] = 0.0f; }
    s_lag_px = s_lag_py = 0;
    s_lag_sh = 0.0f;
}

bool vg_canopy_intro_active(void) { return s_intro_on; }

// HAS THE SEQUENCE CALLED FOR THE INSTRUMENTS YET.
//
// A LATCH, and it is a latch because the obvious version was a bug. This started as a
// `progress() >= CANOPY_INTRO_HUD_AT` test against a progress function that returned 1.0 when
// nothing was running -- so the condition was satisfied by DEFAULT, and vg_hud_decay is reached
// from vg_world_step, which runs in the attract loop, the title and the cutscene. The cockpit's
// power-on sound played on the title screen.
//
// Worse, it was not idempotent across matches: vg_match_start is called from enter_intro, at the
// top of the CUTSCENE, so the game-side flag was cleared several seconds before the player ever
// took the seat -- and the cutscene then ran thousands of world steps with the cue armed.
//
// So the state cannot be inferred. Only the running sequence sets this, and only `reset` and
// `begin` clear it, which makes every ordering safe: nothing is cued at power-on, nothing is
// cued while a match is being built, and nothing is cued twice.
bool vg_canopy_intro_cued(void) { return s_icued; }

// HOW MANY REGIONS HAVE LIT IN THE RUNNING SEQUENCE, 0..CANOPY_ZONES.
//
// Reported rather than acted on, because the thing that wants it is a SOUND and this file is
// the band rasteriser -- it has no business talking to the mixer, and a cue fired from here
// would be a cue that does not happen when the panel is not being drawn. The caller watches
// this for a change and beeps, which also lets it pitch each one.
//
// ZERO WHEN NOTHING IS RUNNING, and that is not a detail. `reset` leaves every region live so
// the flight path draws the whole frame, so counting s_ilive unconditionally reports four the
// entire time a match is not booting -- and the caller, whose own counter was just zeroed,
// would have fired four beeps over the cutscene. That is the same trap the instruments' cue
// fell into: a query that answers for a state it is not in. The answer has to be scoped to the
// sequence, because that is the only thing the question means.
int vg_canopy_intro_lit(void) {
    if (!s_can || !s_intro_on) return 0;
    int n = 0;
    for (int z = 0; z < s_can->zones; z++) if (s_ilive[z]) n++;
    return n;
}

// Nothing running and nothing cued -- for when a match is BUILT, which happens at the top of the
// cutscene and is a long way from the player taking the seat.
void vg_canopy_intro_reset(void) {
    s_intro_on = false;
    s_icued    = false;
    s_settle_t = -1.0f;
    // A HULL WITH NO DRAWING REACHES HERE, and this used to read s_can->zones anyway.
    //
    // vg_match_start calls this from the top of the cutscene, before anything has selected a
    // cockpit for the hull about to fly -- so on a hull that HAS no cockpit, s_can is null and
    // the load faulted. The panic rebooted the board, and from the seat that looks like the
    // match refusing to start and dropping back to the title. Reported exactly that way.
    //
    // Guarded here rather than at the call, because the three scalars above are the boot
    // chain's disarm and have to happen for every hull. Only the per-zone arrays need a
    // drawing to be about.
    if (!s_can) return;
    for (int z = 0; z < s_can->zones; z++) { s_izon[z] = 255; s_ilive[z] = 1; s_iglow[z] = 0; }
}

// HOW MUCH FLEX THE FRAME IS ALLOWED, 0 through the sequence and 1 once it has settled.
//
// The caller multiplies both the warp and the lag by this. It is not an optimisation: the world
// gate reads the unwarped column, so a warped frame during the sequence would put the black
// somewhere other than where the panel ends. The ramp afterwards exists because the resting
// warp is full bulge -- releasing straight into it would pop.
float vg_canopy_intro_flex(void) {
    if (!s_can)     return 1.0f;    // nothing to hold flat, and nothing to settle
    if (s_intro_on) return 0.0f;
    if (s_settle_t < 0.0f) return 1.0f;
    float a = s_settle_t / CANOPY_INTRO_SETTLE;
    if (a >= 1.0f) return 1.0f;
    return a * a * (3.0f - 2.0f * a);          // smoothstep, so it arrives without a corner
}

bool vg_canopy_intro_update(float dt) {
    if (!s_can) return false;
    if (!s_intro_on) {
        if (s_settle_t >= 0.0f) {
            s_settle_t += dt;
            if (s_settle_t >= CANOPY_INTRO_SETTLE) s_settle_t = -1.0f;
        }
        return false;
    }
    s_intro_t += dt;

    // THE INSTRUMENTS' CUE, latched from inside the running sequence -- which is the only place
    // that can know the sequence is actually running. The length is derived here from the pacing
    // constants, so retuning them moves the cue with them rather than sliding it out of step.
    const float span = CANOPY_INTRO_LEAD + (float)(s_can->zones - 1) * CANOPY_INTRO_STEP
                     + CANOPY_INTRO_FLASH + CANOPY_INTRO_DISSOLVE + CANOPY_INTRO_LIT;
    if (!s_icued && span > 0.0f && s_intro_t >= span * CANOPY_INTRO_HUD_AT) s_icued = true;

    for (int z = 0; z < s_can->zones; z++) {
        const float e = s_intro_t - (CANOPY_INTRO_LEAD + (float)z * CANOPY_INTRO_STEP);
        if (e < 0.0f) continue;                  // this one's turn has not come
        s_ilive[z]  = 1;
        s_ifill[z]  = CANOPY_INTRO_WHITE;        // it has flashed, so a held pixel is white now

        // The region holds solid white for FLASH, then the world dissolves out of it. A flash
        // has an instant onset by definition, so there is no ramp up -- the fill switches from
        // black to white on the frame the zone's turn arrives.
        float r = (e - CANOPY_INTRO_FLASH) / CANOPY_INTRO_DISSOLVE;
        if (r < 0.0f) r = 0.0f;
        if (r > 1.0f) r = 1.0f;
        s_izon[z] = (uint8_t)(r * 255.0f + 0.5f);

        // The members HOLD at full heat through the dissolve and cool only afterwards.
        //
        // Cooling them from the moment the region lit was the obvious reading and it wasted the
        // effect: a member is invisible against the region's own white fill, so at the instant the
        // glow peaked there was nothing behind it to clip against, and by the time world cells
        // came through the colour had already gone. The heat has to still be there when the world
        // arrives, because the world is what it saturates against.
        //
        // So the frame is the LAST thing to settle, cooling over LIT after its region is fully in.
        float f = 1.0f - (e - CANOPY_INTRO_FLASH - CANOPY_INTRO_DISSOLVE) / CANOPY_INTRO_LIT;
        if (f < 0.0f) f = 0.0f;
        if (f > 1.0f) f = 1.0f;
        f *= CANOPY_INTRO_LIT_PEAK;
        const int q = (int)(f * CANOPY_INTRO_QSTEP + 0.5f);
        const uint8_t want = (uint8_t)((q * 255) / CANOPY_INTRO_QSTEP);
        if (want != s_iq[z]) { s_iglow[z] = want; canopy_ilut(z); }
    }

    // Over when the last zone's members have finished cooling, which is now the last thing to
    // happen anywhere in the sequence. The settle that follows is not part of it: the view is
    // fully in well before this and the frame is merely taking up its flex.
    const float end = CANOPY_INTRO_LEAD + (float)(s_can->zones - 1) * CANOPY_INTRO_STEP
                    + CANOPY_INTRO_FLASH + CANOPY_INTRO_DISSOLVE + CANOPY_INTRO_LIT;
    if (s_intro_t >= end) {
        s_intro_on = false;
        s_settle_t = 0.0f;
        for (int z = 0; z < s_can->zones; z++) {
            s_izon[z] = 255; s_ilive[z] = 1;
            if (s_iglow[z]) { s_iglow[z] = 0; canopy_ilut(z); }
        }
        return false;
    }
    return true;
}

// Three instantiations, not four. The intro is always rigid -- see the note above the intro
// state -- so there is no warped intro path to build.
// Which of the three balance points applies, for draw_band's two-core split.
// BALANCING TIME, NOT PIXELS.
//
// The baked split puts the cut where the DRAWING costs the same either side, and measured
// in a course run that still left core 1 finishing its half in 6,701 us and then standing
// at the rendezvous for 2,217 while core 0 took 8,918 for an equal share of the picture.
//
// The two cores are not equally fast at the same work. Core 0 carries the audio task and
// the system's own, so an even share of pixels is an uneven share of time, and a band costs
// the SLOWER half. No table baked from the drawing can know that -- it is a property of what
// else is running, and it moves.
//
// So it is measured. The bias nudges toward whichever side is waiting, one row at a time,
// and settles wherever the two halves finish together. One row of 32 is about 3% of a band,
// small enough that a wrong step costs nothing and slow enough that it does not chase noise.
//
// IT BALANCES THE WHOLE PRIMITIVE PASS NOW, not the canopy alone. draw_band's fork grew
// from the canopy case to take every primitive of the band, and the half/wait pair feeding
// this comes off brackets around that whole fork -- so the bias settles where the two
// cores' shares of ALL the drawing finish together. The baked table still supplies the
// starting cut, because the canopy remains the heaviest and most uneven item in the pass;
// the bias absorbs everything else, the same way it already absorbed the other core's load.
static int s_split_bias = 0;

void vg_canopy_split_nudge(uint32_t half_us, uint32_t wait_us) {
    // Only while there is something to balance. A frame with no canopy reports both at zero
    // and must not drag the bias anywhere.
    if (!half_us) return;
    // A FORTIETH of the half. A tenth parked the bias at 18 rows with 613 us still showing,
    // and tightening it did close that -- canw fell to 157 and the cut moved to 19.6.
    //
    // IT BOUGHT NOTHING, and that is worth writing down rather than tuning further. A band
    // costs max(half, half), and core 1 simply absorbed the work core 0 put down: 7,841/8,454
    // became 8,314/8,471. Both settings sit on the flat bottom of the same curve. The tighter
    // one is kept because a balance held to 157 us survives a change of scene better than one
    // held to 613, not because it is faster.
    //
    // The split is finished. What is left in the canopy is its work, not its scheduling.
    const uint32_t dead = half_us / 40u;
    if (wait_us > dead) {
        // This core waited: the other side is slower, so give it less by cutting later.
        if (s_split_bias < BAND_H / 3) s_split_bias++;
    } else if (wait_us < dead / 2u) {
        // It did not wait at all, so this side is the slow one. Cut earlier.
        if (s_split_bias > -BAND_H / 3) s_split_bias--;
    }
}

int vg_canopy_split_at(int band_index) {
    if (!s_can)      return ROW_SPLIT;
    if (s_intro_on)  return ROW_SPLIT;   // the world gate dwarfs the frame; midpoint is right
    int at = s_warp_on ? (int)s_wsplit[band_index] : (int)s_can->split[band_index];
    at += s_split_bias;
    // Clamped well inside the band: a split at 0 or BAND_H is not a split, and the pass
    // would silently go back to one core.
    if (at < 2)              at = 2;
    if (at > BAND_H - 2)     at = BAND_H - 2;
    return at;
}

// THE COLOUR TABLE IS BUILT BEFORE THE FORK, ONCE, ON ONE CORE.
//
// canopy_rows_t opens with a lazy rebuild -- `if (!s_can_ready) canopy_lut()` -- and the
// alarm requant dirties the table mid-flight, so under the two-core split BOTH cores
// could enter the 256-entry rebuild together and draw through the torn half. The first
// split analysis flagged this as latent; the whole-loop split made it hot; what it looks
// like from outside is a handful of wrong-coloured canopy pixels that come and go BY
// BUILD LAYOUT, which framed two innocent changes before the race was run to ground.
// The lazy check stays as a backstop -- once warmed it never fires in the frame.
void vg_canopy_warm(void) {
    if (s_can && !s_can_ready) canopy_lut();
}

void IRAM_ATTR vg_canopy_rows(uint16_t* band, int by0, int r0, int r1) {
    if (!s_can) return;                    // no drawing selected: no cockpit, and no crash
    // Aft with no sequence running: there is no frame to draw and no world to hold back, so
    // the whole pass is skipped. Checked here rather than at the submit site, because the
    // primitive still has to exist for the intro's gate to run.
    //
    // THE INTRO, NOT THE GATE. Damage briefly widened this to s_gate_on, which meant a
    // single faulty panel put the ENTIRE canopy back into the aft pass for the rest of the
    // match -- the one view that had been free of it.
    //
    // The rear view is a button the player presses, and it is exempt: the damage is to the
    // FORWARD of the ship, so looking back at what is chasing you is never obscured by a
    // panel that broke in front of you. Hits are covered by the same rule and for the same
    // reason.
    if (s_can_rear && !s_intro_on) return;
    if (s_intro_on)     canopy_rows_t<false, true>(band, by0, r0, r1);
    else if (s_warp_on) canopy_rows_t<true, false>(band, by0, r0, r1);
    else                canopy_rows_t<false, false>(band, by0, r0, r1);
}

// AND `oth` SPLIT INTO ITS THREE, for the same reason the canopy got its own counter.
// A DEAD COUNTER FOR AS LONG AS THE TINT HAS BEEN DRAWN AT THE SOURCE.
//
// s_tint_us was declared, reset and read, and nothing ever wrote to it, so `tnt` reported
// 0 for a warning that costs over a millisecond. It cost real time to trust: it was quoted
// three times as evidence the boundary effect was cheap, and then as evidence it was
// expensive, and the number behind both was never measured at all.
//
// THE COST IS NOT SEPARABLE ANY MORE, which is why this returns a LEVEL and not a time.
// The tint moved into the sky fill and into submit, and then out of both --
// so its work is interleaved with the fill it colours, and bracketing it would mean a
// second pass to measure the first. What it costs is visible directly in `sky`, measured:
// 1792 us with the warning off and 3028 with it on, up to 3445 hard against the wall.
//
// So `tnt` reports HOW HARD THE WARNING IS RUNNING, 0 to 100. That is the number that was
// actually missing: without it there is no way to tell which telemetry windows had the
// effect active, so an average over a flight buries it -- which is exactly how a 1240 us
// cost and six frames a second were measured as "no change".
// HOW HARD THE WALL WARNING IS RUNNING, 0 to 100. Not microseconds, and it has been three
// different wrong things.
//
// It was a dead counter for as long as the tint was drawn at the source: s_tint_us was
// declared, reset and read and nothing ever wrote to it, so `tnt` reported 0 for an effect
// costing over a millisecond. Then it was pointed at s_tint_k -- in the same commit that
// switched the ring off by handing vg_rast_tint a zero -- so it faithfully reported that
// zero while the cockpit was visibly red on the panel. Two flights each time.
//
// The level is what was actually needed. Without it there is no way to tell which telemetry
// windows had the warning up, so an average over a flight buries the effect entirely -- which
// is how 1240 us and six frames a second were once measured as "no change".
uint32_t vg_rast_tint_us(void) {
    return (uint32_t)(s_alarm * 100.0f + 0.5f);
}

// WHAT THE DRAWING COSTS, on the device and without flying.
//
// The canopy only draws inside a match, so reading its cost off the frame counter needs
// someone at the controls -- and that counter reports one band's worse half plus the
// rendezvous, which is not a number the baker can predict from a table. This runs the
// whole pass on one core over a scratch row and reports it straight.
//
// Row by row, because a real band buffer is 30 KB and there is not that much heap free
// at runtime. Each panel row is an independent column of the drawing, so a row at a time
// is the same work in the same order; internal SRAM is uniform, so a reused row costs
// what a spread-out band costs.
//
// The scratch is refilled between rows, outside the timing. Blending into the same row
// 480 times would drive every channel to the rail and leave every saturation branch
// taken, which is not what the pass meets in a frame.
void vg_canopy_bench(VgCanopyCost* out) {
    static uint16_t scratch[SCR_W];
    if (!s_can) { *out = VgCanopyCost{}; return; }
    if (!s_can_ready) canopy_lut();

    // BOTH STATES, because the warp is the thing most likely to be changed next and its cost
    // is the question. The game's own setting is saved and put back, so running the bench does
    // not leave the frame flexing at whatever the bench used last.
    const int   save_q  = s_wq;
    const bool  save_on = s_warp_on;

    // AND THE INTRO IS PINNED OFF for the first two passes, because canopy_rows now dispatches
    // on it. Left alone, a bench run during the sequence would time the intro path and report
    // it as the flight cost -- the same failure the glyph bench had, where the number is wrong
    // and believed. Measured on purpose in the third pass below.
    const bool  save_in = s_intro_on;
    uint8_t     save_live[VG_CANOPY_MAX_ZONES], save_rev[VG_CANOPY_MAX_ZONES];
    uint16_t    save_fill[VG_CANOPY_MAX_ZONES];
    for (int z = 0; z < s_can->zones; z++) {
        save_live[z] = s_ilive[z]; save_rev[z] = s_izon[z]; save_fill[z] = s_ifill[z];
    }
    s_intro_on = false;

    uint32_t cyc = 0, cal = 0, wcyc = 0, icyc = 0;
    vg_canopy_warp(0.0f);
    for (int py = 0; py < SCR_H; py++) {
        for (int x = 0; x < SCR_W; x++) scratch[x] = 0x1084;   // mid grey, both ways to go
        const uint32_t t0 = esp_cpu_get_cycle_count();
        vg_canopy_rows(scratch, py, 0, 1);
        cyc += esp_cpu_get_cycle_count() - t0;

        // The same two reads around nothing, so the harness pays for itself.
        const uint32_t t1 = esp_cpu_get_cycle_count();
        asm volatile("" ::: "memory");
        cal += esp_cpu_get_cycle_count() - t1;
    }
    vg_canopy_warp(1.0f);
    for (int py = 0; py < SCR_H; py++) {
        for (int x = 0; x < SCR_W; x++) scratch[x] = 0x1084;
        const uint32_t t0 = esp_cpu_get_cycle_count();
        vg_canopy_rows(scratch, py, 0, 1);
        wcyc += esp_cpu_get_cycle_count() - t0;
    }
    // THE INTRO, AT ITS WORST, which is the only figure worth having.
    //
    // Its cost is not constant: a zone that has not come up is a full-screen black fill, one
    // mid-dissolve is a threshold test per pixel, and one that is all the way in is skipped
    // entirely. The dear case is every zone dissolving at once -- which the sequence never
    // actually reaches, since the zones are staggered -- so this is a ceiling and not a
    // measurement of the sequence.
    //
    // Perf is not the point of the intro and the author has said so. This exists so that "it
    // dips" is a number rather than an impression, and so the next person to widen the zones
    // knows what they are spending.
    s_intro_on = true;
    for (int z = 0; z < s_can->zones; z++) {
        s_ilive[z] = 1; s_izon[z] = 128; s_ifill[z] = CANOPY_INTRO_WHITE;
    }
    vg_canopy_warp(0.0f);
    for (int py = 0; py < SCR_H; py++) {
        for (int x = 0; x < SCR_W; x++) scratch[x] = 0x1084;
        const uint32_t t0 = esp_cpu_get_cycle_count();
        vg_canopy_rows(scratch, py, 0, 1);
        icyc += esp_cpu_get_cycle_count() - t0;
    }

    s_wq = save_q; s_warp_on = save_on;   // leave the game's own flex as it was
    s_intro_on = save_in;
    for (int z = 0; z < s_can->zones; z++) {
        s_ilive[z] = save_live[z]; s_izon[z] = save_rev[z]; s_ifill[z] = save_fill[z];
    }

    out->us       = (cyc  > cal ? cyc  - cal : 0) / 240u;
    out->warp_us  = (wcyc > cal ? wcyc - cal : 0) / 240u;
    out->intro_us = (icyc > cal ? icyc - cal : 0) / 240u;
    out->blocks   = (int)s_can->blocks;
    out->flat_px  = (int)s_can->flat_px;
    out->lit_px   = (int)s_can->lit_px;
}
