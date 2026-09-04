#include "vg_screens.h"
#include "vg_bezel.h"
#include "vg_console.h"
#include "generated/bezel_console.h"
#include "vg_sfx.h"
#include "vg_course.h"
#include "vg_draw.h"
#include "vg_glitch.h"
#include "vg_game.h"
#include <stdio.h>
#include <math.h>

// Ship select and pause. The tournament map is big enough to want its own file.

static void centred(int y, const char* s, uint16_t col, int scale) {
    vg_text((SCR_W - vg_text_width(s, scale)) / 2, y, s, col, scale);
}

// The live contact, set once a frame by vg_state_update. See vg_draw.h.
static bool  s_press_held = false;
static float s_press_x = 0.0f, s_press_y = 0.0f;

void vg_press_set(bool held, float x, float y) {
    s_press_held = held;
    s_press_x = x;
    s_press_y = y;
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
// Ship select
// ---------------------------------------------------------------------------

// WHICH WHEEL ROW a contact landed on, as an offset from the detent: -1 is the
// row above the selection, +1 the row below, 0 the selection itself. Outside the
// wheel entirely it returns SEL_ROW_NONE.
//
// The wheel is what a tap tests against now, not four cards. Tapping a neighbour
// nudges by one, exactly as the letter wheels do -- the screen has to stay usable
// without a drag, and the board has a hardware button too.
int vg_select_row_at(float x, float y) {
    if (!vg_in_rect(x, y, SEL_WHEEL_X, SEL_WHEEL_Y, SEL_WHEEL_W, SEL_WHEEL_H))
        return SEL_ROW_NONE;
    const float dy = y - (float)SEL_WHEEL_DETENT;
    int k = (int)lroundf(dy / (float)SEL_WHEEL_PITCH);
    // Clamped to the rows that are actually DRAWN, so a tap in the margin below
    // the last name nudges by one rather than by however far the finger was out.
    if (k < SEL_WHEEL_LO)                     k = SEL_WHEEL_LO;
    if (k > SEL_WHEEL_LO + SEL_WHEEL_N - 1)   k = SEL_WHEEL_LO + SEL_WHEEL_N - 1;
    return k;
}

bool vg_select_confirm_at(float x, float y) {
    return vg_console_key_at(VG_CON_KEY, x, y);
}

// THE FIVE AXES, AS A SHAPE.
//
// Absolute numbers are meaningless before you have flown any of them; what a
// player needs here is the shape of a class against the others, which is what a
// radar chart is for and what four independent bars never were.
//
// Drawn the way the HUD radar is drawn -- see draw_radar. The trig is baked once
// because that instrument already learned what per-frame cosf costs; the
// graticule is a line loop at fixed fractions; and the plotted polygon is an
// OUTLINE, because this renderer has no polygon fill and a solid slab would be
// against the house style regardless.
static float s_ax_cs[5][2];
static bool  s_ax_ready = false;

static void chart_tables(void) {
    if (s_ax_ready) return;
    // Clockwise from the top, so SPEED sits at twelve and the order round the
    // dial is the order vg_ship_axes writes them in.
    for (int i = 0; i < 5; i++) {
        const float a = -1.57079633f + (float)i * (6.28318531f / 5.0f);
        s_ax_cs[i][0] = cosf(a);
        s_ax_cs[i][1] = sinf(a);
    }
    s_ax_ready = true;
}

static void chart_pt(int i, float t, float* px, float* py) {
    *px = (float)SEL_CHART_CX + s_ax_cs[i][0] * (float)SEL_CHART_R * t;
    *py = (float)SEL_CHART_CY + s_ax_cs[i][1] * (float)SEL_CHART_R * t;
}

static void chart_ring(float t, uint16_t col, int w) {
    float px, py, nx, ny;
    chart_pt(4, t, &px, &py);
    for (int i = 0; i < 5; i++) {
        chart_pt(i, t, &nx, &ny);
        vg_line_w(px, py, nx, ny, col, w);
        px = nx; py = ny;
    }
}

// WHAT THE SYSTEM DOES, as a specification rather than a pitch.
//
// It read "YOU FLY EVERY ROUND ALL THE WAY IN", which is how you would sell the
// class to somebody. The shape is [guidance type]. [what makes it different] --
// the words a manual would use, not the words a recruiter would.
//
// A LABELLED FIELD: an INVERSE-VIDEO tab, then the value beside it, WRAPPED.
//
// The label is knocked out of a filled block rather than drawn in dim ink,
// because a caption and its contents are different KINDS of thing and the panel
// should say so without being read. Inversion is how the rest of the interface
// already says "this is chrome" -- the HUD panel tabs, the bracket's marker for
// you, the missile rack. One vocabulary.
//
// INK_ONFILL, never COL_BLACK: vg_text treats colour 0 as invisible, so black
// text on a fill is no text at all.
//
// THE CAPTION STAYS SMALL AND THE VALUE GROWS. A label is chrome -- you learn
// what MSL means once and never read it again -- while the value beside it has
// to be legible on a 314 ppi panel, where scale 1 is a point and a half tall.
// Keeping the tab at scale 1 also buys the value five characters on its first
// line, which is the difference between two lines and three.
//
// AND IT WRAPS, with a hanging indent: the first line starts after the tab and
// the rest run the full width of the column. That is what lets the screen stay
// divided. A 243px column is twenty characters at scale 2 and the descriptions
// are twenty-five to thirty-one, so the only way to read them at that size
// without taking the wheel's strip is to give them a second line.
//
// Breaks on spaces, never mid-word. Returns the bottom of what it drew, so the
// caller can stack the next row under a block whose height it does not know.
static int field_wrap(int x, int y, int w, const char* label,
                      const char* value, uint16_t vcol) {
    const int tw  = vg_text_width(label, 1);
    const int ind = tw + 4 + 9;                 // tab, its padding, then the gap

    vg_fill_rect(x - 2, y + 2, tw + 4, 9, INK);
    vg_text(x, y + 3, label, INK_ONFILL, 1);

    int at = 0, line = 0;
    while (value[at]) {
        const int left = (line == 0) ? (w - ind) : w;
        const int cols = left / 12;             // 6px a glyph at scale 2
        if (cols < 1) break;

        int take = 0, brk = 0;
        while (value[at + take] && take < cols) {
            if (value[at + take] == ' ') brk = take;
            take++;
        }
        // Only break early if there is more to come: the last line keeps its tail.
        if (value[at + take] && brk > 0) take = brk;

        char buf[48];
        int n = 0;
        for (int i = 0; i < take && n < (int)sizeof(buf) - 1; i++) buf[n++] = value[at + i];
        buf[n] = 0;

        vg_text((line == 0) ? x + ind : x, y + line * 18, buf, vcol, 2);

        at += take;
        while (value[at] == ' ') at++;
        line++;
        if (line >= 3) break;                   // a description that long is a bug
    }
    return y + line * 18;
}

static void draw_chart(const float ax[5]) {
    chart_tables();

    // Graduations first, hairline, so they read as scale and not as structure.
    chart_ring(0.33f, INK_TRACE, 1);
    chart_ring(0.66f, INK_TRACE, 1);
    for (int i = 0; i < 5; i++) {
        float ex, ey;
        chart_pt(i, 1.0f, &ex, &ey);
        vg_line((float)SEL_CHART_CX, (float)SEL_CHART_CY, ex, ey, INK_TRACE);
    }
    // ...then the rim at 2px, which is the instrument border. The same weighting
    // rule the radar uses: outer edge structural, inner rings graduations.
    chart_ring(1.0f, INK_FAINT, 2);

    // The class itself, over the top and at the top of the ramp.
    {
        float px, py, nx, ny;
        chart_pt(4, ax[4], &px, &py);
        for (int i = 0; i < 5; i++) {
            chart_pt(i, ax[i], &nx, &ny);
            vg_line_w(px, py, nx, ny, INK_MAX, 2);
            px = nx; py = ny;
        }
    }

    // Labels outside their own vertex, nudged so the text clears the rim instead
    // of straddling it: pushed fully left of a left-hand vertex, fully right of a
    // right-hand one, and centred on the two that sit near the vertical.
    for (int i = 0; i < 5; i++) {
        const char* nm = vg_ship_axis_name(i);
        const float ox = s_ax_cs[i][0], oy = s_ax_cs[i][1];
        float lx = (float)SEL_CHART_CX + ox * (float)(SEL_CHART_R + SEL_CHART_LABEL);
        float ly = (float)SEL_CHART_CY + oy * (float)(SEL_CHART_R + SEL_CHART_LABEL);
        const float w = (float)vg_text_width(nm, 1);
        lx -= w * ((ox < -0.3f) ? 1.0f : ((ox > 0.3f) ? 0.0f : 0.5f));
        ly -= (oy < -0.3f) ? 7.0f : ((oy > 0.3f) ? 0.0f : 3.5f);
        vg_text((int)lx, (int)ly, nm, INK, 1);
    }
}

// THE CLASS, FROM ABOVE, laid on its side.
//
// A top-down silhouette is how a pilot actually tells one airframe from another,
// and it is the one view that survives being small: no projection, no culling, no
// depth. It replaced a turning 3-D model, which was the wrong instrument twice
// over -- it spent most of its time edge-on, and it was the SAME shape for all
// four classes, so it said "a ship" where this says which one.
//
// Nose to the RIGHT, because the slot is wide and short and a plan view is long
// and narrow. Scaled UNIFORMLY off the widest span in the roster, so a class that
// is genuinely narrow reads as narrow instead of being stretched to fill the box.
static void draw_plan_view(const ShipSpec* sp, int cls) {
    const VgShipPlan& pl = vg_ship_plan[(cls < SHIP_CLASSES) ? cls : 0];
    if (!pl.pts || pl.n < 2) return;
    (void)sp;

    // One scale for every class, from the roster's widest half-span. Fitting each
    // ship to the slot individually would make them all the same size, which is
    // the one thing the silhouette must not do.
    // BALLISTA's reverse wing, and it is the widest thing in the roster now. This
    // is the scale EVERY class is drawn against, so it has to track whichever hull
    // is broadest or that hull walks out of the slot.
    const float MAX_HALF_SPAN = 0.58f;
    const float sy = ((float)SEL_MODEL_H * 0.46f) / MAX_HALF_SPAN;
    const float sx = sy;                            // uniform: no stretching
    const float cx = (float)(SEL_PANEL_X + SEL_PANEL_W / 2);
    const float cy = (float)(SEL_MODEL_Y + SEL_MODEL_H / 2);

    // Both sides at once, walking the half-outline and mirroring as we go, so the
    // two strokes cannot drift apart.
    float px = cx + pl.pts[0] * sx, py = cy - pl.pts[1] * sy;
    float qx = px,                  qy = cy + pl.pts[1] * sy;
    for (int i = 1; i < pl.n; i++) {
        const float nx = cx + pl.pts[i * 2] * sx;
        const float ny = cy - pl.pts[i * 2 + 1] * sy;
        const float my = cy + pl.pts[i * 2 + 1] * sy;
        vg_line_w(px, py, nx, ny, INK_BRIGHT, 2);
        vg_line_w(qx, qy, nx, my, INK_BRIGHT, 2);
        px = nx; py = ny;
        qx = nx; qy = my;
    }
    // CLOSE BOTH ENDS, wherever the outline stops at a real width rather than at a
    // point. Every hull is blunt at the back, and LANCE's beak is clipped square at
    // the front -- without this the two mirrored halves simply stop and the ship
    // reads as an open bracket.
    for (int e = 0; e < 2; e++) {
        const int   k  = e ? (pl.n - 1) : 0;
        const float ex = cx + pl.pts[k * 2] * sx;
        const float ey =      pl.pts[k * 2 + 1] * sy;
        if (ey > 0.5f) vg_line_w(ex, cy - ey, ex, cy + ey, INK_BRIGHT, 2);
    }

    // NO SPINE, AND NO FUSELAGE EDGE PAIR. Both were long straight lines running
    // most of the length, and at this size they read as ruled lines drawn over the
    // ship rather than as part of it -- and worse, they ran straight through the
    // missile bays, which are the one piece of detail that says what a class DOES.
    // The closed tail already ties the halves together.

    // THE BAYS, SOLID, and they are the one piece of detail carrying a NUMBER --
    // count them and you know the magazine. Drawn before the lines so the canopy
    // and the rest sit over the top.
    for (int i = 0; i < pl.nb; i++) {
        const float bx0 = pl.bay[i * 4],     by0 = pl.bay[i * 4 + 1];
        const float bx1 = pl.bay[i * 4 + 2], by1 = pl.bay[i * 4 + 3];
        const int   xa  = (int)(cx + ((bx0 < bx1) ? bx0 : bx1) * sx);
        const int   w   = (int)(((bx0 < bx1) ? (bx1 - bx0) : (bx0 - bx1)) * sx);
        const float lo  = ((by0 < by1) ? by0 : by1) * sy;
        const float hi  = ((by0 < by1) ? by1 : by0) * sy;
        const int   h   = (int)(hi - lo);
        if (w < 1 || h < 1) continue;
        vg_fill_rect(xa, (int)(cy - hi), w, h, INK_BRIGHT);
        // A bay authored from the centreline mirrors onto itself; drawing it twice
        // would be harmless but pointless, and the test is the same one the
        // detail lines use.
        if (lo > 0.5f) vg_fill_rect(xa, (int)(cy + lo), w, h, INK_BRIGHT);
    }

    // ...and what is knocked back out of them.
    for (int i = 0; i < pl.nc; i++) {
        const float ax = cx + pl.cut[i * 4]     * sx;
        const float ay =      pl.cut[i * 4 + 1] * sy;
        const float bx = cx + pl.cut[i * 4 + 2] * sx;
        const float by =      pl.cut[i * 4 + 3] * sy;
        vg_line(ax, cy - ay, bx, cy - by, INK_WELL);
        if (ay > 0.5f || by > 0.5f) vg_line(ax, cy + ay, bx, cy + by, INK_WELL);
    }

    // INSIDE. Dimmer than the outline on purpose: the silhouette is what
    // identifies the ship and the detail is what makes it look built, so the
    // hierarchy has to say which is which -- size and intensity, as ever.
    for (int i = 0; i < pl.nd; i++) {
        const float ax = cx + pl.dtl[i * 4]     * sx;
        const float ay =      pl.dtl[i * 4 + 1] * sy;
        const float bx = cx + pl.dtl[i * 4 + 2] * sx;
        const float by =      pl.dtl[i * 4 + 3] * sy;
        vg_line(ax, cy - ay, bx, cy - by, INK);
        if (ay > 0.5f || by > 0.5f) vg_line(ax, cy + ay, bx, cy + by, INK);
    }
}

// CHANGING SHIP, and the two halves of the panel do it differently.
//
// The CHART tweens. Five numbers against five numbers is a clean interpolation and
// the shape visibly deforms from one class into the next, which is worth more than
// the numbers themselves: you see that CHARIOT is BALLISTA turned inside out.
//
// The HULL cannot tween. Two plan outlines have different point counts and no
// honest correspondence between them -- pairing them up would be inventing a
// relationship that is not there, and it would look like it. So the hull is
// SWAPPED at the half way mark and the swap is hidden under noise, which is also
// the more truthful gesture: this is an instrument re-acquiring, not a ship
// bending into another ship.
//
// Timed off vg.state_t rather than an integrated dt. The renderer has no dt to
// give, and a static accumulator would tick at the frame rate instead of the
// clock -- so a transition would run at one speed on the desktop and another on
// the board, and a replay would not reproduce.
static int   s_tr_to = -1;          // the class the panel is settling on
static int   s_tr_from = -1;        // ...and the one it is leaving
static float s_tr_t0 = -1.0f;
static float s_tr_ax[5] = { 0, 0, 0, 0, 0 };   // the chart AS SHOWN when the change came

// The mask, and it is DELIBERATELY THE SAME MOTIF AS A HIT.
//
// vg_glitch.h keeps one vocabulary for every state where the readout is in
// trouble, so that trouble always looks like the same kind of trouble -- taking a
// hit, being destroyed, straining the engine. A panel exchanging one ship for
// another belongs to that family: the readout is momentarily not showing you
// anything true, and the game already has a way of saying so.
//
// So this borrows the two rules that make the hit read as damage rather than as
// sparkle:
//
//   THE HASH IS SHARED. vg_glitch_hash, not a private one, so the grain is the
//   same grain. Also not vg_frand -- a menu has no business drawing on the
//   replay stream.
//
//   IT IS BUCKETED, NOT PER FRAME. Sampling every frame strobes at whatever rate
//   the panel happens to run at and reads as noise, which has no location and so
//   nothing wrong with it. Held for a bucket, a fault sits somewhere specific
//   long enough to be seen. That is the whole difference.
//
// A TEAR IS SHIFTED SIGNAL, not a stripe laid on top -- the glitch header is
// explicit about it. The band is knocked out and a fragment redrawn beside
// itself, so the picture in the band is displaced rather than merely covered.
static void draw_plan_noise(float p) {
    const float d = sinf(p * 3.14159265f);      // 0 at the ends, 1 in the middle
    if (d <= 0.02f) return;

    const int x0 = SEL_PANEL_X + 8, w = SEL_PANEL_W - 16;
    const int y0 = SEL_MODEL_Y,     h = SEL_MODEL_H;
    const uint32_t bucket = (uint32_t)(vg.state_t * SEL_TEAR_RATE);

    // Tears first: wide bands, few, and the severity decides how many.
    const int nt = 1 + (int)(4.0f * d);
    for (int i = 0; i < nt; i++) {
        const uint32_t r = vg_glitch_hash(bucket * 2654435761u + (uint32_t)i * 2246822519u);
        const int by = y0 + (int)((r >> 3)  % (uint32_t)h);
        const int bh = 2 + (int)((r >> 11) % 5u);
        const int dx = (int)((r >> 17) % 27u) - 13;
        if (by + bh > y0 + h) continue;
        vg_fill_rect(x0, by, w, bh, INK_WELL);
        // The fragment, put back beside where it came from.
        const int fw = 18 + (int)((r >> 23) % 60u);
        int fx = x0 + (int)((r >> 5) % (uint32_t)(w - fw)) + dx;
        if (fx < x0) fx = x0;
        if (fx + fw > x0 + w) fx = x0 + w - fw;
        vg_fill_rect(fx, by, fw, (bh > 2) ? 2 : bh, INK_BRIGHT);
    }

    // ...then grain over the top, so the tears are not the only texture.
    const int ng = (int)(18.0f * d);
    for (int i = 0; i < ng; i++) {
        const uint32_t r = vg_glitch_hash(bucket * 40503u + (uint32_t)i * 2654435761u);
        const int px = x0 + (int)((r >> 4)  % (uint32_t)w);
        const int py = y0 + (int)((r >> 13) % (uint32_t)h);
        const int len = 3 + (int)((r >> 22) % 9u);
        if (px + len > x0 + w) continue;
        vg_line((float)px, (float)py, (float)(px + len), (float)py,
                ((r & 3u) == 0u) ? INK_BRIGHT : INK_TRACE);
    }
}

void vg_draw_select(void) {
    const bool opp = (vg.gym && vg.sel_opp);
    const int  cur = opp ? (int)vg.gym_opp : (int)vg.ship;

    // A NEW SELECTION. The chart is snapshotted AS SHOWN rather than as the class
    // it was heading for, so spinning the wheel fast leaves from wherever the
    // shape had actually reached instead of jumping back to the last full stop.
    float ax_now[5];
    vg_ship_axes(vg_spec((ShipClass)cur), ax_now);
    if (cur != s_tr_to) {
        if (s_tr_to >= 0) {
            const float q = (s_tr_t0 < 0.0f) ? 1.0f
                          : (vg.state_t - s_tr_t0) / SEL_MORPH_TIME;
            const float e = (q >= 1.0f) ? 1.0f : (q <= 0.0f ? 0.0f : q * q * (3.0f - 2.0f * q));
            float ax_prev[5];
            vg_ship_axes(vg_spec((ShipClass)s_tr_to), ax_prev);
            for (int i = 0; i < 5; i++)
                s_tr_ax[i] = s_tr_ax[i] + (ax_prev[i] - s_tr_ax[i]) * e;
        } else {
            for (int i = 0; i < 5; i++) s_tr_ax[i] = ax_now[i];
        }
        s_tr_from = s_tr_to;
        s_tr_to   = cur;
        s_tr_t0   = vg.state_t;
    }

    float p = (s_tr_t0 < 0.0f) ? 1.0f : (vg.state_t - s_tr_t0) / SEL_MORPH_TIME;
    if (p < 0.0f) p = 0.0f;
    if (p > 1.0f) p = 1.0f;
    const float ease = p * p * (3.0f - 2.0f * p);        // smoothstep

    // ONLY THE HULL LAGS. The words follow the wheel immediately, because the wheel
    // is what the thumb just moved and a panel that disagrees with it reads as the
    // screen not having noticed. The hull has to lag -- it cannot tween, so it is
    // swapped at the half way mark -- and that mismatch is invisible because it
    // happens under the thickest part of the noise.
    const int shown = (ease < 0.5f && s_tr_from >= 0) ? s_tr_from : cur;

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
    const int lit = s_press_held ? vg_select_row_at(s_press_x, s_press_y)
                                 : SEL_ROW_NONE;
    if (lit != SEL_ROW_NONE)
        vg_fill_rect(SEL_WHEEL_X, detent + lit * SEL_WHEEL_PITCH - 18,
                     SEL_WHEEL_W, 36, INK_TRACE);

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
                      detent + k * SEL_WHEEL_PITCH - (sc * 7) / 2,
                      nm, col, sc, tr);
    }

    // The detent: two rules bracketing the selected row, plus the spine down the
    // left edge. That spine is the same 6px inverse-video mark the cards carried
    // -- the shape of the screen changed, the vocabulary did not.
    // The detent grew with the name it brackets: a 21px glyph inside a 36px well
    // leaves three pixels top and bottom, which reads as the row being too small
    // for its own word rather than as a selection.
    vg_fill_rect(SEL_WHEEL_X, detent - 21, SEL_WHEEL_W, 1, INK_TRACE);
    vg_fill_rect(SEL_WHEEL_X, detent + 20, SEL_WHEEL_W, 1, INK_TRACE);
    vg_fill_rect(SEL_WHEEL_X, detent - 21, SEL_SPINE_W, 42, INK_BRIGHT);

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
    vg_rect(SEL_PANEL_X, SEL_PANEL_Y, SEL_PANEL_W, SEL_PANEL_H, INK_TRACE);

    // THE NAME, AND THEN WHAT IT CARRIES. There used to be a tagline between them
    // and there is not any more.
    //
    // It was the one line on the panel still drawn at scale 1, which on this
    // display is 0.57mm -- and rather than find it room at a readable size, it
    // went. A tagline tells you how to feel about a hull before you have flown
    // it, and the two rows under it tell you what the hull actually does. Let the
    // player decide which one they want; the specification is the part that is
    // any use in deciding.
    //
    // ShipSpec::tagline is still there and nothing reads it now.
    {
        const ShipSpec* hs = vg_spec((ShipClass)cur);
        const int bx = SEL_PANEL_X + SEL_FIELD_PAD;
        const int bw = SEL_PANEL_W - 2 * SEL_FIELD_PAD;

        vg_text_track(SEL_PANEL_X
                          + (SEL_PANEL_W
                             - vg_text_track_width(hs->name, 3, SEL_TITLE_TRACK)) / 2,
                      SEL_PANEL_Y + 6, hs->name, INK_MAX, 3, SEL_TITLE_TRACK);

        // MSL is what it CARRIES, WPN is what the system DOES with it. MSL is not
        // a new word either: the rack instrument in flight is labelled MSL, so it
        // already means "the round" to anyone who has flown.
        // The tagline gets air under it and the two fields close up. They are a
        // PAIR -- the round and what the system does with it -- and they were
        // sitting further apart than either sat from the subtitle above, which
        // read as three separate lines rather than a caption and a block.
        const int b = field_wrap(bx, SEL_PANEL_Y + 34, bw, "MSL",
                                 vg_wpn_name(hs->wpn), INK_BRIGHT);
        field_wrap(bx, b + 8, bw, "WPN", vg_wpn_desc(hs->wpn), INK);
    }

    {
        float ax[5];
        for (int i = 0; i < 5; i++)
            ax[i] = s_tr_ax[i] + (ax_now[i] - s_tr_ax[i]) * ease;
        draw_chart(ax);
    }

    vg_fill_rect(SEL_PANEL_X + 8, SEL_MODEL_Y - 8, SEL_PANEL_W - 16, 1, INK_TRACE);
    draw_plan_view(vg_spec((ShipClass)shown), shown);
    draw_plan_noise(p);

    vg_console_key(VG_CON_KEY, (vg.gym && !vg.sel_opp) ? "NEXT" : "ENTER");
    vg_console_close();
}

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
    // Knock the whole frame back a stop so the instruments read as suspended
    // rather than live, without hiding the fight you are about to return to.
    for (int y = 0; y < SCR_H; y += 2) vg_fill_rect(0, y, SCR_W, 1, COL_BLACK);

    if (vg.pause_page == 1) {
        centred(120, "CONFIG", INK_MAX, 5);
        volume_slider(PAU_SLD_MUSIC_Y, "MUSIC", vg_vol.music);
        volume_slider(PAU_SLD_SFX_Y,   "SFX",   vg_vol.sfx);
        check_row(PAU_CHK_Y, "SCANLINES", vg_disp.scanlines);
        vg_button(PAU_BTN_X, PAU_BACK_Y, PAU_BTN_W, PAU_BTN_H, "BACK", true, true);
        return;
    }

    centred(120, "PAUSED", INK_MAX, 5);

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

