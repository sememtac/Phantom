#include "vg_screens.h"
#include "vg_draw.h"
#include "vg_game.h"
#include <math.h>

// Ship select and pause. The tournament map is big enough to want its own file.

static void centred(int y, const char* s, uint16_t col, int scale) {
    vg_text((SCR_W - vg_text_width(s, scale)) / 2, y, s, col, scale);
}

void vg_button(int x, int y, int w, int h, const char* label,
               bool primary, bool live) {
    const uint16_t frame = !live    ? INK_TRACE
                         : primary  ? INK_BRIGHT
                                    : INK;
    const uint16_t ink   = !live    ? INK_TRACE
                         : primary  ? INK_MAX
                                    : INK_BRIGHT;

    // Same dark well the instrument panels sit in, so thin strokes keep their
    // contrast against a lit nebula.
    vg_fill_rect(x, y, w, h, INK_WELL);

    const int s = 2;
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
// Ship select
// ---------------------------------------------------------------------------

int vg_select_card_at(float x, float y) {
    for (int i = 0; i < SHIP_CLASSES; i++)
        if (vg_in_rect(x, y, SEL_CARD_X, SEL_CARD_Y0 + i * SEL_CARD_PITCH,
                       SEL_CARD_W, SEL_CARD_H))
            return i;
    return -1;
}

bool vg_select_confirm_at(float x, float y) {
    return vg_in_rect(x, y, SEL_GO_X, SEL_GO_Y, SEL_GO_W, SEL_GO_H);
}

// Four short bars per card. Absolute numbers would be meaningless before you
// have flown any of them; what a player needs at this screen is the SHAPE of the
// ship relative to the other three.
static void stat_bar(int x, int y, int w, float t, uint16_t col) {
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    vg_rect(x, y, w, 7, INK_FAINT);
    int fw = (int)((float)(w - 2) * t);
    if (fw > 0) vg_fill_rect(x + 1, y + 1, fw, 5, col);
}

void vg_draw_select(void) {
    centred(38, "SELECT SHIP", INK_MAX, 3);
    centred(72, "LOCKED FOR THE TOURNAMENT", INK_FAINT, 1);

    for (int i = 0; i < SHIP_CLASSES; i++) {
        const ShipSpec* s   = vg_spec((ShipClass)i);
        const int       y   = SEL_CARD_Y0 + i * SEL_CARD_PITCH;
        const bool      sel = ((int)vg.ship == i);

        vg_rect(SEL_CARD_X, y, SEL_CARD_W, SEL_CARD_H,
                sel ? INK_BRIGHT : INK_TRACE);
        if (sel) {
            // Inverse-video spine down the left edge rather than a filled card:
            // a solid block behind eight characters of amber is unreadable.
            vg_fill_rect(SEL_CARD_X, y, 6, SEL_CARD_H, INK_BRIGHT);
        }

        vg_text(SEL_CARD_X + 16, y + 8, s->name, sel ? INK_MAX : INK_BRIGHT, 3);
        // The description is the reason to pick one of these, so it is set at
        // reading size and on the bright ramp rather than tucked away faint.
        vg_text(SEL_CARD_X + 16, y + 40, s->tagline,
                sel ? INK_BRIGHT : INK, 2);

        // SPD / HUL / DMG / VOL, each normalised against the strongest class so
        // the bars are comparable down the column.
        const int bx = SEL_CARD_X + 300, bw = 100;
        stat_bar(bx, y + 8,  bw, (s->speed_max - 300.0f) / 180.0f,  INK_BRIGHT);
        stat_bar(bx, y + 22, bw, s->hull / 120.0f,                  INK_BRIGHT);
        stat_bar(bx, y + 36, bw, s->msl_damage / 44.0f,             INK_BRIGHT);
        stat_bar(bx, y + 50, bw,
                 ((float)s->magazine / s->reload) / 9.0f,           INK_BRIGHT);

        vg_text(bx - 26, y + 7,  "SPD", INK, 1);
        vg_text(bx - 26, y + 21, "HUL", INK, 1);
        vg_text(bx - 26, y + 35, "DMG", INK, 1);
        vg_text(bx - 26, y + 49, "VOL", INK, 1);
    }

    vg_button(SEL_GO_X, SEL_GO_Y, SEL_GO_W, SEL_GO_H, "ENTER", true, true);
}

// ---------------------------------------------------------------------------
// Pause
// ---------------------------------------------------------------------------

bool vg_pause_resume_at(float x, float y) {
    return vg_in_rect(x, y, PAU_BTN_X, PAU_RESUME_Y, PAU_BTN_W, PAU_BTN_H);
}

bool vg_pause_quit_at(float x, float y) {
    return vg_in_rect(x, y, PAU_BTN_X, PAU_QUIT_Y, PAU_BTN_W, PAU_BTN_H);
}

void vg_draw_pause(void) {
    // Knock the whole frame back a stop so the instruments read as suspended
    // rather than live, without hiding the fight you are about to return to.
    for (int y = 0; y < SCR_H; y += 2) vg_fill_rect(0, y, SCR_W, 1, COL_BLACK);

    centred(140, "PAUSED", INK_MAX, 5);

    vg_button(PAU_BTN_X, PAU_RESUME_Y, PAU_BTN_W, PAU_BTN_H, "RESUME", true,  true);
    vg_button(PAU_BTN_X, PAU_QUIT_Y,   PAU_BTN_W, PAU_BTN_H, "QUIT",   false, true);

    centred(370, "QUIT ABANDONS THE TOURNAMENT", INK_FAINT, 1);
    centred(392, "+/- RESUMES", INK_FAINT, 1);
}
