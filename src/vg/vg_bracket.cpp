#include "vg_bracket.h"
#include "vg_ui.h"
#include "vg_shipview.h"
#include "vg_console.h"
#include "vg_raster.h"
#include "vg_draw.h"
#include "vg_game.h"
#include "vg_tourney.h"
#include <stdio.h>

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

// A THIRD BIGGER THAN IT WAS, and the reason is the device rather than the
// drawing. The sheet was laid out so that one whole side of it was on screen at
// once, which is more than a glance can use and smaller than a 314 ppi panel can
// carry: the callsign was scale 2 in a 56px box and the ship class was a single
// letter at scale 1, which is 0.57mm and is not small type, it is type nobody can
// read. It is the same fault the ship-select pass fixed once already.
//
// At this pitch six boxes and about three and a half columns fit the window. That
// reverses the rule in design/notes/design.md that the whole sheet is always
// visible and panning is only for detail -- the author's call, made after a UX
// pass on the device and recorded there.
#define BRK_CW      129     // column pitch, canvas px
#define BRK_RY      48      // row pitch (eight first-round rows)
#define BOX_W       76
#define BOX_H       30

// Inside a box: the trail stripe down the left edge, the callsign clear of it,
// and the class mark's cell at the right. Named because the draw and the eye have
// to agree about where a box's furniture sits, and three hand-picked offsets in
// the middle of a draw function is how that stops being true.
#define BOX_HUE_W   4
#define BOX_TAG_X   9
#define BOX_PAD     5
// The class mark's half extents. A shade wider than tall, because three of the
// four marks point forward and want the room in that direction.
#define BOX_GLYPH_HW 8
#define BOX_GLYPH_HH 6

#define CANVAS_W    (9 * BRK_CW)
#define CANVAS_H    (8 * BRK_RY)

// A CAPTION STRIP ACROSS THE TOP OF THE WINDOW, and the sheet has what is left.
//
// The round and the bank used to sit on a black band above the map, on plating
// the art now owns -- and the art has no hole for them, because they are not a
// banner and not a key. They belong to the picture, so they go inside the window,
// pinned while the sheet moves under them.
//
// 22px is a scale 2 line with four either side. It costs the sheet half a row and
// buys a number the player is constantly deciding against a fixed place to be.
#define CAP_H       22

// The window, from the art, less the caption. VIEW_* is the SHEET's strip, which
// is what every reader of it downstream means by it.
#define VIEW_X0     BRK_VIEW_X0
#define VIEW_W      BRK_VIEW_W
#define VIEW_Y0     (BRK_VIEW_Y0 + CAP_H)
#define VIEW_H      (BRK_VIEW_H - CAP_H)

static float s_pan_x = 0.0f;
static float s_pan_y = 0.0f;

// ===========================================================================
// THE CHYRON
//
// A broadcast runs its results across the bottom of the picture and this page IS
// the broadcast -- the console is behind us by the time the draw is made. So the
// tournament reads itself out: every match already flown, newest round first.
//
// IT IS NOT AN IFT LINE, and that is not a style choice. Broadcast::ift_fired is
// a uint8_t with one bit per slot and there are already nine of them, so a tenth
// would silently do nothing -- vg_ift.h says so in as many words. This composes
// its own text and borrows only the voice: white, bare uppercase, no punctuation,
// and the same triple space the IFT separates fields with.
//
// THE RESULT OF EVERY MATCH IS ALREADY IN THE TABLE and no field had to be added
// for this. vt.slot[r] is the field at the START of round r, so match m is
// slot[r][2m] against slot[r][2m+1] and the winner is slot[r+1][m] -- which makes
// the loser the other one. Fifteen matches, reconstructable at any point.
//
// A VERB, AND IT IS IN THE PRESENT TENSE. The first pass ran winner and loser
// with three spaces between them and no verb, on the grounds that IFT_MATCH_END
// already does that and the reading would be learned once. It is not learned
// once: a crawl of bare pairs reads as a list of names, and the one thing a
// result has to say is which of the two is still flying.
//
// ELIMINATES rather than ELIMINATED because a headline does not use the past --
// "UNITED WIN LEAGUE", never WON -- and a crawl in the past tense reads as an
// archive rather than as a feed. The word is doing a second job as well. The
// tournament is lethal and the broadcast runs it for viewing figures and tax
// revenue; a network that says ELIMINATES when it means killed is exactly the
// voice design.md describes.
//
// With the verb binding the two names, one width of gap is enough.
// ===========================================================================

#define CHY_VERB  " ELIMINATES "
#define CHY_OUT   "      "      // one match to the next

// A FRESH SHEET HAS NOTHING TO REPORT, and it does not pretend otherwise. The
// author's line. Administrative, incurious, and not on anybody's side -- and it
// says the ticker has a job it cannot do yet rather than filling the space.
#define CHY_COLD  "AWAITING MATCH RESULTS"

// Fifteen matches at about twenty-four characters each, plus four round names.
// Roughly 420 in the worst case, which is a whole tournament read out.
static char s_chyron[640];

void vg_bracket_chyron(void) {
    s_chyron[0] = 0;
    int at = 0;

    // Resolved rounds only. vt.round is the one being flown, so everything below
    // it is history; a finished tournament has all four.
    const int done = vt.complete ? TOURNEY_ROUNDS : (int)vt.round;

    for (int r = done - 1; r >= 0; r--) {
        const int n = TOURNEY_ENTRANTS >> r;

        at += snprintf(s_chyron + at, sizeof(s_chyron) - at, "%s%s",
                       at ? CHY_OUT : "", vg_tourney_round_name((uint8_t)r));
        if (at >= (int)sizeof(s_chyron) - 1) break;

        for (int m = 0; m < n / 2; m++) {
            const int a = vt.slot[r][m * 2];
            const int b = vt.slot[r][m * 2 + 1];
            const int w = vt.slot[r + 1][m];
            if (a < 0 || b < 0 || w < 0) continue;
            const int l = (w == a) ? b : a;

            at += snprintf(s_chyron + at, sizeof(s_chyron) - at,
                           CHY_OUT "%s" CHY_VERB "%s",
                           vt.entrant[w].tag, vt.entrant[l].tag);
            if (at >= (int)sizeof(s_chyron) - 1) break;
        }
    }

    if (!s_chyron[0])
        snprintf(s_chyron, sizeof(s_chyron), "%s", CHY_COLD);
}

static void clamp_pan(void) {
    // AGAINST THE WINDOW, NOT THE SCREEN. The sheet used to own the full width of
    // the panel; it owns the aperture now, which is 122px narrower, and a clamp
    // that still said SCR_W would stop panning with the last column under the
    // steel.
    const float maxx = (float)(CANVAS_W - VIEW_W);
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

// THE KEYS ARE HOLES IN THE ART and their rectangles come out of it, so a hit
// test is which hole was hit. Three macros and three rectangles went with them.
bool vg_bracket_course_at(float x, float y) {
    return vg_console_key_at(BRK_KEY_COURSE, x, y);
}

bool vg_bracket_ready_at(float x, float y) {
    return vg_console_key_at(BRK_KEY_READY, x, y);
}

bool vg_bracket_repair_at(float x, float y) {
    return vg_console_key_at(BRK_KEY_REPAIR, x, y);
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
    *sx = VIEW_X0 + cx - (int)s_pan_x;
    *sy = VIEW_Y0 + cy - (int)s_pan_y;
}

void vg_bracket_focus_player(void) {
    int col, local, bx, by;
    slot_place(vt.round, vt.player_pos, &col, &local);
    slot_box(col, vt.round, local, &bx, &by);
    s_pan_x = (float)(bx + BOX_W / 2) - (float)VIEW_W * 0.5f;
    s_pan_y = (float)(by + BOX_H / 2) - (float)VIEW_H * 0.5f;
    clamp_pan();
}

// A horizontal or vertical rule, clipped to the viewport so connectors cannot
// bleed into the header or over the ready button.
static void rule(int x, int y, int w, int h, uint16_t col) {
    if (y < VIEW_Y0) { h -= (VIEW_Y0 - y); y = VIEW_Y0; }
    if (y + h > VIEW_Y0 + VIEW_H) h = VIEW_Y0 + VIEW_H - y;
    if (h <= 0 || w <= 0) return;
    if (x + w < VIEW_X0 || x > VIEW_X0 + VIEW_W) return;
    vg_fill_rect(x, y, w, h, col);
}

// THE CALLSIGN AND WHAT THEY FLY, laid out once for both kinds of box.
//
// The class was one character at SCALE 1 and could not be read on the device at
// all, which made it decoration rather than the tactical fact it is meant to be --
// seeing a BALLISTA two rounds out is something you can plan for, and only if you
// can see it. At scale 2 the four initials are 12px and unmistakably distinct.
//
// Both lines are centred vertically in the box rather than offset from its top, so
// the box can change height without either of them being re-picked.
static void box_label(int sx, int sy, const char* tag, ShipClass cls,
                      uint16_t ink) {
    const int ty = sy + (BOX_H - 14) / 2;      // a scale 2 glyph is 14 tall
    vg_text(sx + BOX_TAG_X, ty, tag, ink, 2);

    // The class as a MARK rather than as a letter. See vg_ship_glyph: a letter at
    // this size is something you read one box at a time, and the sheet is for
    // taking in a column of opponents at once.
    vg_ship_glyph(cls, sx + BOX_W - BOX_PAD - BOX_GLYPH_HW,
                  sy + BOX_H / 2, BOX_GLYPH_HW, BOX_GLYPH_HH, ink);
}

static void draw_entrant(int idx, int bx, int by, bool alive, bool is_next_opp) {
    int sx, sy;
    canvas_to_screen(bx, by, &sx, &sy);
    if (sx + BOX_W < VIEW_X0 || sx > VIEW_X0 + VIEW_W) return;
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
        vg_fill_rect(sx, sy, BOX_HUE_W, BOX_H, hue);
        // INK_ONFILL, not COL_BLACK: vg_text treats colour 0 as invisible, so a
        // black label renders as nothing and the slot reads as a solid block.
        box_label(sx, sy, e->tag, e->cls, INK_ONFILL);
        return;
    }

    uint16_t frame = alive ? (is_next_opp ? INK_MAX : INK_TRACE) : INK_FAINT;
    uint16_t ink   = alive ? (is_next_opp ? INK_MAX : INK_BRIGHT) : INK_FAINT;

    vg_rect(sx, sy, BOX_W, BOX_H, frame);
    vg_fill_rect(sx + 1, sy + 1, BOX_HUE_W - 1, BOX_H - 2, hue);
    box_label(sx, sy, e->tag, e->cls, ink);

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
    (void)opp;

    // THE RIG. Broadcast rather than terminal: no glass, because this page is the
    // picture and not a machine showing you one -- and because the warp would cut
    // every rule on the sheet into chords and overflow the slice. See
    // VgConsoleForm. The banner is the chyron, and the chassis is painted by
    // close(), last, over anything that ran past a hole.
    vg_console_open(&BEZEL_TOURNEY, VG_CON_BROADCAST, s_chyron, nullptr);

    // CLIPPED TO THE WINDOW, and this is not belt and braces. The steel masks
    // whatever the sheet spills onto plating, but the two key wells and the
    // chyron are HOLES: a box drawn 30px below the aperture lands in a key, and
    // the chassis has no pixels there to cover it with.
    vg_rast_viewport(VIEW_X0, VIEW_Y0, VIEW_W, VIEW_H);

    // --- connectors, under the boxes ---
    for (int r = 0; r < TOURNEY_ROUNDS; r++) {
        const int n = TOURNEY_ENTRANTS >> r;
        for (int m = 0; m < n / 2; m++) {
            int ca, la, cb, lb;
            slot_place(r, m * 2,     &ca, &la);
            slot_place(r, m * 2 + 1, &cb, &lb);

            int ax, ay, bx2, by2;
            slot_box(ca, r, la, &ax, &ay);
            slot_box(cb, r, lb, &bx2, &by2);

            if (ca != cb) {
                // THE FINAL, and it is the only pair that straddles the middle.
                // Its two boxes sit either SIDE of the champion's rather than
                // above and below each other, so there is no spine to drop
                // between them and the general case cannot draw it.
                //
                // It used to be skipped outright, which left the champion box
                // floating with nothing joining it to the two fighters who get
                // there -- the one box on the sheet the whole tree points at.
                // One rule straight through, and the champion is drawn over the
                // middle of it afterwards.
                const int x0 = ((ax < bx2) ? ax : bx2) + BOX_W;
                const int x1 = (ax < bx2) ? bx2 : ax;
                int hsx, hsy;
                canvas_to_screen(x0, ay + BOX_H / 2, &hsx, &hsy);
                rule(hsx, hsy, x1 - x0, 1, INK_FAINT);
                continue;
            }

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

    vg_rast_viewport_full();

    // --- the caption -------------------------------------------------------
    //
    // Round on the left, bank on the right, inside the window and pinned while
    // the sheet moves under them. INK_WELL and not COL_BLACK: a fill whose colour
    // is zero is DROPPED, which is how the band this replaces went four months
    // without ever painting.
    vg_fill_rect(BRK_VIEW_X0, BRK_VIEW_Y0, BRK_VIEW_W, CAP_H, INK_WELL);
    vg_text(BRK_VIEW_X0 + 4, BRK_VIEW_Y0 + 4,
            vg_tourney_round_name(vt.round), INK_BRIGHT, 2);

    snprintf(buf, sizeof(buf), "%d CR", vg.credits);
    vg_text(BRK_VIEW_X0 + BRK_VIEW_W - 4 - vg_text_width(buf, 2),
            BRK_VIEW_Y0 + 4, buf, INK_MAX, 2);

    // THE VS LINE AND THE ARCHETYPE ARE GONE.
    //
    // The archetype was drawn at scale 1, which on this panel is 0.57mm, and it
    // was answering a question nobody asked at the sheet: who you are about to
    // fly against is the sheet's whole subject, and what they will SAY while
    // doing it is not something you can act on. design.md makes the stronger
    // argument -- personality is rolled independently of ship, seeding and hue
    // precisely so the bracket is not readable at a glance, and printing it here
    // undoes that on purpose.
    //
    // The VS line went with it because it was saying twice what the sheet says
    // once: the next opponent's box is promoted to INK_MAX and the page opens
    // centred on it. If that turns out to be too quiet in the hand, this is four
    // lines to put back.

    // --- the keys, which are holes in the plating --------------------------
    //
    // No footer band and no rectangles. The chassis drew the wells; a key is its
    // label and what it does under a thumb.
    //
    // THE HULL READOUT WENT TO THE REPAIR PAGE, where the decision it informs is
    // actually taken. It was the biggest thing on the bottom of this screen and
    // all it could tell you was whether to press a key that leads somewhere that
    // says it again.
    const int hurt = (int)(vg.health_max - vg.health + 0.5f);

    // REPAIR is offered whether or not it can be taken, and goes dim when it
    // cannot, so the key itself answers "can I afford this". The hit test does
    // not change: a key that stops responding reads as broken.
    const bool can_repair = hurt > 0 && vg.credits >= CREDIT_PER_HULL;
    vg_console_key(BRK_KEY_REPAIR, "REPAIR", can_repair);

    // Always offered, never required. A player can spend a whole tournament
    // without touching it, and one who wants to practise can come back between
    // any two rounds.
    vg_console_key(BRK_KEY_COURSE, "COURSE", true);
    vg_console_key(BRK_KEY_READY,  "READY",  true);

    vg_console_close();
}
