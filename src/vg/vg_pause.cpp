#include "vg_pause.h"
#include "vg_console.h"
#include "vg_states.h"
#include "vg_ui.h"
#include "vg_draw.h"
#include "vg_game.h"
#include "vg_course.h"
#include "vg_sfx.h"
#include <stdio.h>

// The pause screen and the config page behind it. Lifted out of what was then
// vg_screens.cpp -- ship select and this sharing nothing but a file name, and
// the tournament table was about to make it three. That file is vg_select.cpp
// now, and every screen has its own pair; see vg_menu.h.

// ---------------------------------------------------------------------------
// Pause
// ---------------------------------------------------------------------------

int vg_pause_items(bool skippable, PauseItem* out) {
    int n = 0;
    out[n++] = PAUSE_RESUME;
    out[n++] = PAUSE_CONFIG;
    if (skippable) out[n++] = PAUSE_SKIP;
    out[n++] = PAUSE_QUIT;
    return n;
}

void vg_pause_rect(int i, int n, int* x, int* y, int* w, int* h) {
    const int total = n * PAU_BTN_H + (n - 1) * PAU_BTN_GAP;
    *x = PAU_BTN_X;
    *y = PAU_STACK_CY - total / 2 + i * (PAU_BTN_H + PAU_BTN_GAP);
    *w = PAU_BTN_W;
    *h = PAU_BTN_H;
}

PauseItem vg_pause_item_at(float px, float py, bool skippable) {
    PauseItem items[4];
    const int n = vg_pause_items(skippable, items);
    for (int i = 0; i < n; i++) {
        int x, y, w, h;
        vg_pause_rect(i, n, &x, &y, &w, &h);
        if (vg_in_rect(px, py, x, y, w, h)) return items[i];
    }
    return PAUSE_NONE;
}

// Generous vertically: a slider is dragged, and a finger that wanders off the
// track mid-drag should not silently stop moving it.
bool vg_pause_music_at(float x, float y) {
    return vg_in_rect(x, y, PAU_SLD_X, PAU_SLD_MUSIC_Y - 18, PAU_SLD_W, PAU_SLD_H + 36);
}
bool vg_pause_sfx_at(float x, float y) {
    return vg_in_rect(x, y, PAU_SLD_X, PAU_SLD_SFX_Y - 18, PAU_SLD_W, PAU_SLD_H + 36);
}
bool vg_pause_back_at(float x, float y) {
    return vg_in_rect(x, y, PAU_BTN_X, PAU_BACK_Y, PAU_BTN_W, PAU_BTN_H);
}
// The whole row, not just the box. A 26 px square is a hard thing to hit with a
// thumb, and the label is part of the control as far as the player is concerned.
bool vg_pause_scanline_at(float x, float y) {
    return vg_in_rect(x, y, PAU_SLD_X, PAU_CHK_Y - 6, PAU_SLD_W, PAU_CHK_SIZE + 12);
}
float vg_pause_slider_value(float x) {
    float v = (x - (float)PAU_SLD_X) / (float)PAU_SLD_W;
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    return v;
}

// A box and a label, drawn as one row because that is what it is to a thumb.
// Ticked with a cross rather than a fill: at this size a filled square and an
// empty one read alike across the room, and two crossing lines do not.
static void check_row(int y, const char* label, bool on) {
    vg_text(PAU_SLD_X + PAU_CHK_SIZE + 14, y + 4, label, INK, 2);

    const int x = PAU_SLD_X;
    vg_fill_rect(x, y, PAU_CHK_SIZE, PAU_CHK_SIZE, INK_WELL);
    vg_rect(x, y, PAU_CHK_SIZE, PAU_CHK_SIZE, INK);
    if (on) {
        const float a = (float)(x + 6),  b = (float)(y + 6);
        const float c = (float)(x + PAU_CHK_SIZE - 6);
        const float d = (float)(y + PAU_CHK_SIZE - 6);
        vg_line(a, b, c, d, INK_BRIGHT);
        vg_line(a, d, c, b, INK_BRIGHT);
    }
}

static void volume_slider(int y, const char* label, float v) {
    vg_text(PAU_SLD_X, y - 26, label, INK, 2);

    char buf[8];
    snprintf(buf, sizeof(buf), "%d", (int)(v * 100.0f + 0.5f));
    vg_text(PAU_SLD_X + PAU_SLD_W - vg_text_width(buf, 2), y - 26, buf, INK_BRIGHT, 2);

    vg_fill_rect(PAU_SLD_X, y, PAU_SLD_W, PAU_SLD_H, INK_WELL);
    vg_rect(PAU_SLD_X, y, PAU_SLD_W, PAU_SLD_H, INK);
    const int fill = (int)((float)(PAU_SLD_W - 4) * v);
    if (fill > 0) vg_fill_rect(PAU_SLD_X + 2, y + 2, fill, PAU_SLD_H - 4, INK_BRIGHT);
}

void vg_draw_pause(void) {
    // Knock the whole frame back a stop so what is behind reads as suspended
    // rather than live, without hiding the thing you are about to return to.
    //
    // INK_WELL, NOT COL_BLACK, AND THIS HAS NEVER DRAWN A ROW. A fill whose
    // colour is zero is DROPPED -- fill_rect_raw returns on `!color`, the same
    // rule that makes black text invisible -- so every one of these two hundred
    // and forty fills has been discarded at submit since the day it was written.
    //
    // FOURTH TIME. The header band and the footer band on the tournament sheet
    // were the same mistake, and so were two window fills and a slider surround
    // in the console. It hides so well because a dim that does nothing looks
    // exactly like a dim that is too subtle, and in flight there is a dark
    // cockpit behind it either way. It only became obvious once PWR started
    // working over the MENUS, where what is behind is bright.
    //
    // A PAGE IS KNOCKED BACK. A FLIGHT IS NOT, AND CANNOT BE FROM HERE.
    //
    // The dim is a primitive, and in flight the CANOPY is applied to the finished
    // band after every primitive has been drawn -- it is a light delta, not a
    // shape. So a dim submitted with the rest of the picture is darkened first
    // and then has the cockpit's light added back on top of it, and the frame
    // comes out as canopy-white rather than as an amber picture turned down. It
    // was flown and it looks like a fault.
    //
    // Dimming after the canopy would mean dimming in the BAND pass, which runs
    // after everything -- including this menu, which is the one thing that must
    // stay lit.
    //
    // So: pages only, which is what was asked for and where the problem was. A
    // paused match already reads as suspended -- nothing moves, and a menu is
    // sitting on top of it.
    if (vg_state_is_menu(vg_state_shown())) {

    // EVERYTHING, PLATING INCLUDED. The chassis was left alone at first, on the
    // reading that the metal is not something showing through -- it is what the
    // display is mounted in. Flown, that is the wrong call: what a pause suspends
    // is the whole machine, and a crisp bezel around a knocked-back screen reads
    // as the picture having failed rather than as the game having stopped.
    //
    // THREE ROWS ON, ONE OFF. The knock is coverage, not colour -- INK_WELL is
    // already all but black, so what decides how hard this reads is the fraction
    // of rows it takes. Two on and two off was half, and half was not enough to
    // stop the page competing with the menu over it.
    //
    // AND THE PERIOD STAYS AT FOUR, so this still costs 120 primitives. Slice 3
    // does not have more to give: measured over the tournament sheet -- a warped
    // menu, which the slice budget names as its worst case -- a single-row comb
    // at 240 peaked at 910 of 950 and threw a primitive away on one frame. An
    // overflow drops whatever was submitted LAST, which here is the pause menu
    // itself. The dim would have eaten the thing it was drawn for.
    for (int y = 0; y < SCR_H; y += 4) vg_fill_rect(0, y, SCR_W, 3, INK_WELL);
    }

    if (vg.pause_page == 1) {
        vg_centred(120, "CONFIG", INK_MAX, 5);
        volume_slider(PAU_SLD_MUSIC_Y, "MUSIC", vg_vol.music);
        volume_slider(PAU_SLD_SFX_Y,   "SFX",   vg_vol.sfx);
        check_row(PAU_CHK_Y, "SCANLINES", vg_disp.scanlines);
        vg_button(PAU_BTN_X, PAU_BACK_Y, PAU_BTN_W, PAU_BTN_H, "BACK", true, true);
        return;
    }

    vg_centred(120, "PAUSED", INK_MAX, 5);

    // SKIP is drawn DEAD, not hidden, while the broadcast is still talking. A
    // button that is missing reads as a game that forgot it; one that is visibly
    // unavailable reads as a rule, and the player watches it become available
    // rather than discovering it by trying.
    const bool skippable = (vg.pause_from == VG_COURSE);

    PauseItem items[4];
    const int n = vg_pause_items(skippable, items);
    for (int i = 0; i < n; i++) {
        int x, y, w, h;
        vg_pause_rect(i, n, &x, &y, &w, &h);
        switch (items[i]) {
        case PAUSE_RESUME:
            vg_button(x, y, w, h, "RESUME", true, true);  break;
        case PAUSE_CONFIG:
            vg_button(x, y, w, h, "CONFIG", false, true); break;
        case PAUSE_SKIP:
            vg_button(x, y, w, h, "SKIP",   false, vg_course.named); break;
        case PAUSE_QUIT:
            vg_button(x, y, w, h, "QUIT",   false, true); break;
        default: break;
        }
    }
}

