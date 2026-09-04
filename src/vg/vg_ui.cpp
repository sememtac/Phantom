#include "vg_ui.h"
#include "vg_draw.h"
#include "vg_config.h"
#include <math.h>

// The live contact, set once a frame by vg_state_update. See vg_ui.h.
static bool  s_press_held = false;
static float s_press_x = 0.0f, s_press_y = 0.0f;

void vg_press_set(bool held, float x, float y) {
    s_press_held = held;
    s_press_x = x;
    s_press_y = y;
}

void vg_press_claim(void) {
    s_press_held = false;
}

bool vg_press_get(float* x, float* y) {
    if (x) *x = s_press_x;
    if (y) *y = s_press_y;
    return s_press_held;
}

bool vg_press_in(int x, int y, int w, int h) {
    return s_press_held && vg_in_rect(s_press_x, s_press_y, x, y, w, h);
}

void vg_button(int x, int y, int w, int h, const char* label,
               bool primary, bool live) {
    const uint16_t frame0 = !live   ? INK_TRACE
                         : primary  ? INK_BRIGHT
                                    : INK;
    const uint16_t ink   = !live    ? INK_TRACE
                         : primary  ? INK_MAX
                                    : INK_BRIGHT;

    // LIT WHILE HELD. A key that does not change under the thumb is a key you
    // cannot tell you hit, and on a touch panel that reads as the machine having
    // missed the press rather than as your finger having missed the key.
    //
    // The well brightens and the frame goes to full ink. Deliberately the WELL
    // and not the label: a brighter label on an unchanged ground reads as a
    // value that changed, and this is a control reporting contact, not a
    // readout reporting news.
    const bool down = live && vg_press_in(x, y, w, h);

    // Same dark well the instrument panels sit in, so thin strokes keep their
    // contrast against a lit nebula.
    vg_fill_rect(x, y, w, h, down ? INK_TRACE : INK_WELL);

    const int s = 2;
    const uint16_t frame = down ? INK_MAX : frame0;
    vg_fill_rect(x,         y,         w, s, frame);
    vg_fill_rect(x,         y + h - s, w, s, frame);
    vg_fill_rect(x,         y,         s, h, frame);
    vg_fill_rect(x + w - s, y,         s, h, frame);

    // Corner ticks: short heavier runs at each corner. Cheap, and it is what
    // stops a plain rectangle reading as a placeholder.
    const int t = 10;
    vg_fill_rect(x,             y,             t, 4, frame);
    vg_fill_rect(x + w - t,     y,             t, 4, frame);
    vg_fill_rect(x,             y + h - 4,     t, 4, frame);
    vg_fill_rect(x + w - t,     y + h - 4,     t, 4, frame);

    const int lw = vg_text_width(label, 3);
    vg_text(x + (w - lw) / 2, y + (h - 21) / 2 - 3, label, ink, 3);

    // Key line under the label marks the primary action without filling the box.
    if (primary && live)
        vg_fill_rect(x + (w - lw) / 2, y + (h - 21) / 2 + 22, lw, 3, INK_BRIGHT);
}

// ---------------------------------------------------------------------------
// The ticker
// ---------------------------------------------------------------------------

void vg_ticker(VgRect fill, VgRect run, const char* text, const char* note,
               float t, int scale) {
    if (!text || fill.w <= 0 || fill.h <= 0 || run.w <= 0) return;

    // THE FILL RECT, NOT THE RUN. A window in chassis art is an octagon; a fill of
    // the rectangle that fits inside it leaves the four chamfered corners
    // unpainted, and the chassis does not cover them either -- they are exempt.
    // The sky showed in the corners. The fill overshoots onto metal, which the
    // chassis paints over on the way past.
    //
    // INK_WELL and not COL_BLACK: a fill whose colour is zero is DROPPED, the same
    // rule that makes black text invisible, so a COL_BLACK window was never being
    // cleared at all.
    vg_fill_rect(fill.x, fill.y, fill.w, fill.h, INK_WELL);

    // THE BIGGEST SIZE THE HOLE WILL TAKE, down from what the caller asked for.
    // Same rule as a key: a banner that only fits the window it was written
    // against is not a banner, it is a coincidence. Three pixels of margin top and
    // bottom, which is what is left of a 27px slot after a scale 3 glyph.
    while (scale > 1 && 7 * scale + 6 > run.h) scale--;

    const int tw    = vg_text_width(text, scale);
    const int cy    = run.y + (run.h - 1) / 2;

    // RIGHT TO LEFT, AND IT WRAPS. Moving right, the block's TAIL enters the
    // window first and the word arrives back to front -- the banner said SHIP
    // SELECT. And a single pass leaves the window empty between readings, which
    // looks like a machine that has stopped; repeating every word-plus-gap makes
    // the tail of one pass the head of the next.
    const int   period = tw + VG_TICKER_GAP;
    const float u      = t * VG_TICKER_RATE;
    const int   off    = (int)(u - floorf(u / (float)period) * (float)period);
    // CENTRED ON THE MIDDLE OF THE HOLE. It was a flat -10, which is exactly
    // right for a scale 3 glyph and three pixels too high for anything else --
    // the chyron at scale 2 sat with all its slack underneath it.
    //
    // A NOTE KEEPS ITS OWN NUMBER, because then the window holds two lines rather
    // than one and the banner is not what is being centred: it moves up to leave
    // room, and the pair is centred between them.
    const int   ty     = cy - (note ? 12 : (7 * scale) / 2);

    // Clipped to the FILL rect. In the console it is needed because the screen
    // aperture notches up either side of the headline bar, so there are exempt
    // pixels off both ends that the chassis cannot paint over -- text ran out of
    // the window and stayed on screen. Text obeys the viewport per pixel, so a
    // letter is cut mid-stroke rather than dropped whole.
    vg_rast_viewport(fill.x, fill.y, fill.w, fill.h);

    // WHITE, because the machine is not the one asking. A banner is the
    // tournament talking to you -- the same voice that speaks over a match -- so
    // it takes COL_IFT. Keys and furniture stay amber, and the difference between
    // the two is the point.
    for (int tx = run.x + run.w - off; tx + tw > run.x; tx -= period)
        vg_text(tx, ty, text, COL_IFT, scale);

    // A note does NOT run. It is a sentence to be read once, not a banner, and a
    // moving one would be the only thing on the screen asking to be chased.
    //
    // Centred on the RUN rect. It was centred on the screen, which happened to be
    // right for the one chassis that had a note and would have been wrong for any
    // window not in the middle of the panel.
    if (note)
        vg_text(run.x + (run.w - vg_text_width(note, 1)) / 2, cy + 4, note, INK, 1);

    vg_rast_viewport_full();
}

// ---------------------------------------------------------------------------
// The wheel
// ---------------------------------------------------------------------------
//
// A tap tests against the wheel, not against four cards. Tapping a neighbour
// nudges by one -- the screen has to stay usable without a drag, and the board
// has a hardware button too.

int vg_wheel_row_at(const VgWheel* wh, float x, float y) {
    if (!wh) return VG_WHEEL_NONE;
    if (!vg_in_rect(x, y, wh->x, wh->y, wh->w, wh->h)) return VG_WHEEL_NONE;

    const float dy = y - (float)wh->detent;
    int k = (int)lroundf(dy / (float)wh->pitch);
    if (k < wh->lo)                 k = wh->lo;
    if (k > wh->lo + wh->n - 1)     k = wh->lo + wh->n - 1;
    return k;
}

int vg_wheel_drag(VgWheel* wh, float dy) {
    if (!wh) return 0;
    wh->accum += dy;

    int steps = 0;
    while (wh->accum >=  WHEEL_STEP) { wh->accum -= WHEEL_STEP; steps--; }
    while (wh->accum <= -WHEEL_STEP) { wh->accum += WHEEL_STEP; steps++; }
    return steps;
}

void vg_wheel_release(VgWheel* wh) {
    if (wh) wh->accum = 0.0f;
}
