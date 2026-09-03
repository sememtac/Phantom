#include "vg_screens.h"
#include "vg_sfx.h"
#include "vg_course.h"
#include "vg_draw.h"
#include "vg_game.h"
#include <stdio.h>
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
    const float dy = y - (float)SEL_WHEEL_MID;
    int k = (int)lroundf(dy / (float)SEL_WHEEL_PITCH);
    if (k < -2) k = -2;
    if (k >  2) k =  2;
    return k;
}

bool vg_select_confirm_at(float x, float y) {
    return vg_in_rect(x, y, SEL_GO_X, SEL_GO_Y, SEL_GO_W, SEL_GO_H);
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

static void draw_chart(const ShipSpec* sp) {
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
    float ax[5];
    vg_ship_axes(sp, ax);
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

void vg_draw_select(void) {
    const bool opp = (vg.gym && vg.sel_opp);
    const int  cur = opp ? (int)vg.gym_opp : (int)vg.ship;

    centred(30, vg.gym ? (opp ? "SELECT OPPONENT" : "SELECT YOUR SHIP")
                       : "SELECT SHIP", INK_MAX, 3);
    if (vg.gym)
        centred(62, opp ? "THEY RESPAWN UNTIL YOU LEAVE"
                        : "PRACTICE -- NOTHING IS SCORED", INK, 2);

    // --- the wheel ---------------------------------------------------------
    vg_rect(SEL_WHEEL_X, SEL_WHEEL_Y, SEL_WHEEL_W, SEL_WHEEL_H, INK_TRACE);

    // Neighbours, dimmer the further out, so you can see what is coming. Only
    // one either side while the roster is four: at two out, a four-item wheel
    // shows the SAME class above and below, which reads as a drawing fault.
    const int kmax = (SHIP_CLASSES >= 5) ? 2 : 1;
    for (int k = -kmax; k <= kmax; k++) {
        if (k == 0) continue;
        const int      idx = (cur + k + SHIP_CLASSES * 2) % SHIP_CLASSES;
        const char*    nm  = vg_spec((ShipClass)idx)->name;
        const int      sc  = (k == -1 || k == 1) ? 2 : 1;
        const uint16_t col = (k == -1 || k == 1) ? INK_TRACE : INK_ONFILL;
        vg_text(SEL_WHEEL_X + (SEL_WHEEL_W - vg_text_width(nm, sc)) / 2,
                SEL_WHEEL_MID + k * SEL_WHEEL_PITCH - (sc * 7) / 2, nm, col, sc);
    }

    // The detent: two rules bracketing the selected row, plus the spine down the
    // left edge. That spine is the same 6px inverse-video mark the cards carried
    // -- the shape of the screen changed, the vocabulary did not.
    vg_fill_rect(SEL_WHEEL_X, SEL_WHEEL_MID - 20, SEL_WHEEL_W, 1, INK_TRACE);
    vg_fill_rect(SEL_WHEEL_X, SEL_WHEEL_MID + 19, SEL_WHEEL_W, 1, INK_TRACE);
    vg_fill_rect(SEL_WHEEL_X, SEL_WHEEL_MID - 20, SEL_SPINE_W, 40, INK_BRIGHT);

    {
        const char* nm = vg_spec((ShipClass)cur)->name;
        vg_text(SEL_WHEEL_X + (SEL_WHEEL_W - vg_text_width(nm, 2)) / 2,
                SEL_WHEEL_MID - 7, nm, INK_MAX, 2);
    }

    // --- the panel ---------------------------------------------------------
    const ShipSpec* sp = vg_spec((ShipClass)cur);
    vg_rect(SEL_PANEL_X, SEL_PANEL_Y, SEL_PANEL_W, SEL_PANEL_H, INK_TRACE);

    vg_text(SEL_PANEL_X + (SEL_PANEL_W - vg_text_width(sp->name, 3)) / 2,
            SEL_PANEL_Y + 8, sp->name, INK_MAX, 3);
    {
        // The weapon system, which is what the four classes now ARE and the one
        // thing this screen never said.
        const char* w = vg_wpn_name(sp->wpn);
        vg_text(SEL_PANEL_X + (SEL_PANEL_W - vg_text_width(w, 1)) / 2,
                SEL_PANEL_Y + 34, w, INK_BRIGHT, 1);
    }
    vg_text(SEL_PANEL_X + (SEL_PANEL_W - vg_text_width(sp->tagline, 1)) / 2,
            SEL_PANEL_Y + 48, sp->tagline, INK, 1);

    draw_chart(sp);

    vg_fill_rect(SEL_PANEL_X + 8, SEL_MODEL_Y - 8, SEL_PANEL_W - 16, 1, INK_TRACE);
    draw_plan_view(sp, cur);

    vg_button(SEL_GO_X, SEL_GO_Y, SEL_GO_W, SEL_GO_H,
              (vg.gym && !vg.sel_opp) ? "NEXT" : "ENTER", true, true);
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

