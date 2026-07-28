#include "vg_screens.h"
#include "vg_draw.h"
#include "vg_game.h"
#include "vg_tourney.h"
#include "vg_voice.h"
#include <stdio.h>
#include <math.h>

// ===========================================================================
// The tournament map.
//
// Drawn MIRRORED -- eight entrants down the left, eight down the right, the
// final meeting in the middle. A 16-entry bracket in that form is very nearly
// square, which is a suspiciously good fit for a 480x480 panel: the whole shape
// of the tournament is legible at once, and dragging is for reading detail
// rather than for reaching content you cannot otherwise see.
//
// Columns, left to right:  R16  QF  SF  F | CHAMPION | F  SF  QF  R16
// ===========================================================================

#define BRK_CW      86      // column pitch, canvas px
#define BRK_RY      40      // row pitch (eight first-round rows)
#define BOX_W       56
#define BOX_H       22

#define CANVAS_W    (9 * BRK_CW)
#define CANVAS_H    (8 * BRK_RY)

// Viewport: the strip of screen the map is drawn into, below the header and
// above the ready button.
#define VIEW_Y0     84
#define VIEW_H      286

static float s_pan_x = 0.0f;
static float s_pan_y = 0.0f;

static void clamp_pan(void) {
    const float maxx = (float)(CANVAS_W - SCR_W);
    const float maxy = (float)(CANVAS_H - VIEW_H);
    if (s_pan_x < 0.0f) s_pan_x = 0.0f;
    if (s_pan_x > (maxx > 0 ? maxx : 0)) s_pan_x = (maxx > 0 ? maxx : 0);
    if (s_pan_y < 0.0f) s_pan_y = 0.0f;
    if (s_pan_y > (maxy > 0 ? maxy : 0)) s_pan_y = (maxy > 0 ? maxy : 0);
}

void vg_bracket_pan(float dx, float dy) {
    // Drag moves the MAP with the finger, so pushing left reveals what is right.
    s_pan_x -= dx;
    s_pan_y -= dy;
    clamp_pan();
}

bool vg_bracket_ready_at(float x, float y) {
    return vg_in_rect(x, y, BRK_GO_X, BRK_GO_Y, BRK_GO_W, BRK_GO_H);
}

bool vg_bracket_repair_at(float x, float y) {
    return vg_in_rect(x, y, BRK_REP_X, BRK_REP_Y, BRK_REP_W, BRK_REP_H);
}

// Where a slot sits in canvas space. In round r each surviving entrant spans
// 2^r of the eight base rows, so its centre is the middle of that span -- which
// is what makes the tree converge on the final.
static void slot_box(int col, int r, int local, int* bx, int* by) {
    float rowc = (float)(local * (1 << r)) + (float)((1 << r) - 1) * 0.5f;
    *bx = col * BRK_CW + (BRK_CW - BOX_W) / 2;
    *by = (int)(rowc * BRK_RY + BRK_RY * 0.5f) - BOX_H / 2;
}

// Round r holds 16>>r entrants; the first half hang off the left spine and the
// second half off the right, mirrored.
static void slot_place(int r, int idx, int* col, int* local) {
    const int n    = TOURNEY_ENTRANTS >> r;
    const int half = n / 2;
    if (idx < half) { *col = r;     *local = idx; }
    else            { *col = 8 - r; *local = idx - half; }
}

static void canvas_to_screen(int cx, int cy, int* sx, int* sy) {
    *sx = cx - (int)s_pan_x;
    *sy = VIEW_Y0 + cy - (int)s_pan_y;
}

void vg_bracket_focus_player(void) {
    int col, local, bx, by;
    slot_place(vt.round, vt.player_pos, &col, &local);
    slot_box(col, vt.round, local, &bx, &by);
    s_pan_x = (float)(bx + BOX_W / 2 - SCR_W / 2);
    s_pan_y = (float)(by + BOX_H / 2) - (float)VIEW_H * 0.5f;
    clamp_pan();
}

// A horizontal or vertical rule, clipped to the viewport so connectors cannot
// bleed into the header or over the ready button.
static void rule(int x, int y, int w, int h, uint16_t col) {
    if (y < VIEW_Y0) { h -= (VIEW_Y0 - y); y = VIEW_Y0; }
    if (y + h > VIEW_Y0 + VIEW_H) h = VIEW_Y0 + VIEW_H - y;
    if (h <= 0 || w <= 0) return;
    if (x + w < 0 || x > SCR_W) return;
    vg_fill_rect(x, y, w, h, col);
}

static void draw_entrant(int idx, int bx, int by, bool alive, bool is_next_opp) {
    int sx, sy;
    canvas_to_screen(bx, by, &sx, &sy);
    if (sx + BOX_W < 0 || sx > SCR_W) return;
    if (sy + BOX_H < VIEW_Y0 || sy > VIEW_Y0 + VIEW_H) return;

    if (idx < 0) {
        // Not decided yet. An empty frame still shows the shape of the draw,
        // which is half the reason to look at the sheet before a match.
        vg_rect(sx, sy, BOX_W, BOX_H, INK_FAINT);
        return;
    }

    const Entrant* e = &vt.entrant[idx];

    // Trail colour down the left edge. This is the one place the interface
    // carries hue, and it earns it: colour here means WHO, exactly as it will
    // when the same colour is streaming off their ship.
    const uint16_t hue = alive ? vg_hue_col(e->hue) : INK_TRACE;

    if (e->is_player) {
        // Inverse video for you -- hierarchy is still brightness and inversion.
        vg_fill_rect(sx, sy, BOX_W, BOX_H, INK_BRIGHT);
        vg_fill_rect(sx, sy, 4, BOX_H, hue);
        // INK_ONFILL, not COL_BLACK: vg_text treats colour 0 as invisible, so a
        // black label renders as nothing and the slot reads as a solid block.
        vg_text(sx + 7, sy + 4, e->tag, INK_ONFILL, 2);
        char pc[2] = { vg_spec(e->cls)->name[0], 0 };
        vg_text(sx + 45, sy + 7, pc, INK_ONFILL, 1);
        return;
    }

    uint16_t frame = alive ? (is_next_opp ? INK_MAX : INK_TRACE) : INK_FAINT;
    uint16_t ink   = alive ? (is_next_opp ? INK_MAX : INK_BRIGHT) : INK_FAINT;

    vg_rect(sx, sy, BOX_W, BOX_H, frame);
    vg_fill_rect(sx + 1, sy + 1, 3, BOX_H - 2, hue);
    vg_text(sx + 7, sy + 4, e->tag, ink, 2);
    // One character of ship class: A / L / C / B. That is what makes the sheet
    // tactical -- seeing a BALLISTA two rounds out is something you can plan for.
    char cls[2] = { vg_spec(e->cls)->name[0], 0 };
    vg_text(sx + 45, sy + 7, cls, ink, 1);

    if (!alive) {
        // Struck through and dropped to the dim ink level: out, but still part
        // of the record of how the bracket got here.
        vg_fill_rect(sx + 2, sy + BOX_H / 2, BOX_W - 4, 1, INK_FAINT);
    }
}

void vg_draw_bracket(void) {
    char buf[48];

    // Which entrants are still standing: exactly those in the current round's
    // field. Anyone in an earlier round but not this one went out.
    bool alive[TOURNEY_ENTRANTS] = { false };
    const int live_round = vt.complete ? TOURNEY_ROUNDS : vt.round;
    const int live_n     = TOURNEY_ENTRANTS >> live_round;
    for (int i = 0; i < live_n; i++) {
        int idx = vt.slot[live_round][i];
        if (idx >= 0) alive[idx] = true;
    }

    const Entrant* opp = vg_tourney_opponent();

    // --- connectors, under the boxes ---
    for (int r = 0; r < TOURNEY_ROUNDS; r++) {
        const int n = TOURNEY_ENTRANTS >> r;
        for (int m = 0; m < n / 2; m++) {
            int ca, la, cb, lb;
            slot_place(r, m * 2,     &ca, &la);
            slot_place(r, m * 2 + 1, &cb, &lb);
            if (ca != cb) continue;          // the pair straddling the middle

            int ax, ay, bx2, by2;
            slot_box(ca, r, la, &ax, &ay);
            slot_box(cb, r, lb, &bx2, &by2);

            const bool left = (ca <= 4);
            // Spine sits in the gutter between this column and the next.
            int spine = left ? (ax + BOX_W + 12) : (ax - 12);
            int sx, sy_a, sy_b;
            canvas_to_screen(spine, ay + BOX_H / 2, &sx, &sy_a);
            canvas_to_screen(spine, by2 + BOX_H / 2, &sx, &sy_b);

            rule(sx, sy_a, 1, sy_b - sy_a, INK_FAINT);

            int hx = left ? (ax + BOX_W) : (ax - 12);
            int hsx, hsy;
            canvas_to_screen(hx, ay + BOX_H / 2, &hsx, &hsy);
            rule(hsx, hsy, 12, 1, INK_FAINT);
            canvas_to_screen(hx, by2 + BOX_H / 2, &hsx, &hsy);
            rule(hsx, hsy, 12, 1, INK_FAINT);
        }
    }

    // --- entrants ---
    for (int r = 0; r <= TOURNEY_ROUNDS; r++) {
        if (r == TOURNEY_ROUNDS) {
            int bx, by;
            slot_box(4, 3, 0, &bx, &by);      // champion box, dead centre
            int idx = vt.slot[TOURNEY_ROUNDS][0];
            draw_entrant(idx, bx, by, true, false);
            continue;
        }
        const int n = TOURNEY_ENTRANTS >> r;
        for (int i = 0; i < n; i++) {
            int col, local, bx, by;
            slot_place(r, i, &col, &local);
            slot_box(col, r, local, &bx, &by);
            int idx = vt.slot[r][i];
            bool is_opp = (opp && idx >= 0 && &vt.entrant[idx] == opp &&
                           r == (int)vt.round);
            draw_entrant(idx, bx, by, idx >= 0 && alive[idx], is_opp);
        }
    }

    // --- header ---
    vg_fill_rect(0, 0, SCR_W, VIEW_Y0 - 2, COL_BLACK);

    // Round on the left, bank on the right, the match you are about to fly
    // across the middle at full size. Credits are a number the player is
    // constantly deciding against, so they get a fixed corner and stay there.
    //
    // Both are held SCR_SAFE clear of the border: the panel is a rounded square
    // and anything pinned to an edge loses its leading characters to the corner.
    vg_text(SCR_SAFE, 12, vg_tourney_round_name(vt.round), INK_BRIGHT, 2);

    snprintf(buf, sizeof(buf), "%d CR", vg.credits);
    vg_text(SCR_W - SCR_SAFE - vg_text_width(buf, 3), 8, buf, INK_MAX, 3);

    if (opp) {
        // Colour column butted against the callsign rather than parked at the
        // screen edge: the point of the swatch is to bind a hue to a NAME, and
        // it only does that if the two are touching.
        snprintf(buf, sizeof(buf), "VS %s  %s", opp->tag, vg_spec(opp->cls)->name);
        const int tw = vg_text_width(buf, 3);
        const int tx = (SCR_W - tw) / 2 + 6;
        vg_fill_rect(tx - 14, 40, 6, 24, vg_hue_col(opp->hue));
        vg_text(tx, 42, buf, INK_MAX, 3);

        // Who they are, under what they fly. The ship tells you how the fight
        // will go; this tells you what you are going to have to listen to.
        const char* a = vg_voice_archetype(opp->voice);
        vg_text((SCR_W - vg_text_width(a, 1)) / 2, 68, a, INK_FAINT, 1);
    }

    // --- footer ---
    vg_fill_rect(0, VIEW_Y0 + VIEW_H, SCR_W, SCR_H - VIEW_Y0 - VIEW_H, COL_BLACK);

    const int hurt = (int)(vg.health_max - vg.health + 0.5f);
    const int hp   = (int)(vg.health + 0.5f);
    const int hmax = (int)(vg.health_max + 0.5f);

    // Your ship and what is left of it, centred and at full size. This is the
    // number every decision on this screen is really about, and it was
    // previously set faint at the smallest scale, where it was unreadable.
    snprintf(buf, sizeof(buf), "%s  %d/%d", vg.spec->name, hp, hmax);
    const int fy = VIEW_Y0 + VIEW_H + 6;
    const int fw = vg_text_width(buf, 3);
    const int fx = (SCR_W - fw) / 2 + 6;
    // Your own colour, butted against your ship the same way.
    vg_fill_rect(fx - 14, fy - 2, 6, 24, vg_hue_col(vg.trail_hue));
    vg_text(fx, fy, buf, hurt > 0 ? INK_MAX : INK_BRIGHT, 3);

    // REPAIR only lights when there is both damage to undo and money to do it
    // with, so the button itself answers "can I afford this".
    const bool can_repair = hurt > 0 && vg.credits >= CREDIT_PER_HULL;
    vg_button(BRK_REP_X, BRK_REP_Y, BRK_REP_W, BRK_REP_H, "REPAIR",
              false, can_repair);
    vg_button(BRK_GO_X, BRK_GO_Y, BRK_GO_W, BRK_GO_H, "READY", true, true);
}
