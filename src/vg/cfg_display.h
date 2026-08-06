#pragma once

// ===========================================================================
// Display, rasteriser and screen-space effects.
// ===========================================================================

#define SCR_W                480
#define SCR_H                480
#define SCR_CX               240.0f
#define SCR_CY               240.0f

// The panel is a rounded square: the corners are cut, and anything anchored
// hard against an edge loses its first characters. Text and controls that sit
// against a border get inset by this much. World rendering is unaffected --
// losing a few pixels of starfield to the bezel does not matter, losing the
// first digit of the credit balance does.
#define SCR_SAFE             38

// Load generator. The attract loop tops out around 300 primitives; a real
// dogfight runs 400-850, so every measurement of the case that actually matters
// needed a human holding the board and flying. This fills the attract loop with
// a full complement of fighters shooting at each other instead, so the worst
// case can be measured hands-off and repeatably. Off in normal builds.
#define VG_BENCH             0

// --- orientation -----------------------------------------------------------
// Canonical orientation is the three hardware buttons pointing UP. The panel
// scans with them on the left, so everything is drawn through a quarter turn
// and touch is rotated back the other way.
//
//   0 = native    1 = 90 CCW    2 = 180    3 = 90 CW
//
// Done at submit time (a couple of ops per primitive) rather than by transposing
// the band buffers, which would cost ~0.26 ms per band against ~0.3 ms of spare
// budget. If the image comes out upside down relative to the buttons, use 3.
#define VG_ROTATE            1

// --- banding ---------------------------------------------------------------
// Horizontal bands rasterised in internal SRAM, one DMA each, double-buffered
// so band N+1 is drawn while band N clocks out.
//
// 32 rows is chosen precisely: 480*32*2 = 30720 bytes, under the S3's 32768-byte
// per-transaction ceiling, so a band is exactly ONE transaction with no chunking,
// and 480/32 = 15 bands exactly. The CO5300 needs even-aligned windows; 32 is
// even and every band origin is a multiple of it, so that is automatic.
#define BAND_H               32
#define NUM_BANDS            (SCR_H / BAND_H)

// Screen-space primitives per frame. Thick-stroked missile trails cost up to 3
// primitives per segment, which is what set this ceiling.
// 3400 WAS FOUR TIMES WHAT THE GAME USES, and internal SRAM is the scarce pool.
//
// The list has to be internal -- it is swept once per band, fifteen times a frame -- and so
// do the band buffers and the backdrop's 32 KB texture. A third band buffer took the total
// past what fits, and the backdrop lost: 32 KB requested, no contiguous block that size, no
// nebula. 3400 primitives at 20 bytes is 66 KB of the pool the backdrop needed 32 of.
//
// Measured per slice, high water since boot, in the attract loop:
//
//   slice 0 star+hoops    93 of 800
//   slice 1 rails         38 of 300
//   slice 2 world        164 of 1500
//   slice 3 instruments  224 of 800
//
// 2000 keeps at least twice the observed peak on every slice and stays above each one's
// THEORETICAL worst, which is the figure that matters where one is known: the grid is at most
// 294 hoop and 160 rail segments, and the rear-view patch re-submits a half-density grid of
// ~234 into the instruments' slice on top of the HUD's own.
//
// An overflow is visible rather than silent -- the telemetry line prints OVERFLOW and the
// primitive count drops -- so if a busy fight finds a slice's ceiling it says so. The shard
// burst is the case 3400 was sized for and it lands in slice 2, which keeps the most room.
#define MAX_PRIMS            2000

// --- projection ------------------------------------------------------------
// FOCAL is the projection-plane distance in pixels: on a 480 px wide screen, 400
// gives ~62 deg horizontal FOV, which reads as a cockpit view without the
// fisheye a wider angle produces on a square panel.
#define FOCAL                400.0f
#define NEAR_Z               1.0f

// --- CRT scanlines ---------------------------------------------------------
// One row in three, halved in brightness. Halving specifically, because it is a
// shift and a mask: the general per-channel scale it replaced cost ~18 ops per
// pixel and broke the DMA window once a backdrop lit every pixel.
#define SCANLINE_PITCH       3

// --- line quality ----------------------------------------------------------
// Anti-aliased (Wu) lines. Each step lights the two pixels straddling the true
// line with coverage from its fractional position, which is what removes the
// stair-stepping on shallow diagonals -- and the HUD warp turns every straight
// instrument line into exactly that.
//
// Costs roughly 6x a solid Bresenham pixel: twice the pixels, and unlike a plain
// store it must READ the destination to blend, so it cannot be a write-only
// loop. Set to 0 for hard-edged lines.
#define VG_LINE_AA           1

// --- spherical HUD warp ----------------------------------------------------
// Instruments are laid out on a flat grid and then bent, as though painted on
// the inside of a curved canopy, so the HUD reads as part of the aircraft rather
// than pasted on in 2D. Radial: displacement grows with the square of the
// distance from screen centre.
//
// NEGATIVE is barrel (edges pull inward, centre bulges) -- the classic CRT face,
// and the safe direction: pincushion pushes the left-edge throttle off-screen.
// Touch handling stays perfectly linear; only the drawing bends.
#define HUD_WARP_K           (-0.22f)
// px per subdivision of a warped edge. 44 -> 64 in the performance pass: the
// HUD was 1.5ms of submit, most of it subdivision, and at 64 the longest lines
// keep their bend with a third fewer segments. The curve is quantised anyway
// (HUD_WARP_STEPS), so the coarser polyline hides under the same quantisation.
#define HUD_WARP_SEG         64.0f

// The bend tracks SPEED, turning a static stylistic effect into continuous
// feedback on how fast you are going.
#define HUD_WARP_SPEED_MIN   0.60f      // 40% less warp when crawling

// Effectively continuous. This was 9 steps, because sub-pixel drift used to make
// every HUD line snap between whole pixels as the warp changed, which read as
// shimmer. Anti-aliasing removed that cause -- a line at a fractional position
// now renders as smooth coverage -- so the quantisation was costing fluidity for
// nothing. Kept as a large number rather than removed so there is still a dial
// if the shimmer ever comes back.
#define HUD_WARP_STEPS       64.0f
