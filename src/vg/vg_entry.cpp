#include "vg_screens.h"
#include "vg_draw.h"
#include "vg_game.h"
#include <math.h>

// ===========================================================================
// Callsign and trail colour.
//
// Three letter wheels rather than a keyboard: a 26-key grid on a 480x480 panel
// gives ~60px keys with no room for anything else, and three characters is far
// too few to be worth that. A wheel is one gesture per character and reads as a
// piece of hardware, which suits the rest of the interface.
//
// The colour picker exposes HUE ONLY. Saturation and value are pinned, because
// a chooser that let you drop either would let a player select a trail they
// cannot see -- and the trail is the thing that says which contact is you.
// ===========================================================================

#define WHEEL_STEP   26.0f      // px of drag per letter

static int   s_letter[3]  = { 0, 0, 0 };   // 0..25
static int   s_drag_col   = -1;
static float s_accum      = 0.0f;
static bool  s_hue_open   = false;
static bool  s_hue_drag   = false;

void vg_entry_reset(void) {
    // Seed the wheels from the last callsign so a second run does not start
    // from AAA again.
    for (int i = 0; i < 3; i++) {
        char c = vg.callsign[i];
        s_letter[i] = (c >= 'A' && c <= 'Z') ? (c - 'A') : 0;
    }
    s_drag_col = -1;
    s_accum    = 0.0f;
    s_hue_open = false;
    s_hue_drag = false;
}

static void commit(void) {
    for (int i = 0; i < 3; i++) vg.callsign[i] = (char)('A' + s_letter[i]);
    vg.callsign[3] = 0;
}

static int column_at(float x, float y) {
    if (y < ENT_WHEEL_Y || y >= ENT_WHEEL_Y + ENT_WHEEL_H) return -1;
    int c = (int)((x - (float)ENT_COL_X0) / (float)ENT_COL_W);
    if (c < 0 || c > 2) return -1;
    return c;
}

static void set_hue_from(float x) {
    float t = (x - (float)ENT_HUE_X) / (float)ENT_HUE_W;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    vg.trail_hue = t;
}

bool vg_entry_update(const VgInput* in, bool tap, float tx, float ty) {
    // --- press: decide what this contact is going to be ---
    if (in->menu_edge) {
        s_accum    = 0.0f;
        s_drag_col = column_at(in->menu_x, in->menu_y);
        // Generous grab pad. The ramp is a coarse choice and the fingertip
        // covers most of its height, so demanding a hit inside a 54px strip made
        // it feel stuck rather than precise. Once latched the drag tracks x
        // alone, so the finger may wander off the bar entirely.
        //
        // The pad must NOT swallow the TRAIL button above it. It did, and the
        // result was that closing the picker set the hue from wherever the
        // button had been pressed -- which for a centred tap is mid-ramp, so
        // every close snapped the colour to cyan.
        const bool on_trail_btn = vg_in_rect(in->menu_x, in->menu_y,
                                             ENT_TRAIL_X, ENT_TRAIL_Y,
                                             ENT_TRAIL_W, ENT_TRAIL_H);
        s_hue_drag = s_hue_open && !on_trail_btn &&
                     vg_in_rect(in->menu_x, in->menu_y,
                                ENT_HUE_X - ENT_HUE_PAD,
                                ENT_HUE_Y - ENT_HUE_PAD,
                                ENT_HUE_W + 2 * ENT_HUE_PAD,
                                ENT_HUE_H + 2 * ENT_HUE_PAD);
        if (s_hue_drag) {
            s_drag_col = -1;          // the ramp wins over any wheel underneath
            set_hue_from(in->menu_x);
        }
    }

    if (in->menu_held) {
        if (s_hue_drag) {
            set_hue_from(in->menu_x);
        } else if (s_drag_col >= 0) {
            // Dragging DOWN rolls the wheel down, which brings earlier letters
            // up into view -- the way a physical wheel behaves.
            s_accum += in->menu_dy;
            while (s_accum >= WHEEL_STEP) {
                s_accum -= WHEEL_STEP;
                s_letter[s_drag_col] = (s_letter[s_drag_col] + 25) % 26;
            }
            while (s_accum <= -WHEEL_STEP) {
                s_accum += WHEEL_STEP;
                s_letter[s_drag_col] = (s_letter[s_drag_col] + 1) % 26;
            }
            commit();
        }
    } else {
        s_drag_col = -1;
        s_hue_drag = false;
    }

    // --- taps ---
    if (tap) {
        int c = column_at(tx, ty);
        if (c >= 0) {
            // A tap above or below the centre letter nudges by one, so the
            // wheels are usable without a drag at all.
            const float mid = (float)ENT_WHEEL_Y + (float)ENT_WHEEL_H * 0.5f;
            if (ty < mid - 24.0f)      s_letter[c] = (s_letter[c] + 25) % 26;
            else if (ty > mid + 24.0f) s_letter[c] = (s_letter[c] + 1)  % 26;
            commit();
        } else if (vg_in_rect(tx, ty, ENT_TRAIL_X, ENT_TRAIL_Y,
                              ENT_TRAIL_W, ENT_TRAIL_H)) {
            s_hue_open = !s_hue_open;
        } else if (s_hue_open &&
                   vg_in_rect(tx, ty, ENT_HUE_X - ENT_HUE_PAD,
                              ENT_HUE_Y,
                              ENT_HUE_W + 2 * ENT_HUE_PAD,
                              ENT_HUE_H + ENT_HUE_PAD)) {
            set_hue_from(tx);
        } else if (vg_in_rect(tx, ty, ENT_GO_X, ENT_GO_Y, ENT_GO_W, ENT_GO_H)) {
            commit();
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------

static void centred(int y, const char* s, uint16_t col, int scale) {
    vg_text((SCR_W - vg_text_width(s, scale)) / 2, y, s, col, scale);
}

void vg_draw_entry(void) {
    centred(30, "CALLSIGN", INK_MAX, 3);

    const int midy = ENT_WHEEL_Y + ENT_WHEEL_H / 2;

    for (int c = 0; c < 3; c++) {
        const int cx = ENT_COL_X0 + c * ENT_COL_W + ENT_COL_W / 2;

        // Neighbours, dimmer the further they are from the detent -- the whole
        // point of a wheel is that you can see what is coming.
        for (int k = -2; k <= 2; k++) {
            if (k == 0) continue;
            int      l   = (s_letter[c] + k + 52) % 26;
            char     s[2] = { (char)('A' + l), 0 };
            int      sc  = (k == -1 || k == 1) ? 3 : 2;
            uint16_t col = (k == -1 || k == 1) ? INK_TRACE : INK_ONFILL;
            vg_text(cx - vg_text_width(s, sc) / 2,
                    midy + k * 34 - (sc * 7) / 2, s, col, sc);
        }

        char s[2] = { (char)('A' + s_letter[c]), 0 };
        vg_text(cx - vg_text_width(s, 6) / 2, midy - 21, s, INK_MAX, 6);
    }

    // Detent rails: two rules bracketing the selected row, so the wheels read as
    // one mechanism rather than three floating letters.
    vg_fill_rect(ENT_COL_X0, midy - 26, 3 * ENT_COL_W, 1, INK_TRACE);
    vg_fill_rect(ENT_COL_X0, midy + 25, 3 * ENT_COL_W, 1, INK_TRACE);

    // --- trail colour ---
    const uint16_t hue = vg_hue_col(vg.trail_hue);

    vg_rect(ENT_TRAIL_X, ENT_TRAIL_Y, ENT_TRAIL_W, ENT_TRAIL_H,
            s_hue_open ? INK_MAX : INK_TRACE);
    vg_text(ENT_TRAIL_X + 14, ENT_TRAIL_Y + 15, "TRAIL", INK_BRIGHT, 2);
    // The swatch is drawn as a short streak rather than a block: it is standing
    // in for a trail, and a trail is what the colour is actually for.
    vg_fill_rect(ENT_TRAIL_X + 110, ENT_TRAIL_Y + 18, 150, 10, hue);

    if (s_hue_open) {
        // The ramp itself, one column per pixel. 400 fills at ~1px wide is
        // nothing next to a frame of world geometry, and it beats any number of
        // discrete swatches for showing that hue is continuous.
        for (int i = 0; i < ENT_HUE_W; i += 2)
            vg_fill_rect(ENT_HUE_X + i, ENT_HUE_Y, 2, ENT_HUE_H,
                         vg_hue_col((float)i / (float)ENT_HUE_W));

        // A wide handle, not a hairline: it has to be findable under a thumb.
        int mx = ENT_HUE_X + (int)(vg.trail_hue * (float)ENT_HUE_W);
        if (mx < ENT_HUE_X + 7)               mx = ENT_HUE_X + 7;
        if (mx > ENT_HUE_X + ENT_HUE_W - 7)   mx = ENT_HUE_X + ENT_HUE_W - 7;
        vg_fill_rect(mx - 7, ENT_HUE_Y - 9, 15, ENT_HUE_H + 18, COL_BLACK);
        vg_fill_rect(mx - 5, ENT_HUE_Y - 7, 11, ENT_HUE_H + 14, INK_MAX);
        vg_fill_rect(mx - 2, ENT_HUE_Y - 4,  5, ENT_HUE_H + 8,  hue);
    } else {
        centred(ENT_HUE_Y + 18, "TAP TRAIL TO CHANGE COLOUR", INK, 2);
    }

    vg_button(ENT_GO_X, ENT_GO_Y, ENT_GO_W, ENT_GO_H, "NEXT", true, true);
}
