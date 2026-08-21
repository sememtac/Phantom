// ===========================================================================
// THE FLAT SPAN, EIGHT PIXELS AT A TIME -- the PIE path.
//
// The 36 cycles a flat pixel cost were never arithmetic: they were the
// read-modify-write of the band, one 32-bit access per pixel pair. The S3's
// vector unit changes the denominator -- one 128-bit access per eight pixels,
// and the blend itself in 16-bit lanes. Measured 3.9 c/px against 31.3 on the
// first (16-pixel-unit) prototype; this second shape exists because the first
// one could not reach the pixels: a flat block averages twenty pixels, and a
// scalar head-and-tail walk left most of a short span on the slow path.
//
// So the unit is EIGHT pixels -- one Q register -- and the edges are masked,
// not walked: the first and last units blend all eight lanes and then keep
// only the covered ones, merging the rest back from the original. Every span
// the dispatcher sends arrives entirely on the vector path.
//
// The guard-bit trick does not survive vectorisation (red's guard is the
// carry out of a 16-bit lane), so the planes come fully apart: extract R/G/B
// into their own lanes, blend with min/relu clamps, repack. min(x+d, 31) and
// max(x-d, 0) on separated fields are EXACTLY the arithmetic the guard bits
// compute, which is why the self-test in vg_canopy_draw.cpp demands
// bit-identity, not closeness.
//
// No byte swap: the fields are extracted from the panel-order lane directly.
// For native N = R<<11|G<<5|B, the swapped lane is
//     V = (G&7)<<13 | B<<8 | R<<3 | G>>3
// and ONE multiplier constant, 8192, reaches every field through SAR alone:
// VMUL is (a*b)>>SAR, so 8192 with SAR 16/21/26 extracts R, B and G-low, and
// with SAR 10/5/0/16 puts everything back. The two masks double as the two
// clamp ceilings. Four resident constants, four working registers: it fits
// the Q file exactly, and the park slot of the first prototype is gone.
//
// IRAM, like every other pass that runs per band: the icache counter reads
// ~2,180 misses a frame and this loop must not be among them.
// ===========================================================================

    .section .dram1,"aw"
    .align  16
pie_consts:                     // broadcast one u16 into each of 8 lanes
    .short  8192                // q0: the one multiplier, steered by SAR
    .short  0x001F              // q1: red/blue mask, and their ceiling
    .short  0x003F              // q2: green mask, and its ceiling
    .short  0x0007              // q3: the three-bit split of green

// vrelu's scalar args are multiplier and shift for the negative side; zeros
// make it max(x, 0). It is two-operand, so this shim gives it the three-arg
// shape the macro hands every clamp. a11 holds zero for SAR duty anyway.
.macro  pie_relu QA, QB, QC
    ee.vrelu.s16    \QA, a11, a11
.endm

// One eight-pixel unit, panel order in, panel order out. OP is the
// saturating add or subtract; CLAMP0/CLAMP1 finish a red-blue or green lane.
// MREG, if given, is an address register holding a 16-byte lane mask: blend
// all eight, keep the masked lanes, restore the rest from the original unit
// (reloaded through the write cursor, which still points at it).
.macro  pie8 OP, CLAMP0, CLAMP1, MREG
    ee.vld.128.ip   q4, a2, 16          // V, eight panel-order pixels
    wsr.sar         a4                  // 16
    ee.vmul.u16     q5, q4, q0          // V >> 3 ...
    ee.andq         q5, q5, q1          // ... = R
    wsr.sar         a12                 // 21
    ee.vmul.u16     q6, q4, q0          // V >> 8 ...
    ee.andq         q6, q6, q1          // ... = B
    ee.andq         q7, q4, q3          // V & 7 = G high three
    wsr.sar         a13                 // 26
    ee.vmul.u16     q4, q4, q0          // V >> 13 = G low three; V now dead
    wsr.sar         a14                 // 10
    ee.vmul.u16     q7, q7, q0          // (V & 7) << 3
    ee.orq          q7, q7, q4          // G
    ee.vldbc.16     q4, a8              // dR
    \OP             q5, q5, q4
    \CLAMP0         q5, q5, q1
    ee.vldbc.16     q4, a9              // dG
    \OP             q7, q7, q4
    \CLAMP1         q7, q7, q2
    ee.vldbc.16     q4, a10             // dB
    \OP             q6, q6, q4
    \CLAMP0         q6, q6, q1
    wsr.sar         a14                 // 10
    ee.vmul.u16     q5, q5, q0          // R << 3
    wsr.sar         a15                 // 5
    ee.vmul.u16     q6, q6, q0          // B << 8
    wsr.sar         a11                 // 0
    ee.vmul.u16     q4, q7, q0          // (G & 7) << 13, truncation masks it
    wsr.sar         a4                  // 16
    ee.vmul.u16     q7, q7, q0          // G >> 3
    ee.orq          q5, q5, q6
    ee.orq          q5, q5, q4
    ee.orq          q5, q5, q7          // the blended unit, panel order
  .ifnb \MREG
    ee.vld.128.ip   q6, \MREG, 0        // the lane mask for this edge
    ee.vld.128.ip   q7, a7, 0           // the original, via the write cursor
    ee.andq         q5, q5, q6          // keep the covered lanes...
    ee.notq         q6, q6
    ee.andq         q7, q7, q6          // ...and the uncovered originals
    ee.orq          q5, q5, q7
  .endif
    ee.vst.128.ip   q5, a7, 16
.endm

.macro  pie_span NAME, OP, CLAMP0, CLAMP1
    .section .iram1,"ax"
    .align  4
    .global \NAME
    .type   \NAME,@function
// a2 = first unit, 16-byte aligned, covering the span; a3 = unit count >= 1;
// a4 = three u16 deltas {dR, dG, dB} in internal SRAM (the caller's stack);
// a5 = head mask; a6 = tail mask (pre-combined into a5 when a3 == 1).
\NAME:
    entry   a1, 32
    movi    a12, pie_consts
    ee.vldbc.16.ip  q0, a12, 2
    ee.vldbc.16.ip  q1, a12, 2
    ee.vldbc.16.ip  q2, a12, 2
    ee.vldbc.16.ip  q3, a12, 2
    mov     a8, a4                      // &dR
    addi    a9, a4, 2                   // &dG
    addi    a10, a4, 4                  // &dB
    movi    a4, 16                      // the SAR fan
    movi    a11, 0                      //   (and vrelu's zero)
    movi    a12, 21
    movi    a13, 26
    movi    a14, 10
    movi    a15, 5
    mov     a7, a2                      // the write cursor trails the reads
    pie8    \OP, \CLAMP0, \CLAMP1, a5   // head unit, masked
    addi    a3, a3, -2
    bltz    a3, .Ldone\@                // one unit: the head was also the tail
    loopnez a3, .Lmid\@
    pie8    \OP, \CLAMP0, \CLAMP1
.Lmid\@:
    pie8    \OP, \CLAMP0, \CLAMP1, a6   // tail unit, masked
.Ldone\@:
    retw
.endm

    pie_span vg_pie_span_add8m, ee.vadds.s16, ee.vmin.s16, ee.vmin.s16
    pie_span vg_pie_span_sub8m, ee.vsubs.s16, pie_relu,    pie_relu
