#include "vg_tv.h"
#include "cfg_display.h"
#include <string.h>

#include "vg_states.h"
#include "vg_sfx.h"

TvState vg_tv;

// SMOOTHSTEP, copied from vg_render.cpp rather than shared. It is four operations and a
// clamp; publishing a header for it would be more coupling than the duplication costs.
static inline float tv_smoothstep(float e0, float e1, float x) {
    if (x <= e0) return 0.0f;
    if (x >= e1) return 1.0f;
    const float t = (x - e0) / (e1 - e0);
    return t * t * (3.0f - 2.0f * t);
}

void vg_tv_begin(uint8_t to) {
    vg_tv.phase = TV_OUT;
    vg_tv.to    = to;
    vg_tv.t     = 0.0f;
}

void vg_tv_clear(void) {
    vg_tv.phase = TV_NONE;
    vg_tv.to    = 0;
    vg_tv.t     = 0.0f;
}

bool vg_tv_step(float dt, void (*join)(uint8_t to)) {
    bool joined = false;
    if (vg_tv.phase == TV_NONE) return false;
    vg_tv.t += dt;

    if (vg_tv.phase == TV_OUT) {
        if (vg_tv.t >= TV_OUT_TIME) { vg_tv.phase = TV_HOLD; vg_tv.t = 0.0f; }
        return false;
    }
    if (vg_tv.phase == TV_HOLD) {
        // THE SWITCH HAPPENS AT THE END OF THE DEAD AIR, not the start of it.
        //
        // Joining at the start would let the new scene run for a whole second
        // behind a black screen -- and the first thing a scene does is start
        // talking. The opening line of the ring course would have been a third
        // spent before the picture existed to show it, which is a mistake already
        // made once in this file's history and not worth making twice.
        if (vg_tv.t >= TV_HOLD_TIME) {
            // BEFORE the phase advances and before the sound, which is where
            // tv_join() sat when this lived in vg_states.cpp. A state's enter hook runs
            // inside this call, and moving it after TV_IN would let that hook see a
            // different phase than it always has.
            joined = true;
            if (join) join(vg_tv.to);
            vg_tv.phase = TV_IN;
            vg_tv.t     = 0.0f;
            // With the picture, not before it: the thump and the dot are the
            // same moment, and hearing it during the dead air would put the
            // sound of the set striking over a screen that is still black.
            vg_sfx_play(SFX_TV_ON, 1.0f);
        }
        return joined;
    }
    if (vg_tv.t >= TV_IN_TIME) {
        vg_tv.phase = TV_NONE;
        vg_tv.t     = 0.0f;
    }
    return joined;
}

void vg_tv_apply(void) {
    // Both directions are the same two phases, but
    // they are not mirror images and each control has its own timing, which is
    // why this is three curves and not one progress number.
    //
    // OUT: the picture fades, the aperture closes on it, and the scan band comes
    // up as it goes -- so the last thing on screen is the band and not a shrunken
    // copy of the scene. IN: the band is already lit, holds a moment, then opens
    // while the picture comes back up underneath it.
    if (vg_tv.phase == TV_NONE) {
        vg_tv_set(1.0f, 1.0f, 0.0f, 0.0f);
    } else if (vg_tv.phase == TV_HOLD) {
        vg_tv_set(0.0f, 0.0f, 0.0f, 1.0f);      // dead air
    } else {
        // ONE CURVE, RUN BOTH WAYS. `c` is how far the set is toward being off:
        // 0 is a picture, 1 is a dot about to vanish. Going out it runs forward,
        // coming back it runs in reverse, so the two are the same shape by
        // construction rather than by two sets of numbers agreeing.
        //
        // They were separate before, and the turn-on had drifted into something
        // else entirely -- the white resolving away before the bar had finished
        // opening, which reads as two bands travelling apart rather than as one
        // line growing out of the middle.
        //
        // Phases, in the order they happen going OUT:
        //   the picture darkens                  dim  rises first
        //   it collapses to the centre line      open falls, 0.05..0.62
        //   what is left whitens                 wash rises
        //   the line shortens to a dot and goes  wide falls, 0.62..1.00
        //
        // THE TWO SHRINKS DO NOT OVERLAP. They used to: the line began losing its
        // width while it was still losing its height, so the dot phase was over
        // in a couple of frames and read as a flash rather than as a motion.
        // Handing the horizontal its own stretch of the curve -- more than a
        // third of it -- is what makes a dot something the eye can follow out of
        // the middle of the screen, and it costs the collapse nothing.
        const float c = (vg_tv.phase == TV_OUT) ? (vg_tv.t / TV_OUT_TIME)
                                                : (1.0f - vg_tv.t / TV_IN_TIME);
        vg_tv_set(1.0f - tv_smoothstep(0.05f, 0.62f, c),
                   1.0f - tv_smoothstep(0.62f, 1.00f, c),
                   // Lit for essentially the whole of it. The kill at the very end
                   // is what makes the dot go OUT rather than merely get small.
                   tv_smoothstep(0.20f, 0.55f, c) * (1.0f - tv_smoothstep(0.985f, 1.0f, c)),
                   tv_smoothstep(0.00f, 0.50f, c));
    }
}


// The set turning on and off
//
// A picture that cuts straight from the menu to the cockpit is a scene change.
// A picture that collapses to a line and comes back is a BROADCAST, which is
// what the tournament is meant to be -- so the transition carries the fiction
// rather than just covering the seam.
//
// Three independent controls, because the two directions are not mirror images
// and one "progress" number could not express either:
//   open  how much of the height is picture at all, centred; the aperture
//   glow  the bright scan band riding the aperture edge
//   dim   how far what remains is faded toward black
//
// All of it is per-ROW work. Whole rows go black, whole rows go white, and only
// the rows that are still picture pay for anything per-pixel -- which is what
// makes an effect over the whole screen affordable at all.
// ---------------------------------------------------------------------------

static float s_tv_open = 1.0f;   // how much of the height is lit, centred
static float s_tv_wide = 1.0f;   // ...and of the width: the dot before the line
static float s_tv_wash = 0.0f;   // how far the lit part is washed toward white
static float s_tv_dim  = 0.0f;   // ...and how far the rest is faded to black

void vg_tv_set(float open, float wide, float wash, float dim) {
    // KEEP IT ONLY IF IT IS IN RANGE, rather than rejecting what looks wrong. A NaN
    // fails every test written the obvious way -- (v < 0) and (v > 1) are both false for
    // it -- so a NaN would sail through a rejection and land in the state. The wall
    // warning's own setter used the same shape before it was deleted.
    #define TV_CLAMP(v) (((v) >= 0.0f && (v) <= 1.0f) ? (v) : (((v) > 0.0f) ? 1.0f : 0.0f))
    s_tv_open = TV_CLAMP(open);
    s_tv_wide = TV_CLAMP(wide);
    s_tv_wash = TV_CLAMP(wash);
    s_tv_dim  = TV_CLAMP(dim);
    #undef TV_CLAMP
}

bool vg_tv_active(void) {
    return s_tv_open < 1.0f || s_tv_wide < 1.0f
        || s_tv_wash > 0.0f || s_tv_dim  > 0.0f;
}

// Dim a span by shifting every channel right, which is a halving per step.
//
// MASK FIRST, THEN SHIFT. Shifting the packed word first would drag blue's low
// bit into red's top bit and green's into blue's, so each field is isolated
// before it moves and re-masked after.
//
// WHERE GREEN ACTUALLY IS. VGC() swaps the two bytes of a native RGB565 word, so
// native R[15:11] G[10:5] B[4:0] is stored as:
//
//   bits 15..13  green's LOW three bits      bits 12..8  blue
//   bits  7..3   red                         bits  2..0  green's HIGH three bits
//
// Green's MOST significant bits are at the BOTTOM of the word. Reading 15..13 as
// "green high" because it sits in the high byte is the natural mistake and it is
// the one made here first: the wash raised red, blue and the bottom bit of green,
// which is the definition of magenta, and the tube flashed bright pink.
#define TV_R   0x00F800F8u
#define TV_B   0x1F001F00u
#define TV_G   0x00070007u      // green's top three bits
#define TV_R16 0x00F8u
#define TV_B16 0x1F00u
#define TV_G16 0x0007u

// Washing toward WHITE is the opposite operation and cheaper: raise the top bits
// of every channel. Four steps from untouched to full white, which is plenty for
// something that resolves in half a second.
//
//   L1  R bit 7        B bit 12        G bit 2
//   L2  R bits 7,6     B bits 12,11    G bits 2,1
//   L3  R bits 7,6,5   B bits 12..10   G bits 2,1,0
static const uint16_t TV_WASH[5] = { 0x0000, 0x1084, 0x18C6, 0x1CE7, 0xFFFF };

static inline uint16_t tv_px(uint16_t v, int s, uint16_t wash) {
    if (s >= 4) v = 0;
    else if (s > 0) v = (uint16_t)((((v & TV_R16) >> s) & TV_R16)
                                 | (((v & TV_B16) >> s) & TV_B16)
                                 | (((v & TV_G16) >> s) & TV_G16));
    return (uint16_t)(v | wash);
}

static inline void tv_span(uint16_t* row, int x0, int x1, int s, uint16_t wash) {
    if (x1 <= x0) return;
    if (s <= 0 && wash == 0) return;                       // nothing to do
    if (wash == 0xFFFF) {                                  // solid, skip the maths
        for (int x = x0; x < x1; x++) row[x] = 0xFFFF;
        return;
    }
    if (s >= 4 && wash == 0) { memset(row + x0, 0, (size_t)(x1 - x0) * 2); return; }

    int x = x0;
    if (x & 1) { row[x] = tv_px(row[x], s, wash); x++; }   // reach alignment

    const uint32_t w32 = (uint32_t)wash | ((uint32_t)wash << 16);
    uint32_t* p = (uint32_t*)(row + x);
    const int n = (x1 - x) >> 1;
    for (int i = 0; i < n; i++) {
        uint32_t v = p[i];
        if (s >= 4)      v = 0;
        else if (s > 0)  v = (((v & TV_R) >> s) & TV_R)
                           | (((v & TV_B) >> s) & TV_B)
                           | (((v & TV_G) >> s) & TV_G);
        p[i] = v | w32;
    }
    const int xt = x + n * 2;
    if (xt < x1) row[xt] = tv_px(row[xt], s, wash);        // odd tail
}

// ---------------------------------------------------------------------------
// The set turning on
//
// An old tube does not open like a shutter. The signal arrives, the line of the
// raster lights at the middle of the screen, and it spreads outward through the
// tube until the whole face is carrying picture -- so what the player should see
// is ONE bright bar at the centre that GROWS, not two edges travelling apart.
//
// That is the difference between this and the first version. The first lit the
// two boundaries of an opening aperture, which is a shutter with glowing edges:
// perfectly good-looking, and the wrong machine.
//
// Four controls, because the phases are genuinely independent:
//   wide  the dot opening into a line, before there is any height at all
//   open  the bar growing outward from the centre line
//   wash  how far what is lit is still raw white rather than resolved picture
//   dim   how far the rest of it has gone to black
//
// THE BAR RUNS ACROSS THE BAND, NOT DOWN IT. The band buffer is in panel space
// and the game is drawn through a quarter turn (see rot_pt in vg_raster.cpp), so
// logical (lx,ly) lands at panel (ly, H-1-lx) and a whole buffer row is a logical
// COLUMN. The first version of this effect ran sideways for exactly that reason.
// ---------------------------------------------------------------------------
void vg_tv_band(uint16_t* band, int by0) {
    (void)by0;

    // A six-level shift would band visibly across a fade this short, so the
    // fractional level is dithered by row -- adjacent rows sit either side of the
    // true level and the eye blends them, at no per-pixel cost.
    static const float ROWD[4] = { 0.125f, 0.625f, 0.375f, 0.875f };
    // Four levels, not five: green has three bits here to red and blue's five, so
    // a fifth step would zero green while the others still carried a tenth of
    // their range -- a fade to black that went through pink on the way.
    const float dimf = s_tv_dim * 4.0f;
    const int   dim0 = (int)dimf;
    const float frac = dimf - (float)dim0;

    const float washf = s_tv_wash * 4.0f;
    const int   wash0 = (int)washf;
    const float wfrac = washf - (float)wash0;

#if VG_ROTATE == 1 || VG_ROTATE == 3
    // Logical y -- the axis the bar grows along -- is the panel's x. A quarter
    // turn either way puts it there, and the bar is symmetric about the centre,
    // so 1 and 3 need no distinguishing; nor do 0 and 2.
    const float half = SCR_W * 0.5f;
    float ap = s_tv_open * half;
    if (s_tv_wash > 0.0f && ap < 1.5f) ap = 1.5f;   // the line itself, always lit
    int lo = (int)(half - ap), hi = (int)(half + ap);
    if (lo < 0)     lo = 0;
    if (hi > SCR_W) hi = SCR_W;
    if (hi < lo)    hi = lo;

    // Logical x is the panel's y: the dot spreading into a full-width line.
    const float hhalf = SCR_H * 0.5f;
    float wp = s_tv_wide * hhalf;
    if (s_tv_wash > 0.0f && wp < 1.5f) wp = 1.5f;
    const int rlo = (int)(hhalf - wp), rhi = (int)(hhalf + wp);

    for (int r = 0; r < BAND_H; r++) {
        uint16_t* row = band + r * SCR_W;
        const int y   = by0 + r;

        if (y < rlo || y >= rhi) {          // outside the line's length: dark
            memset(row, 0, SCR_W * 2);
            continue;
        }
        if (lo > 0)     memset(row,      0, (size_t)lo * 2);
        if (hi < SCR_W) memset(row + hi, 0, (size_t)(SCR_W - hi) * 2);

        const int s = dim0 + (ROWD[y & 3] < frac  ? 1 : 0);
        const int w = wash0 + (ROWD[y & 3] < wfrac ? 1 : 0);
        tv_span(row, lo, hi, s, TV_WASH[w > 4 ? 4 : w]);
    }
#else
    // Untested: this board is VG_ROTATE 1. Kept so the effect survives a port.
    const float half = SCR_H * 0.5f;
    float ap = s_tv_open * half;
    if (s_tv_wash > 0.0f && ap < 1.5f) ap = 1.5f;
    const int lo = (int)(half - ap), hi = (int)(half + ap);

    const float whalf = SCR_W * 0.5f;
    float wp = s_tv_wide * whalf;
    if (s_tv_wash > 0.0f && wp < 1.5f) wp = 1.5f;
    int clo = (int)(whalf - wp), chi = (int)(whalf + wp);
    if (clo < 0) clo = 0;
    if (chi > SCR_W) chi = SCR_W;

    for (int r = 0; r < BAND_H; r++) {
        const int y   = by0 + r;
        uint16_t* row = band + r * SCR_W;
        if (y < lo || y >= hi) { memset(row, 0, SCR_W * 2); continue; }
        if (clo > 0)     memset(row,       0, (size_t)clo * 2);
        if (chi < SCR_W) memset(row + chi, 0, (size_t)(SCR_W - chi) * 2);
        const int s = dim0  + (ROWD[y & 3] < frac  ? 1 : 0);
        const int w = wash0 + (ROWD[y & 3] < wfrac ? 1 : 0);
        tv_span(row, clo, chi, s, TV_WASH[w > 4 ? 4 : w]);
    }
#endif
}
