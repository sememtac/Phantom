#include "vg_select.h"
#include "vg_ui.h"
#include "vg_shipview.h"
#include "vg_bezel.h"
#include "vg_console.h"
#include "generated/bezel_console.h"
#include "vg_sfx.h"
#include "vg_course.h"
#include "vg_draw.h"
#include "vg_game.h"
#include <math.h>

// Ship select: the wheel on the left, and the machine it is bolted into. What
// the panel on the right says about a class is vg_shipview.

// THE WHEEL, and it is the same object the hit test and the drawing both use.
// The two used to carry a copy of this arithmetic each, which is how a tap came
// to land 19px from the row the eye had picked. See VgWheel.
static VgWheel s_wheel = {
    SEL_WHEEL_X, SEL_WHEEL_W,
    SEL_WHEEL_Y, SEL_WHEEL_H,
    SEL_WHEEL_DETENT, SEL_WHEEL_PITCH,
    SEL_WHEEL_LO, SEL_WHEEL_N,
    0.0f,
};

VgWheel* vg_select_wheel(void) { return &s_wheel; }

int vg_select_row_at(float x, float y) {
    return vg_wheel_row_at(&s_wheel, x, y);
}

// HOW BRIGHT A ROW'S MARKER IS, by its distance from the detent.
//
// THE WHOLE LADDER, 100 down to 30, and the width of it is the point. The first
// version ran 90 / 70 / 50 -- the same rungs the NAMES use, so that a row's mark
// and its name would agree about how far out they were -- and read as three bars
// of much the same weight. What a gradient has to do here is answer "which row am
// I on" before anything is read, and 20 points a rung is not enough of an answer.
//
// So the marks are not tied to the names any more. The names still run MAX / INK /
// FAINT because they have to stay READABLE at every distance: a name dimmed to
// TRACE is a class you cannot see, and the argument for showing the whole roster
// was that a chooser should not hide your options. A mark has no such duty. It
// carries no information of its own, so it is free to go almost out.
//
// Three rungs is the whole range this wheel can ask for. The window is capped at
// SEL_WHEEL_SHOWN = 5 and it is centred, so the furthest row is two from the
// detent however long the roster gets; the fallback is for a wider window.
static uint16_t spine_ink(int a) {
    switch (a) {
    case 0:  return INK_MAX;
    case 1:  return INK_FAINT;
    case 2:  return INK_TRACE;
    }
    return INK_TRACE;
}

bool vg_select_confirm_at(float x, float y) {
    return vg_console_key_at(VG_CON_KEY, x, y);
}

// THE PANEL'S MEMORY, and this screen owns it because this screen is the one
// showing a panel. One selection, one panel, one tween.
static VgShipView s_view = { -1, -1, -1.0f, { 0, 0, 0, 0, 0 } };


void vg_draw_select(void) {
    const bool opp = (vg.gym && vg.sel_opp);
    const int  cur = opp ? (int)vg.gym_opp : (int)vg.ship;

    vg_console_open(&BEZEL_CONSOLE,
                    vg.gym ? (opp ? "SELECT OPPONENT" : "SELECT YOUR SHIP")
                           : "SELECT SHIP",
                    vg.gym ? (opp ? "THEY RESPAWN UNTIL YOU LEAVE"
                                  : "PRACTICE -- NOTHING IS SCORED")
                           : nullptr);

    // --- the wheel ---------------------------------------------------------
    vg_rect(SEL_WHEEL_X, SEL_WHEEL_Y, SEL_WHEEL_W, SEL_WHEEL_H, INK_TRACE);

    // EVERY CHOICE, dimming with distance from the detent.
    //
    // It used to show one either side, to dodge the fact that a four-item wheel
    // asked for two either side shows the SAME class above and below. But hiding
    // an option to avoid drawing it twice is the wrong trade -- a chooser that
    // conceals two of your four is asking you to remember them.
    //
    // So the window is the roster, capped at SEL_WHEEL_SHOWN. Four rows means one
    // above the detent and two below, which is what an even count in an odd window
    // gives you; it is stable, and every class appears exactly once.
    const int shown_n  = SEL_WHEEL_N;
    const int shown_lo = SEL_WHEEL_LO;
    const int detent   = SEL_WHEEL_DETENT;

    // THE ROW UNDER THE THUMB, LIT. The wheel is a control like any other and it
    // had the same fault the keys had: a tap that nudges the wheel by one gave no
    // sign that the tap landed, so a press near a row border felt like the screen
    // ignoring you rather than like a miss.
    //
    // It uses vg_select_row_at rather than a rectangle of its own, because that
    // function IS where a tap resolves to a row. A second copy of the arithmetic
    // here would light one row while the tap moved to another, and the two would
    // disagree only near the borders -- which is exactly where it matters.
    float pxr, pyr;
    const int lit = vg_press_get(&pxr, &pyr) ? vg_select_row_at(pxr, pyr)
                                             : VG_WHEEL_NONE;
    //
    // IT STOPS AT THE MARKER COLUMN and does not run under it. The highlight is
    // INK_TRACE, and so is the furthest marker -- a full-width lift would erase
    // the mark on exactly the row a thumb was pressing, which is the one moment
    // the mark is being looked at. The marker keeps the dark screen behind it and
    // stays legible at every rung.
    if (lit != VG_WHEEL_NONE)
        vg_fill_rect(SEL_WHEEL_X + SEL_SPINE_W,
                     vg_wheel_row_y(&s_wheel, lit) - 18,
                     SEL_WHEEL_W - SEL_SPINE_W, 36, INK_TRACE);

    for (int k = shown_lo; k < shown_lo + shown_n; k++) {
        if (k == 0) continue;                 // the detent is drawn over the rails
        const int   a   = (k < 0) ? -k : k;
        const int   idx = (cur + k + SHIP_CLASSES * 4) % SHIP_CLASSES;
        const char* nm  = vg_spec((ShipClass)idx)->name;
        // Readable at every step, just quieter. Faded to a hint would defeat the
        // point of showing them at all.
        // EVERY NEIGHBOUR AT SCALE 2, where the far ones used to drop to 1. On
        // the device scale 1 is 0.57mm and simply cannot be read, so a row drawn
        // at it is a row that is not there -- and the whole argument for showing
        // the entire roster was that a chooser should not hide your options.
        // Distance is carried by ink alone now, which is what it should have been
        // carrying all along.
        const int      sc  = 2;
        const int      tr  = SEL_NAME_TRACK;
        const uint16_t col = (a == 1) ? INK : INK_FAINT;
        vg_text_track(SEL_WHEEL_X
                          + (SEL_WHEEL_W - vg_text_track_width(nm, sc, tr)) / 2,
                      vg_wheel_row_y(&s_wheel, k) - (sc * 7) / 2,
                      nm, col, sc, tr);
    }

    // The detent: two rules bracketing the selected row, plus the spine down the
    // left edge. That spine is the same 6px inverse-video mark the cards carried
    // -- the shape of the screen changed, the vocabulary did not.
    // The detent grew with the name it brackets: a 21px glyph inside a 36px well
    // leaves three pixels top and bottom, which reads as the row being too small
    // for its own word rather than as a selection.
    vg_fill_rect(SEL_WHEEL_X, detent - SEL_SPINE_H / 2,
                 SEL_WHEEL_W, 1, INK_TRACE);
    vg_fill_rect(SEL_WHEEL_X, detent + SEL_SPINE_H / 2 - 1,
                 SEL_WHEEL_W, 1, INK_TRACE);

    // THE MARKER, ON EVERY ROW.
    //
    // It used to be the selection's alone, and a mark that only one row carries is
    // a mark you have to go looking for to learn what it means. Every row has one
    // now and the BRIGHTNESS is what says which row you are on -- a column of them
    // fading either side of the detent, so the hierarchy is read as a gradient
    // rather than found as the one row that is different.
    //
    // LAST, over the rails, which is where the single spine already was. The rails
    // run the full width and cross the marker's top and bottom rows, so drawing
    // these first would let INK_TRACE cut the ends off every one of them.
    //
    // The selected row is unchanged by this: the same bar, the same height, the
    // same ink, in the same order. What changed is what is underneath it.
    for (int k = shown_lo; k < shown_lo + shown_n; k++) {
        const int a = (k < 0) ? -k : k;
        vg_fill_rect(SEL_WHEEL_X, vg_wheel_row_y(&s_wheel, k) - SEL_SPINE_H / 2,
                     SEL_SPINE_W, SEL_SPINE_H, spine_ink(a));
    }

    // THE SELECTED NAME AT SCALE 3. It is the one word on this screen you are
    // actually choosing between, and at scale 2 it was 1.13mm on the device --
    // legible, and no larger than the specification beside it, which had the
    // hierarchy the wrong way round.
    // CENTRED IN WHAT IS LEFT, not in the box. The spine is 6px of solid ink hard
    // against the left edge, so a name centred on the whole width sits nine pixels
    // from it -- which measures as clear and reads as joined, because the first
    // upright of a B nine pixels from a bar looks like part of the bar.
    //
    // And no tracking on this one. At scale 3 the font's own bearing is three
    // pixels, which is enough; the tracking was there for scale 2, where it was
    // not. Dropping it is what makes the margins possible at all -- BALLISTA is
    // 155px tracked against 141 plain, in a box with 152 to give once the spine
    // and the margins are taken out.
    {
        const char* nm = vg_spec((ShipClass)cur)->name;
        const int   x0 = SEL_WHEEL_X + SEL_SPINE_W + 10;
        const int   room = SEL_WHEEL_W - SEL_SPINE_W - 18;
        vg_text(x0 + (room - vg_text_width(nm, 3)) / 2, detent - 10, nm, INK_MAX, 3);
    }

    // --- the panel ---------------------------------------------------------
    //
    // THE WHOLE RIGHT-HAND SIDE IS ONE WIDGET NOW. The name, the two fields, the
    // chart and the hull from above were written out here, which meant the only
    // way for another screen to say what a class IS was to copy them. See
    // vg_shipview.h.
    //
    // The panel's rectangle is still this screen's decision -- the wheel takes
    // the left strip and the panel gets what is left -- and everything inside it
    // is the widget's.
    vg_shipview_draw(&s_view, cur, SEL_PANEL_X, SEL_PANEL_Y,
                     SEL_PANEL_W, SEL_PANEL_H);

    vg_console_key(VG_CON_KEY, (vg.gym && !vg.sel_opp) ? "NEXT" : "ENTER", true);
    vg_console_close();
}
