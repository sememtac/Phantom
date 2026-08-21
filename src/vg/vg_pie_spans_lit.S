// ===========================================================================
// THE LIT SPAN -- a level per pixel, so the deltas arrive as lanes, not a
// broadcast. Sibling of vg_pie_spans.S, which carries the flat spans and the
// full story; this unit adds the graded edges, which are 44% of AEGIS's
// pixels and 57% of CHARIOT's.
//
// The vector unit has no gather, so the per-pixel LUT walk cannot leave the
// scalar side -- the dispatcher stages lut[lv[i]] into a 16-byte tile per
// unit, and everything else comes here: the field split, the blend, the
// clamps, the masked edges. The delta fields are extracted from the
// NATIVE-order tile with the same 8192 constant through two more SAR stops:
// D>>11 is SAR 24 (self-masking), D>>5 is SAR 18 then the green mask. The
// tile is read three times, once per field, because the Q file has no
// register to hold it; a load from the caller's stack is one cycle, and the
// a-file ends this variant exactly full.
// ===========================================================================

    .section .dram1,"aw"
    .align  16
pie_consts:                     // broadcast one u16 into each of 8 lanes
    .short  8192                // q0: the one multiplier, steered by SAR
    .short  0x001F              // q1: red/blue mask, and their ceiling
    .short  0x003F              // q2: green mask, and its ceiling
    .short  0x0007              // q3: the three-bit split of green

.macro  pie_relu QA, QB, QC
    ee.vrelu.s16    \QA, a11, a11
.endm

.macro  pie8_lit OP, CLAMP0, CLAMP1, MREG
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
    ee.vld.128.ip   q4, a8, 0           // the delta tile, native order
    wsr.sar         a9                  // 24
    ee.vmul.u16     q4, q4, q0          // D >> 11 = dR lanes, self-masked
    \OP             q5, q5, q4
    \CLAMP0         q5, q5, q1
    ee.vld.128.ip   q4, a8, 0           // the tile again
    wsr.sar         a10                 // 18
    ee.vmul.u16     q4, q4, q0          // D >> 5, 11 bits wide ...
    ee.andq         q4, q4, q2          // ... = dG lanes
    \OP             q7, q7, q4
    \CLAMP1         q7, q7, q2
    ee.vld.128.ip   q4, a8, 16          // and once more, advancing to the next
    ee.andq         q4, q4, q1          // dB lanes
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
    ee.andq         q5, q5, q6
    ee.notq         q6, q6
    ee.andq         q7, q7, q6
    ee.orq          q5, q5, q7
  .endif
    ee.vst.128.ip   q5, a7, 16
.endm

.macro  pie_span_lit NAME, OP, CLAMP0, CLAMP1
    .section .iram1,"ax"
    .align  4
    .global \NAME
    .type   \NAME,@function
// a2 = first unit, 16-byte aligned; a3 = unit count >= 1; a4 = the delta
// tiles, one aligned 16-byte tile of native-order deltas per unit; a5 = head
// mask; a6 = tail mask (pre-combined into a5 by the dispatcher when a call
// has only one unit).
\NAME:
    entry   a1, 32
    movi    a12, pie_consts
    ee.vldbc.16.ip  q0, a12, 2
    ee.vldbc.16.ip  q1, a12, 2
    ee.vldbc.16.ip  q2, a12, 2
    ee.vldbc.16.ip  q3, a12, 2
    mov     a8, a4                      // the tile cursor
    movi    a4, 16                      // the SAR fan
    movi    a9, 24
    movi    a10, 18
    movi    a11, 0
    movi    a12, 21
    movi    a13, 26
    movi    a14, 10
    movi    a15, 5
    mov     a7, a2                      // the write cursor trails the reads
    pie8_lit \OP, \CLAMP0, \CLAMP1, a5  // head unit, masked
    addi    a3, a3, -2
    bltz    a3, .Ldone\@                // one unit: the head was also the tail
    loopnez a3, .Lmid\@
    pie8_lit \OP, \CLAMP0, \CLAMP1
.Lmid\@:
    pie8_lit \OP, \CLAMP0, \CLAMP1, a6  // tail unit, masked
.Ldone\@:
    retw
.endm

    pie_span_lit vg_pie_span_lit_add8m, ee.vadds.s16, ee.vmin.s16, ee.vmin.s16
    pie_span_lit vg_pie_span_lit_sub8m, ee.vsubs.s16, pie_relu,    pie_relu

