#include "vg_entry.h"
#include "vg_ui.h"
#include "vg_console.h"
#include "generated/bezel_console.h"
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

static int   s_letter[3]  = { 0, 0, 0 };   // 0..25
static int   s_drag_col   = -1;

// ONE WHEEL FOR WHICHEVER COLUMN IS UNDER THE THUMB. The three are identical to
// a finger and only one can be dragged at a time, so this carries the drag and
// not the geometry -- the letters' own hit test is below, and it keeps a wider
// dead zone than a wheel's own row test would give it. See the tap.
static VgWheel s_drag = { 0, 0, 0, 0, 0, 0, 0, 0, 0.0f };
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
    vg_wheel_release(&s_drag);
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
    vg.trail_hue = t * ENT_HUE_SPAN;
}

bool vg_entry_update(const VgInput* in, bool tap, float tx, float ty) {
    // --- press: decide what this contact is going to be ---
    if (in->menu_edge) {
        vg_wheel_release(&s_drag);
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
            // The direction and the detent size are the wheel's -- see VgWheel.
            const int d = vg_wheel_drag(&s_drag, in->menu_dy);
            s_letter[s_drag_col] =
                ((s_letter[s_drag_col] + d) % 26 + 26) % 26;
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
            //
            // NOT vg_wheel_row_at, and the difference is the dead zone. That
            // function splits at half a pitch, which here would be 17px; this
            // wants 24, because three columns stand side by side and a thumb
            // that lands near the middle of one should do nothing rather than
            // nudge whichever half of the row it was closest to.
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
        } else if (vg_console_key_at(VG_CON_KEY, tx, ty)) {
            commit();
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------

void vg_draw_entry(void) {
    // The same terminal the ship select is bolted into. REGISTRATION rather than
    // CALLSIGN: the banner is the tournament's own wording for what this desk is
    // for, and the word on the glass below is what you are entering.
    vg_console_open(&BEZEL_CONSOLE, VG_CON_TERMINAL, "CALLSIGN REGISTRATION",
                    nullptr);

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
        // TWO HUNDRED THIN FILLS IS A RAMP; EIGHT HUNDRED WARPED QUADS IS A BUG.
        //
        // A warped fill leaves as a strip of quads, two triangles each, and a
        // 2px column thirty tall is still long enough to be subdivided -- so the
        // ramp alone asked for roughly 800 primitives against an instrument slice
        // with about 250 left in it. It overflowed, and an overflow drops
        // whatever was submitted LAST: the chassis went, the key went with it,
        // and the handle -- drawn after the ramp -- never appeared at all. The
        // panel did not disappear. It ran out of room.
        //
        // A RAMP IS ONE OBJECT, so it bends as one, exactly as a word does. The
        // curve is sampled once at the strip's centre and the whole control is
        // translated there rigidly. That is also the more honest reading of what
        // this is: a flat part set into the console, not something painted on the
        // tube. vg_hud_warp_at answers "where did the panel put this spot" without
        // opening the bracket, which is what the rear-view patch uses it for.
        int dx, dy;
        vg_console_flat((float)ENT_HUE_X + (float)ENT_HUE_W * 0.5f,
                        (float)ENT_HUE_Y + (float)ENT_HUE_H * 0.5f, &dx, &dy);

        // The ramp itself, one column per two pixels. It beats any number of
        // discrete swatches for showing that hue is continuous.
        for (int i = 0; i < ENT_HUE_W; i += 2)
            vg_fill_rect(ENT_HUE_X + i + dx, ENT_HUE_Y + dy, 2, ENT_HUE_H,
                         vg_hue_col((float)i / (float)ENT_HUE_W * ENT_HUE_SPAN));

        // A wide handle, not a hairline: it has to be findable under a thumb.
        // Divided by the span, so the handle sits under the colour it selected. A hue
        // from an older profile can exceed the span; the clamp below pins it to the end.
        int mx = ENT_HUE_X + (int)(vg.trail_hue / ENT_HUE_SPAN * (float)ENT_HUE_W);
        if (mx < ENT_HUE_X + 7)               mx = ENT_HUE_X + 7;
        if (mx > ENT_HUE_X + ENT_HUE_W - 7)   mx = ENT_HUE_X + ENT_HUE_W - 7;
        mx += dx;
        // INK_ONFILL and not COL_BLACK: a fill whose colour is zero is dropped,
        // so the dark surround the handle needs was never being drawn.
        vg_fill_rect(mx - 7, ENT_HUE_Y - 9 + dy, 15, ENT_HUE_H + 18, INK_ONFILL);
        vg_fill_rect(mx - 5, ENT_HUE_Y - 7 + dy, 11, ENT_HUE_H + 14, INK_MAX);
        vg_fill_rect(mx - 2, ENT_HUE_Y - 4 + dy,  5, ENT_HUE_H + 8,  hue);

        vg_console_bend();
    }

    vg_console_key(VG_CON_KEY, "NEXT", true);
    vg_console_close();
}
