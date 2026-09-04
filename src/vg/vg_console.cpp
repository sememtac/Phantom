#include "vg_console.h"
#include "vg_ui.h"
#include "vg_raster.h"
#include "vg_draw.h"
#include "vg_glitch.h"
#include "vg_game.h"
#include <math.h>

// Where the sweep landed this frame. The fault is drawn AT the line rather than
// anywhere on the glass, so the two have to agree, and working the position out
// twice would let them disagree by a pixel on the frames it matters.
static float s_sweep_y = 0.0f;

// Whether the glass bracket is open. A key has to be drawn FLAT wherever it is
// called from -- see vg_console_key -- and the only way to put the bracket back
// afterwards is to know it was open.
static bool s_glass = false;

static void fill_box(const VgBezelSlot* s, uint16_t col) {
    vg_fill_rect(s->bx0, s->by0, s->bx1 - s->bx0 + 1, s->by1 - s->by0 + 1, col);
}

// ---------------------------------------------------------------------------

void vg_console_open(const VgBezel* b, VgConsoleForm form,
                     const char* headline, const char* note) {
    // vg_bezel_use is the one place the current machine is kept. This function had
    // its own copy of the pointer as well, written every frame and never read.
    vg_bezel_use(b);
    if (!b) return;

    // THE HEADLINE IS A TICKER IN A HOLE, and the hole is the only thing this
    // layer has that vg_ui does not: the slot's two rectangles. Everything else
    // about a running banner was general, and forty lines of it lived here
    // because the console was the only screen that had one. See vg_ticker.
    const VgBezelSlot* hl = vg_bezel_headline();
    if (hl) {
        const VgRect fill = { hl->bx0, hl->by0,
                              (int16_t)(hl->bx1 - hl->bx0 + 1),
                              (int16_t)(hl->by1 - hl->by0 + 1) };
        const VgRect run  = { hl->x0, hl->y0,
                              (int16_t)(hl->x1 - hl->x0 + 1),
                              (int16_t)(hl->y1 - hl->y0 + 1) };
        // A TITLE IS SET LARGE AND A CRAWL IS NOT. Two words at scale 3 are a
        // sign over a desk; a tournament's results at scale 3 are twenty-six
        // characters of a four-hundred character read, which never shows enough
        // of itself at once to have a shape. And a note means both lines are
        // smaller, because two of them share one window.
        const int scale = (form == VG_CON_BROADCAST) ? 2 : (note ? 2 : 3);
        vg_ticker(fill, run, headline, note, vg.state_t, scale);
    }

    // A BROADCAST STOPS HERE: the banner and the plating, and no tube under it.
    // See VgConsoleForm.
    if (form == VG_CON_BROADCAST) return;

    // GLASS FROM HERE. The plating is cold steel and dead flat; the display under
    // it is a tube, and a tube bends its picture. The curve pulls the corners of
    // the picture inward and the chassis paints last over what is left, so the
    // display does not end at a drawn border -- it disappears beneath the steel.
    //
    // Finer chords than the cockpit uses: a panel border is 266px on a side and at
    // the default that is five straight pieces with visible joints.
    vg_hud_warp(true, VG_CON_WARP);
    vg_hud_warp_seg(VG_CON_SEG);
    s_glass = true;

    int gx0, gy0, gx1, gy1;
    if (!vg_console_glass(&gx0, &gy0, &gx1, &gy1)) return;
    gx1 += gx0 - 1;
    gy1 += gy0 - 1;

    // --- what makes it a display rather than a window ---------------------
    //
    // Both of these go down FIRST, so the screen's own instruments draw over
    // them. They belong to the DISPLAY and not to what it is showing, and a
    // fiducial crossing a word would be reading as content.
    //
    // REGISTRATION CROSSES, tiled. Every instrument panel ever built has them --
    // alignment marks, the thing a display is checked against rather than
    // anything it is telling you. They are inside the warp bracket for a second
    // reason: a curve needs something regular laid across it to be seen. Bent
    // text is just badly set and a bent border could be a drawn shape, but a grid
    // of identical marks that are no longer on a grid can only be glass.
    for (int gy = gy0 + VG_CON_TICK_STEP / 2; gy < gy1; gy += VG_CON_TICK_STEP)
        for (int gx = gx0 + VG_CON_TICK_STEP / 2; gx < gx1; gx += VG_CON_TICK_STEP) {
            vg_line((float)(gx - VG_CON_TICK_ARM), (float)gy,
                    (float)(gx + VG_CON_TICK_ARM), (float)gy, INK_TRACE);
            vg_line((float)gx, (float)(gy - VG_CON_TICK_ARM),
                    (float)gx, (float)(gy + VG_CON_TICK_ARM), INK_TRACE);
        }

    // THE SWEEP: one line down the glass on a loop, which is the cheapest
    // possible way to say the hardware is powered. A still picture is a picture;
    // a still picture with one thing crossing it on a clock is a MACHINE showing
    // you a picture.
    //
    // IT RUNS WIDE OF THE GLASS ON PURPOSE. The warp is a barrel curve, so it
    // pulls a point inward in proportion to its distance from the centre -- and
    // the ends of a line spanning the whole aperture are the furthest points on
    // it. Drawn edge to edge the sweep came up SHORT of both edges, which reads as
    // a line that has been cut rather than as glass that is curved.
    //
    // AND IT IS NOT A METRONOME. Every pass takes its pace from a hash of the pass
    // number: an exponent bends the ramp without moving its ends, so a pass still
    // starts at the top and finishes at the bottom however hard it is bent; one
    // pass in eight STALLS partway down, which is the most convincing thing here
    // because broken hardware hesitates rather than running slow; and one in four
    // breaks into pieces, because a sweep that is always whole is a drawn object
    // and one that is sometimes in bits is a signal.
    {
        const float hh   = (float)(gy1 - gy0);
        const float u    = vg.state_t * VG_CON_SWEEP_RATE;
        const float pass = floorf(u / hh);
        const uint32_t ph = vg_glitch_hash((uint32_t)pass * 2654435761u);

        float f = u / hh - pass;
        f = powf(f, 0.65f + (float)(ph & 255u) * (0.85f / 255.0f));
        if (((ph >> 9) & 7u) == 0u) {
            const float at = 0.20f + (float)((ph >> 12) & 127u) * (0.55f / 127.0f);
            if (f > at && f < at + 0.10f) f = at;
        }

        const float sy = (float)gy0 + f * hh;
        const float x0 = (float)(gx0 - VG_CON_SWEEP_OVER);
        const float x1 = (float)(gx1 + VG_CON_SWEEP_OVER);
        s_sweep_y = sy;

        // Two lines rather than one -- a faint band with a brighter edge leading
        // it -- because a single hairline reads as a scratch ON the glass where a
        // pair reads as something passing behind it.
        const int n = (((ph >> 17) & 3u) == 0u) ? 3 : 1;
        for (int i = 0; i < n; i++) {
            const float a = x0 + (x1 - x0) * ((float)i / (float)n);
            const float b = x0 + (x1 - x0) * ((float)(i + 1) / (float)n)
                          - ((n > 1) ? 26.0f : 0.0f);
            vg_line(a, sy, b, sy, INK_FAINT);
            vg_line(a, sy + 2.0f, b, sy + 2.0f, INK_TRACE);
        }
    }

    // AND THE SWEEP CATCHES, now and then.
    //
    // THE FAULT BELONGS TO THE SWEEP. It used to tear bands anywhere on the glass
    // and scatter grain over the rest, and the trouble with that is not that it
    // was ugly -- it handed the eye three or four new places to look every time it
    // fired, on a screen whose job is to be read. A fault with no location is
    // noise, and noise is distracting precisely because there is nothing in it to
    // find. Tied to the line there is ONE place it can happen and the eye already
    // knows where. It reads as the sweep snagging rather than the panel breaking,
    // which is the smaller and truer claim.
    //
    // Same vocabulary as a hit -- vg_glitch's hash, bucketed rather than sampled
    // per frame -- because the game has one way of saying a readout is in trouble.
    {
        const uint32_t slow = (uint32_t)(vg.state_t * VG_CON_FAULT_RATE);
        const uint32_t sh   = vg_glitch_hash(slow * 40503u + 17u);
        if ((sh % 5u) == 0u) {
            const int w  = gx1 - gx0;
            const int bh = 3 + (int)((sh >> 7) % 5u);
            const int by = (int)s_sweep_y - bh / 2;

            // A TEAR IS SHIFTED SIGNAL, not a stripe over the top: the band is
            // knocked out and a fragment put back beside where it came from. The
            // fragment is BRIGHT because a dark one is nothing -- out on open
            // glass INK_WELL over near-black showed only where a band happened to
            // cross a word.
            if (by > gy0 && by + bh < gy1) {
                vg_fill_rect(gx0, by, w, bh, INK_WELL);
                const int fw = 40 + (int)((sh >> 19) % 120u);
                int fx = gx0 + (int)((sh >> 5) % (uint32_t)(w - fw));
                if (fx < gx0)             fx = gx0;
                if (fx + fw > gx0 + w)    fx = gx0 + w - fw;
                vg_fill_rect(fx, by, fw, 2, INK_BRIGHT);
            }
        }
    }
}

void vg_console_close(void) {
    s_glass = false;
    // Back to flat before the steel. The chassis is a span blit and never goes
    // through warp_pt, so this is belt and braces -- but leaving the bracket open
    // across a state change would hand the next screen a bent panel.
    vg_hud_warp(false, 1.0f);

    // LAST, so the steel masks whatever ran past the glass, with the drawing's own
    // outline rather than with a rectangle.
    vg_bezel_prim();
}

void vg_console_flat(float cx, float cy, int* dx, int* dy) {
    if (dx || dy) {
        float wx = cx, wy = cy;
        if (s_glass) vg_hud_warp_at(VG_CON_WARP, cx, cy, &wx, &wy);
        if (dx) *dx = (int)lrintf(wx - cx);
        if (dy) *dy = (int)lrintf(wy - cy);
    }
    if (s_glass) vg_hud_warp(false, 1.0f);
}

void vg_console_bend(void) {
    if (!s_glass) return;
    vg_hud_warp(true, VG_CON_WARP);
    vg_hud_warp_seg(VG_CON_SEG);
}

bool vg_console_glass(int* x, int* y, int* w, int* h) {
    const VgBezelSlot* g = vg_bezel_slot(0);
    if (!g) return false;
    const int x0 = g->x0 + VG_GLASS_INSET_X, x1 = g->x1 - VG_GLASS_INSET_X;
    const int y0 = g->y0 + VG_GLASS_INSET_Y, y1 = g->y1 - VG_GLASS_INSET_Y;
    if (x) *x = x0;
    if (y) *y = y0;
    if (w) *w = x1 - x0 + 1;
    if (h) *h = y1 - y0 + 1;
    return true;
}

// THE HIT AREA IS BIGGER THAN THE KEY, and it has to be.
//
// A key window is about 30px tall, which on a 314 ppi panel is 2.4mm. A fingertip
// is nearer 8, so a target that size is one you have to aim at -- and a press that
// misses by two millimetres reads as the button not working rather than as the
// finger being off. Nothing else is hit-tested out in the plating, so the area is
// free to run into it.
//
// The DRAWN key does not grow. Making the picture match would put it over the
// chassis; growing only what it accepts costs nothing and is invisible.
static bool key_rect(int n, int* x, int* y, int* w, int* h) {
    const VgBezelSlot* s = vg_bezel_slot(n);
    if (!s) return false;
    *x = s->bx0 - 20;
    *y = s->by0 - 9;
    *w = (s->bx1 - s->bx0 + 1) + 40;
    *h = (s->by1 - s->by0 + 1) + 34;
    return true;
}

bool vg_console_key_at(int n, float x, float y) {
    int kx, ky, kw, kh;
    if (!key_rect(n, &kx, &ky, &kw, &kh)) return false;
    return vg_in_rect(x, y, kx, ky, kw, kh);
}

void vg_console_key(int n, const char* label, bool live) {
    const VgBezelSlot* s = vg_bezel_slot(n);
    int kx, ky, kw, kh;
    if (!s || !key_rect(n, &kx, &ky, &kw, &kh)) return;

    // THE CHASSIS ALREADY DREW THE BOX. vg_button paints a well, a 2px frame and
    // corner ticks, and every one of those is a second border inside the lit
    // window the metal provides -- a button drawn on a button. What is left of a
    // key when the machine owns its box is the label, the line under it, and the
    // change under a thumb.
    //
    // Lit from the HIT rect, not the drawn one. A press that will register has to
    // light the key, or the player learns the key is unreliable when what is
    // really happening is that it lights on a smaller area than it accepts.
    const bool down = vg_press_in(kx, ky, kw, kh);

    // THE BIGGEST SIZE THE HOLE WILL TAKE. Down from 3, which is what a key
    // window wide enough for it gets; the broadcast rig's wells are 84px and
    // REPAIR at scale 3 is 108. Two pixels of margin either side, because a
    // label touching the bevel reads as overflowing it.
    const int iw = s->x1 - s->x0 + 1;
    const int ih = s->y1 - s->y0 + 1;
    int scale = 3;
    while (scale > 1 && (vg_text_width(label, scale) > iw - 4 ||
                         7 * scale + 6 > ih))
        scale--;

    const int  lw   = vg_text_width(label, scale);
    const int  lx   = s->x0 + (iw - lw) / 2;
    // The LABEL is centred, not the label and its underline together. Centring
    // the pair reads better and moves every existing key two pixels, which is a
    // change to two screens that were not being asked to change.
    const int  ly   = s->y0 + (ih - 7 * scale) / 2;

    // FLAT, WHEREVER THIS IS CALLED FROM. A key is set into the plating, not into
    // the glass, and the plating does not bend.
    //
    // It used to be drawn after the bracket closed, because the close and the key
    // were one function. Splitting them so a screen can have SEVERAL keys put the
    // key inside the bracket, and the curve took it apart in two ways at once: the
    // well is a fill and leaves as warped quads, the label is text and is warped
    // as one rigid block, so the two moved by different amounts and the black came
    // away from the word. And a two-pixel underline bent into chords loses pixels
    // where the chords meet, which is the uneven thickness.
    //
    // Neither is a bug in the warp. A key simply does not belong in it.
    // Null offsets: a key is set into the PLATING, and the plating does not move.
    vg_console_flat(0.0f, 0.0f, nullptr, nullptr);

    // A KEY THAT CANNOT BE TAKEN GOES DIM AND KEEPS ITS HIT TEST. The well does
    // not light under a thumb either: the press registers, and what the player is
    // being told is that pressing it will not help.
    fill_box(s, (down && live) ? INK_TRACE : INK_WELL);
    vg_text(lx, ly, label, live ? INK_MAX : INK_TRACE, scale);
    vg_fill_rect(lx, ly + 7 * scale + 3, lw, 2,
                 live ? (down ? INK_MAX : INK_BRIGHT) : INK_TRACE);

    vg_console_bend();
}
