#include "vg_screens.h"
#include "vg_bezel.h"
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

// WHAT THE SYSTEM DOES, as a specification rather than a pitch.
//
// It read "YOU FLY EVERY ROUND ALL THE WAY IN", which is how you would sell the
// class to somebody. The shape is [guidance type]. [what makes it different] --
// the words a manual would use, not the words a recruiter would.
//
// ANY COUNT HERE IS DERIVED AND THE PROSE IS NOT, which is the right split: a
// magazine moves under tuning and a guidance principle does not. BALLISTA has been
// semi-active since it was drawn, but its reach went 1600 to 4200 and LANCE's
// stack time went 1.2 to 0.95 inside one sitting.
//
// Only LANCE names a number now, because four IS the mechanic there -- the salvo
// size is what the class is about. CHARIOT's twelve is legible from the twelve
// cells in its bay, and saying it twice was saying it once too often.
static void wpn_how(const ShipSpec* sp, char* out, int n) {
    if (!sp) { out[0] = 0; return; }
    switch (sp->wpn) {
    case WPN_ARAAM:
        // "ON CONTACT", not "ON RADAR DETECTION": the MSL row directly above
        // already reads AR-AAM ACTIVE RADAR, so the word is on the panel once and
        // a contact here can only be a radar contact. It also has to be shorter --
        // the long form measured 265px against a 264px panel and touched both
        // borders.
        snprintf(out, n, "ACTIVE SEEKER. REARM ON CONTACT");
        break;
    case WPN_RFAAM:
        snprintf(out, n, "RAPID FIRE PROXIMITY FUSE");
        break;
    case WPN_SLAAM:
        // NOT "multi target". Every banked round launches at vg_wpn.target -- one
        // contact, four rounds. Firing a stacked bay across several contacts is a
        // real and interesting mechanic and this class does not have it; saying so
        // here would be the screen promising something the code does not do.
        //
        // The tagline says MULTI-ROUND for the same reason, and it is worth
        // knowing it was MULTI-LOCK first: ROUND is true where LOCK is not. One
        // lock on one contact is the premise of the class and not a gap in it.
        snprintf(out, n, "SALVO %d. STACKED LOCK RELEASE", sp->magazine);
        break;
    case WPN_SAAAM:
        snprintf(out, n, "SEMI-ACTIVE, MANUAL MISSILE GUIDANCE");
        break;
    default: out[0] = 0; break;
    }
}

// A LABELLED FIELD: an INVERSE-VIDEO tab, then the value beside it.
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
// TWO COLUMNS, PASSED IN, because the rows have to agree. Centring each row on
// its own put the two tabs at different x -- and since both labels are the same
// width, the entire offset was the length of the value beside them, which is not
// a thing the caption column should know about. Read down, it looked like two
// unrelated captions instead of one specification block.
//
// The fill still hugs its OWN label, so a longer caption later grows its tab
// rather than shifting the values.
static void field(int lx, int vx, int y,
                  const char* label, const char* value, uint16_t vcol) {
    vg_fill_rect(lx - 2, y - 1, vg_text_width(label, 1) + 4, 9, INK);
    vg_text(lx, y, label, INK_ONFILL, 1);
    vg_text(vx, y, value, vcol, 1);
}

// WHERE THE TWO COLUMNS SIT. Centred on the WIDER row, so the block keeps the
// panel's axis, and the shorter row is flush with it rather than centred inside
// it.
//
// The GAP GIVES FIRST. A field that will not fit is a writing problem and gets
// fixed in the string, but the panel must not be the thing that reports it: an
// overlong value would otherwise draw its tab outside the left border and run
// its last characters past the right one. Closing the gap to 4 buys five
// characters, and past that the block pins to the margin and simply clips.
static void field_cols(const char* l0, const char* v0,
                       const char* l1, const char* v1, int* lx, int* vx) {
    const int w0 = vg_text_width(l0, 1), w1 = vg_text_width(l1, 1);
    const int lw = (w0 > w1) ? w0 : w1;
    const int a  = vg_text_width(v0, 1), b = vg_text_width(v1, 1);
    const int vw = (a > b) ? a : b;
    const int room = SEL_PANEL_W - 2 * SEL_FIELD_PAD;
    const int gap  = (lw + 9 + vw <= room) ? 9 : 4;
    int x = SEL_PANEL_X + (SEL_PANEL_W - (lw + gap + vw)) / 2;
    if (x < SEL_PANEL_X + SEL_FIELD_PAD) x = SEL_PANEL_X + SEL_FIELD_PAD;
    *lx = x;
    *vx = x + lw + gap;
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

// ===========================================================================
// THE CONSOLE, AS A PAIR OF BRACKETS
//
// Two screens are bolted into this machine now -- callsign registration and ship
// select -- and they are the same machine, so the chassis, the running banner,
// the key and the curve of the glass belong in one place rather than in each of
// them. What a screen supplies is its banner, its note and the word on its key.
//
// console_open leaves the WARP BRACKET OPEN. Everything a screen draws between
// the two calls is on the glass and bends; the banner and the key are drawn
// outside it, because they sit in windows cut into the plating and a lit inset
// that bows with the tube reads as a decal stuck on it.
// ===========================================================================

// The banner window, the ticker across it, and then the glass.
void console_open(const char* title, const char* note) {
    vg_fill_rect(BEZEL_CONSOLE_BAR_TOP_BOX_X0, BEZEL_CONSOLE_BAR_TOP_BOX_Y0,
                 BEZEL_CONSOLE_BAR_TOP_BOX_X1 - BEZEL_CONSOLE_BAR_TOP_BOX_X0 + 1,
                 BEZEL_CONSOLE_BAR_TOP_BOX_Y1 - BEZEL_CONSOLE_BAR_TOP_BOX_Y0 + 1,
                 INK_WELL);

    const int   bx    = BEZEL_CONSOLE_BAR_TOP_X0;
    const int   bw    = BEZEL_CONSOLE_BAR_TOP_X1 - bx + 1;
    const int   scale = note ? 2 : 3;
    const int   tw    = vg_text_width(title, scale);

    // RIGHT TO LEFT, and it WRAPS. Moving right the block's tail enters first and
    // the word arrives back to front -- SHIP SELECT. And a single pass leaves the
    // glass empty between readings, which looks like a machine that has stopped;
    // repeating every word-plus-gap makes the tail of one pass the head of the
    // next.
    //
    // vg.state_t, not an integrated dt: the renderer has no dt to give, and an
    // accumulated one runs the ticker at the frame rate rather than the clock.
    //
    // Clipped to the window BOX. It is needed because the screen aperture notches
    // up either side of this bar, so there are exempt pixels off both ends that
    // the chassis cannot paint over -- text ran out of the window and stayed on
    // screen. The box lets the letters reach the glass; the chassis cuts the
    // chamfer.
    const int   period = tw + SEL_CHYRON_GAP;
    const float u   = vg.state_t * SEL_CHYRON_RATE;
    const int   off = (int)(u - floorf(u / (float)period) * (float)period);
    const int   ty  = SEL_TITLE_CY - (note ? 12 : 10);

    vg_rast_viewport(BEZEL_CONSOLE_BAR_TOP_BOX_X0, BEZEL_CONSOLE_BAR_TOP_BOX_Y0,
                     BEZEL_CONSOLE_BAR_TOP_BOX_X1 - BEZEL_CONSOLE_BAR_TOP_BOX_X0 + 1,
                     BEZEL_CONSOLE_BAR_TOP_BOX_Y1 - BEZEL_CONSOLE_BAR_TOP_BOX_Y0 + 1);

    // WHITE, because the machine is not the one asking. A banner is the
    // tournament talking to you through the terminal -- the same voice that
    // speaks over a match -- so it takes COL_IFT. The key stays amber: that is
    // furniture, and the difference between the two is the point.
    for (int tx = bx + bw - off; tx + tw > bx; tx -= period)
        vg_text(tx, ty, title, COL_IFT, scale);

    // A note does NOT run. It is a sentence to be read once, not a banner, and a
    // moving one would be the only thing on the screen asking to be chased.
    if (note) centred(SEL_TITLE_CY + 4, note, INK, 1);
    vg_rast_viewport_full();

    // GLASS FROM HERE. The plating is cold steel and dead flat; the display under
    // it is a tube, and a tube bends its picture. The curve pulls the corners of
    // the picture inward and the chassis paints last over what is left, so the
    // display does not end at a drawn border -- it disappears beneath the steel.
    //
    // Finer chords than the cockpit uses: a panel border is 266px on a side and
    // at the default that is five straight pieces with visible joints.
    vg_hud_warp(true, SEL_GLASS_WARP);
    vg_hud_warp_seg(SEL_GLASS_SEG);

    // --- what makes it a display rather than a window ---------------------
    //
    // Both of these go down FIRST, so the screen's own instruments draw over
    // them. That is the right way round and not just the safe one: they belong
    // to the DISPLAY, not to what it is showing, and a fiducial that crossed a
    // word would be reading as content.
    //
    // REGISTRATION CROSSES, tiled. Every instrument panel ever built has them --
    // they are alignment marks, the thing a display is checked against rather
    // than anything it is telling you -- and a regular grid of them across the
    // glass is what says "this readout was manufactured". They are in the
    // concept art for the same reason.
    //
    // They also do a second job here, which is why they are inside the warp
    // bracket: a curve needs something regular laid across it to be seen at all.
    // Bent text is just badly set and a bent border could be a drawn shape, but a
    // grid of identical marks that are not on a grid any more can only be glass.
    for (int gy = SEL_AP_Y0 + SEL_TICK_STEP / 2; gy < SEL_AP_Y1; gy += SEL_TICK_STEP)
        for (int gx = SEL_AP_X0 + SEL_TICK_STEP / 2; gx < SEL_AP_X1; gx += SEL_TICK_STEP) {
            vg_line((float)(gx - SEL_TICK_ARM), (float)gy,
                    (float)(gx + SEL_TICK_ARM), (float)gy, INK_TRACE);
            vg_line((float)gx, (float)(gy - SEL_TICK_ARM),
                    (float)gx, (float)(gy + SEL_TICK_ARM), INK_TRACE);
        }

    // THE SWEEP. One line down the glass, on a loop, which is the cheapest
    // possible way to say the hardware is powered: a still picture is a picture,
    // and a still picture with one thing crossing it on a clock is a MACHINE
    // showing you a picture.
    //
    // IT RUNS WIDE OF THE GLASS ON PURPOSE. The warp is a barrel curve, so it
    // pulls a point inward in proportion to its distance from the centre -- and
    // the ends of a line spanning the whole aperture are the furthest points on
    // it. Drawn edge to edge the sweep came up SHORT of both edges, which read as
    // the line being cut off rather than as the glass being curved. It is drawn
    // past both edges now and the chassis trims it, which is the same division of
    // labour the banner uses.
    //
    // AND IT IS NOT A METRONOME. A constant rate reads as a screensaver; this is
    // meant to be a tube that has been running in a shed in orbit for years.
    // Every pass gets its own pace from a hash of the pass number:
    //
    //   AN EXPONENT ON THE RAMP, which bends the speed without moving the ends.
    //   f and f^e both run 0 to 1, so a pass still starts at the top and finishes
    //   at the bottom however hard it is bent -- the seam between passes stays a
    //   seam and never becomes a jump.
    //
    //   A STALL, on one pass in eight. The line stops partway down, sits there,
    //   and then carries on. It is the single most convincing thing here: broken
    //   hardware does not run slowly, it HESITATES.
    //
    //   AND THE LINE BREAKS UP on one pass in four, into pieces with gaps. A
    //   sweep that is always whole is a drawn object; one that is sometimes in
    //   bits is a signal.
    //
    // All of it is a pure function of vg.state_t. Nothing is integrated and
    // nothing is sampled per frame, so it runs at the same pace on the desktop
    // and on the board and a replay reproduces it.
    {
        const float    hh   = (float)(SEL_AP_Y1 - SEL_AP_Y0);
        const float    u    = vg.state_t * SEL_SWEEP_RATE;
        const float    pass = floorf(u / hh);
        const uint32_t ph   = vg_glitch_hash((uint32_t)pass * 2654435761u);

        float f = u / hh - pass;
        f = powf(f, 0.65f + (float)(ph & 255u) * (0.85f / 255.0f));

        if (((ph >> 9) & 7u) == 0u) {
            const float at = 0.20f + (float)((ph >> 12) & 127u) * (0.55f / 127.0f);
            if (f > at && f < at + 0.10f) f = at;
        }

        const float sy = (float)SEL_AP_Y0 + f * hh;
        const float x0 = (float)(SEL_AP_X0 - SEL_SWEEP_OVER);
        const float x1 = (float)(SEL_AP_X1 + SEL_SWEEP_OVER);

        // Two lines rather than one -- a faint band with a brighter edge leading
        // it -- because a single hairline reads as a scratch ON the glass where a
        // pair reads as something passing behind it.
        const int n = (((ph >> 17) & 3u) == 0u) ? 3 : 1;
        for (int i = 0; i < n; i++) {
            const float a = x0 + (x1 - x0) * ((float)i / (float)n);
            const float b = x0 + (x1 - x0) * ((float)(i + 1) / (float)n)
                          - ((n > 1) ? 26.0f : 0.0f);
            vg_line(a, sy, b, sy, INK_FAINT);
            vg_line(a, sy + 2.0f, b, sy + 2.0f, INK_TRACE);
        }
    }

    // AND THE GLASS FAULTS, now and then.
    //
    // Same vocabulary as a hit and the same reason as the plan view's mask: this
    // game already has one way of saying a readout is in trouble, and a second
    // one invented here would read as a different kind of trouble. The hash is
    // vg_glitch_hash and the sampling is BUCKETED, which is what separates damage
    // from noise -- noise is everywhere and so has nothing wrong with it, while a
    // fault that holds still for a moment is somewhere specific.
    //
    // Rare, and that is the whole setting. A panel that glitches constantly is a
    // stylistic effect; one that is clean for ten seconds and then tears for half
    // of one is a panel that is BROKEN, and the difference is entirely in how
    // often it happens.
    {
        const uint32_t slow = (uint32_t)(vg.state_t * SEL_FAULT_RATE);
        const uint32_t sh   = vg_glitch_hash(slow * 40503u + 17u);
        if ((sh % 6u) == 0u) {
            const int x0 = SEL_AP_X0, w = SEL_AP_X1 - SEL_AP_X0;
            const int y0 = SEL_AP_Y0, h = SEL_AP_Y1 - SEL_AP_Y0;

            // THE FAST CLOCK inside the slow one. The slow bucket decides that
            // the panel is in trouble and holds that decision for the best part
            // of a second; this one re-places the damage several times inside it,
            // so the fault stays put while the picture in it does not.
            const uint32_t fast = (uint32_t)(vg.state_t * 22.0f);

            const int nb = 2 + (int)((sh >> 3) % 3u);
            for (int i = 0; i < nb; i++) {
                const uint32_t g = vg_glitch_hash(fast * 2654435761u
                                                  + (uint32_t)i * 2246822519u);
                const int by = y0 + (int)((g >> 3) % (uint32_t)h);
                const int bh = 3 + (int)((g >> 11) % 7u);
                if (by + bh > y0 + h) continue;

                // A TEAR IS SHIFTED SIGNAL, not a stripe laid on top: the band is
                // knocked out and a fragment put back beside where it came from.
                //
                // THE FRAGMENT IS BRIGHT, and the first version's was not. It used
                // the plan view's colours, which work there because that slot is
                // full of lit ship and the knock-out has something to remove. Out
                // here the glass is mostly empty and INK_WELL on near-black is
                // nothing at all -- the fault only showed where it happened to
                // cross a word, which is a few percent of the rows. Displaced
                // signal has to be signal.
                vg_fill_rect(x0, by, w, bh, INK_WELL);
                const int fw = 30 + (int)((g >> 23) % 110u);
                int fx = x0 + (int)((g >> 5) % (uint32_t)(w - fw))
                       + (int)((g >> 17) % 41u) - 20;
                if (fx < x0)          fx = x0;
                if (fx + fw > x0 + w) fx = x0 + w - fw;
                vg_fill_rect(fx, by, fw, (bh > 3) ? 2 : bh, INK_BRIGHT);
            }

            // ...and grain over the top, so the tears are not the only texture
            // and the whole pane looks disturbed rather than three rows of it.
            for (int i = 0; i < 14; i++) {
                const uint32_t g = vg_glitch_hash(fast * 40503u
                                                  + (uint32_t)i * 2654435761u);
                const int px = x0 + (int)((g >> 4)  % (uint32_t)w);
                const int py = y0 + (int)((g >> 13) % (uint32_t)h);
                const int ln = 4 + (int)((g >> 22) % 11u);
                if (px + ln > x0 + w) continue;
                vg_line((float)px, (float)py, (float)(px + ln), (float)py,
                        ((g & 3u) == 0u) ? INK : INK_TRACE);
            }
        }
    }
}

// Back to flat, then the key, then the steel over everything.
void console_close(const char* key) {
    vg_hud_warp(false, 1.0f);

    // THE CHASSIS ALREADY DREW THE KEY'S BOX. vg_button paints a well, a 2px
    // frame and corner ticks, and every one of those is a second border inside
    // the lit window the metal provides -- a button on a button. What is left of
    // a key when the machine owns its box is the label, the line under it that
    // marks the primary action, and the one thing a key must do: change while it
    // is held.
    const int  lw   = vg_text_width(key, 3);
    const int  lx   = SEL_GO_X + (SEL_GO_W - lw) / 2;
    const int  ly   = SEL_GO_Y + (SEL_GO_H - 21) / 2;
    const bool down = vg_press_in(SEL_GO_X, SEL_GO_Y, SEL_GO_W, SEL_GO_H);

    vg_fill_rect(BEZEL_CONSOLE_BAR_BOT_BOX_X0, BEZEL_CONSOLE_BAR_BOT_BOX_Y0,
                 BEZEL_CONSOLE_BAR_BOT_BOX_X1 - BEZEL_CONSOLE_BAR_BOT_BOX_X0 + 1,
                 BEZEL_CONSOLE_BAR_BOT_BOX_Y1 - BEZEL_CONSOLE_BAR_BOT_BOX_Y0 + 1,
                 down ? INK_TRACE : INK_WELL);
    vg_text(lx, ly, key, INK_MAX, 3);
    vg_fill_rect(lx, ly + 24, lw, 2, down ? INK_MAX : INK_BRIGHT);

    // LAST, so the steel masks whatever ran past the glass.
    vg_bezel_use(&BEZEL_CONSOLE);
    vg_bezel_prim();
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

    console_open(vg.gym ? (opp ? "SELECT OPPONENT" : "SELECT YOUR SHIP")
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
        const int      sc  = (a <= 1) ? 2 : 1;
        const int      tr  = (sc == 2) ? SEL_NAME_TRACK : 1;
        const uint16_t col = (a == 1) ? INK : INK_FAINT;
        vg_text_track(SEL_WHEEL_X
                          + (SEL_WHEEL_W - vg_text_track_width(nm, sc, tr)) / 2,
                      detent + k * SEL_WHEEL_PITCH - (sc * 7) / 2,
                      nm, col, sc, tr);
    }

    // The detent: two rules bracketing the selected row, plus the spine down the
    // left edge. That spine is the same 6px inverse-video mark the cards carried
    // -- the shape of the screen changed, the vocabulary did not.
    vg_fill_rect(SEL_WHEEL_X, detent - 18, SEL_WHEEL_W, 1, INK_TRACE);
    vg_fill_rect(SEL_WHEEL_X, detent + 17, SEL_WHEEL_W, 1, INK_TRACE);
    vg_fill_rect(SEL_WHEEL_X, detent - 18, SEL_SPINE_W, 36, INK_BRIGHT);

    {
        const char* nm = vg_spec((ShipClass)cur)->name;
        vg_text_track(SEL_WHEEL_X
                          + (SEL_WHEEL_W
                             - vg_text_track_width(nm, 2, SEL_NAME_TRACK)) / 2,
                      detent - 7, nm, INK_MAX, 2, SEL_NAME_TRACK);
    }

    // --- the panel ---------------------------------------------------------
    const ShipSpec* sp = vg_spec((ShipClass)cur);
    vg_rect(SEL_PANEL_X, SEL_PANEL_Y, SEL_PANEL_W, SEL_PANEL_H, INK_TRACE);

    // IDENTITY FIRST, THEN FIELDS. The name and its tagline are one thing -- a
    // tagline is a subtitle and belongs against the name it subtitles -- and the
    // two labelled rows beneath are specification. Interleaved, the unlabelled
    // tagline sat between two labelled rows and read as a field whose caption had
    // gone missing.
    vg_text_track(SEL_PANEL_X
                      + (SEL_PANEL_W
                         - vg_text_track_width(sp->name, 3, SEL_TITLE_TRACK)) / 2,
                  SEL_PANEL_Y + 8, sp->name, INK_MAX, 3, SEL_TITLE_TRACK);
    vg_text(SEL_PANEL_X + (SEL_PANEL_W - vg_text_width(sp->tagline, 1)) / 2,
            SEL_PANEL_Y + 34, sp->tagline, INK_FAINT, 1);

    // MSL is what it CARRIES, WPN is what the system DOES with it. MSL is not a
    // new word either: the rack instrument in flight is labelled MSL, so it
    // already means "the round" to anyone who has flown.
    {
        char how[48];
        wpn_how(sp, how, sizeof(how));
        const char* msl = vg_wpn_name(sp->wpn);
        int lx, vx;
        field_cols("MSL", msl, "WPN", how, &lx, &vx);
        field(lx, vx, SEL_PANEL_Y + 50, "MSL", msl, INK_BRIGHT);
        field(lx, vx, SEL_PANEL_Y + 64, "WPN", how, INK);
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

    console_close((vg.gym && !vg.sel_opp) ? "NEXT" : "ENTER");
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

