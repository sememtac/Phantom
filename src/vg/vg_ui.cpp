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
