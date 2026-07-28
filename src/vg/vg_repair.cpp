#include "vg_screens.h"
#include "vg_draw.h"
#include "vg_game.h"
#include "vg_save.h"
#include <stdio.h>
#include <math.h>

// ===========================================================================
// Repair.
//
// The interesting decision here is never "repair or not", it is "twenty points
// now, or bank it for the semi-final where I know I will need it". So the
// control is a slider over a partial amount rather than a single BUY FULL
// button, and the cost updates under your thumb.
//
// If you cannot afford it, that is the game working as intended.
// ===========================================================================

static int s_buy = 0;    // hull points currently selected

// The most that could be bought right now: whichever runs out first, the damage
// or the money.
static int affordable(void) {
    int missing = (int)(vg.health_max - vg.health + 0.5f);
    if (missing < 0) missing = 0;
    int can_pay = vg.credits / CREDIT_PER_HULL;
    return (missing < can_pay) ? missing : can_pay;
}

void vg_repair_reset(void) { s_buy = 0; }

static void set_from(float x) {
    const int cap = affordable();
    if (cap <= 0) { s_buy = 0; return; }
    float t = (x - (float)REP_SLIDE_X) / (float)REP_SLIDE_W;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    s_buy = (int)(t * (float)cap + 0.5f);
}

bool vg_repair_update(const VgInput* in, bool tap, float tx, float ty) {
    if (s_buy > affordable()) s_buy = affordable();

    if (in->menu_held &&
        vg_in_rect(in->menu_x, in->menu_y, REP_SLIDE_X, REP_SLIDE_Y - 12,
                   REP_SLIDE_W, REP_SLIDE_H + 24))
        set_from(in->menu_x);

    if (tap) {
        if (vg_in_rect(tx, ty, REP_BUY_X, REP_BUY_Y, REP_BUY_W, REP_BUY_H)) {
            if (s_buy > 0) {
                vg.credits -= s_buy * CREDIT_PER_HULL;
                vg.health  += (float)s_buy;
                if (vg.health > vg.health_max) vg.health = vg.health_max;
                s_buy = 0;
                // Bank the spend. Hull is not persisted -- it belongs to a run,
                // not to the player -- so a reboot mid-tournament costs the
                // repair. Recording the deduction is the honest half of that.
                vg_save_store();
            }
        } else if (vg_in_rect(tx, ty, REP_BACK_X, REP_BACK_Y,
                              REP_BACK_W, REP_BACK_H)) {
            s_buy = 0;
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------

static void centred(int y, const char* s, uint16_t col, int scale) {
    vg_text((SCR_W - vg_text_width(s, scale)) / 2, y, s, col, scale);
}

void vg_draw_repair(void) {
    char buf[48];

    centred(28, "REPAIR", INK_MAX, 3);

    // --- hull bar: what you have, plus what you are about to buy ---
    vg_rect(REP_BAR_X, REP_BAR_Y, REP_BAR_W, REP_BAR_H, INK_TRACE);

    const float inner = (float)(REP_BAR_W - 6);
    const int   have  = (int)(inner * (vg.health / vg.health_max));
    const int   plus  = (int)(inner * ((float)s_buy / vg.health_max));

    if (have > 0)
        vg_fill_rect(REP_BAR_X + 3, REP_BAR_Y + 3, have, REP_BAR_H - 6, INK_BRIGHT);
    if (plus > 0) {
        // The purchase shows as hatching rather than solid fill, so it never
        // reads as hull you already own.
        for (int i = 0; i < plus; i += 3)
            vg_fill_rect(REP_BAR_X + 3 + have + i, REP_BAR_Y + 3, 1,
                         REP_BAR_H - 6, INK_FAINT);
        vg_rect(REP_BAR_X + 3 + have, REP_BAR_Y + 3, plus, REP_BAR_H - 6, INK);
    }

    snprintf(buf, sizeof(buf), "%s   %d / %d", vg.spec->name,
             (int)(vg.health + 0.5f), (int)(vg.health_max + 0.5f));
    centred(REP_BAR_Y + REP_BAR_H + 12, buf, INK_BRIGHT, 2);

    // --- slider ---
    const int cap = affordable();
    vg_rect(REP_SLIDE_X, REP_SLIDE_Y, REP_SLIDE_W, REP_SLIDE_H,
            cap > 0 ? INK_TRACE : INK_ONFILL);
    if (cap > 0) {
        int kx = REP_SLIDE_X + (int)((float)REP_SLIDE_W * ((float)s_buy / (float)cap));
        if (kx > REP_SLIDE_X + REP_SLIDE_W - 8) kx = REP_SLIDE_X + REP_SLIDE_W - 8;
        vg_fill_rect(REP_SLIDE_X + 2, REP_SLIDE_Y + 2,
                     (kx - REP_SLIDE_X > 2 ? kx - REP_SLIDE_X - 2 : 0),
                     REP_SLIDE_H - 4, INK_ONFILL);
        vg_fill_rect(kx, REP_SLIDE_Y + 2, 8, REP_SLIDE_H - 4, INK_MAX);
    }

    snprintf(buf, sizeof(buf), "+%d HULL", s_buy);
    vg_text(REP_SLIDE_X, REP_SLIDE_Y - 24, buf, INK_MAX, 2);

    snprintf(buf, sizeof(buf), "%d CR", s_buy * CREDIT_PER_HULL);
    vg_text(REP_SLIDE_X + REP_SLIDE_W - vg_text_width(buf, 2),
            REP_SLIDE_Y - 24, buf, INK_MAX, 2);

    snprintf(buf, sizeof(buf), "CREDITS %d", vg.credits);
    centred(REP_SLIDE_Y + REP_SLIDE_H + 14, buf, INK_BRIGHT, 3);

    snprintf(buf, sizeof(buf), "%d CR PER POINT", CREDIT_PER_HULL);
    centred(REP_BUY_Y + REP_BUY_H + 12, buf, INK, 2);

    vg_button(REP_BUY_X,  REP_BUY_Y,  REP_BUY_W,  REP_BUY_H,  "BUY",
              s_buy > 0, s_buy > 0);
    vg_button(REP_BACK_X, REP_BACK_Y, REP_BACK_W, REP_BACK_H, "BACK",
              false, true);

    if (cap == 0) {
        const char* why = (vg.health >= vg.health_max) ? "HULL INTACT"
                                                       : "NOT ENOUGH CREDITS";
        centred(REP_SLIDE_Y + REP_SLIDE_H + 48, why, INK_BRIGHT, 2);
    }
}
