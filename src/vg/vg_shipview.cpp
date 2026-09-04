#include "vg_shipview.h"
#include "vg_ui.h"
#include "vg_draw.h"
#include "vg_glitch.h"
#include "vg_game.h"
#include <math.h>
#include <stdio.h>

// The ship, drawn: the name, what it carries, the shape its five numbers make,
// and the hull from above. Lifted out of vg_select.cpp, which was the only
// screen that could ask for any of it. See vg_shipview.h for what the split is
// for and what a caller now has to hold.

// WHERE EVERYTHING SITS, worked out once from the rectangle the panel was given
// and passed down. That is the whole of what "it takes a rectangle" costs: the
// helpers used to read the select screen's constants directly, which is what
// made them the select screen's.
struct Lay {
    int x, y, w, h;     // the panel itself
    int cx, cy, r;      // the chart
    int my, mh;         // the plan view's slot
    int bx, bw;         // the inside of the panel, where a labelled field goes
};

static Lay lay_of(int x, int y, int w, int h) {
    Lay L;
    L.x  = x; L.y = y; L.w = w; L.h = h;
    L.cx = x + w / 2;
    L.cy = y + SHIPVIEW_CHART_DY;
    L.r  = SHIPVIEW_CHART_R;
    L.my = y + SHIPVIEW_MODEL_DY;
    L.mh = SHIPVIEW_MODEL_H;
    L.bx = x + SHIPVIEW_FIELD_PAD;
    L.bw = w - 2 * SHIPVIEW_FIELD_PAD;
    return L;
}

// THE WIDEST HALF-SPAN IN THE ROSTER, found by looking rather than by being
// told. See the note where it is spent.
//
// Once, on the first draw, over four short outlines -- the chart's trig tables
// are built the same way and for the same reason.
static float plan_max_half_span(void) {
    static float s_span = 0.0f;
    if (s_span > 0.0f) return s_span;
    for (int c = 0; c < SHIP_CLASSES; c++) {
        const VgShipPlan& pl = vg_ship_plan[c];
        if (!pl.pts) continue;
        for (int i = 0; i < pl.n; i++) {
            const float y = fabsf(pl.pts[i * 2 + 1]);
            if (y > s_span) s_span = y;
        }
    }
    return s_span;
}

void vg_ship_axes(const ShipSpec* sp, float out[5]) {
    if (!sp) { for (int i = 0; i < 5; i++) out[i] = 0.0f; return; }

    // RATE is vg_ship_rate, defined with the roster rather than here because the
    // invariants check that same figure against the range it is plotted on.
    const float v[5] = { sp->speed_max, sp->hull, sp->lock_range,
                         sp->msl_damage, vg_ship_rate(*sp) };
    // The display ranges are deliberately WIDER than the roster, so a class sits
    // somewhere inside its axis rather than at an end, and so adding a fifth ship
    // does not silently reshape the other four. Min-max across the roster would
    // do exactly that. The invariants at the foot of vg_ship.cpp are what stop a
    // class leaving them without saying so.
    const float lo[5] = { SHIP_AX_SPEED_LO, SHIP_AX_HULL_LO, SHIP_AX_RANGE_LO,
                          SHIP_AX_DMG_LO,   SHIP_AX_RATE_LO };
    const float hi[5] = { SHIP_AX_SPEED_HI, SHIP_AX_HULL_HI, SHIP_AX_RANGE_HI,
                          SHIP_AX_DMG_HI,   SHIP_AX_RATE_HI };

    for (int i = 0; i < 5; i++) {
        const float span = hi[i] - lo[i];
        float t = (span > 0.0f) ? (v[i] - lo[i]) / span : 0.0f;
        if (t < 0.0f) t = 0.0f;
        if (t > 1.0f) t = 1.0f;
        // A floor, so a collapsed axis is still a visible vertex rather than a
        // crease at the centre. CHARIOT's RANGE is 0.09 and its DAMAGE 0.07; at
        // literal zero the polygon would fold through the middle and read as a
        // drawing fault instead of as a weakness.
        out[i] = SHIP_AX_FLOOR + t * (1.0f - SHIP_AX_FLOOR);
    }
}

const char* vg_ship_axis_name(int i) {
    static const char* N[5] = { "SPEED", "HULL", "RANGE", "DAMAGE", "RATE" };
    return (i >= 0 && i < 5) ? N[i] : "";
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

static void chart_pt(const Lay* L, int i, float t, float* px, float* py) {
    *px = (float)L->cx + s_ax_cs[i][0] * (float)L->r * t;
    *py = (float)L->cy + s_ax_cs[i][1] * (float)L->r * t;
}

static void chart_ring(const Lay* L, float t, uint16_t col, int w) {
    float px, py, nx, ny;
    chart_pt(L, 4, t, &px, &py);
    for (int i = 0; i < 5; i++) {
        chart_pt(L, i, t, &nx, &ny);
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

static void draw_chart(const Lay* L, const float ax[5]) {
    chart_tables();

    // Graduations first, hairline, so they read as scale and not as structure.
    chart_ring(L, 0.33f, INK_TRACE, 1);
    chart_ring(L, 0.66f, INK_TRACE, 1);
    for (int i = 0; i < 5; i++) {
        float ex, ey;
        chart_pt(L, i, 1.0f, &ex, &ey);
        vg_line((float)L->cx, (float)L->cy, ex, ey, INK_TRACE);
    }
    // ...then the rim at 2px, which is the instrument border. The same weighting
    // rule the radar uses: outer edge structural, inner rings graduations.
    chart_ring(L, 1.0f, INK_FAINT, 2);

    // The class itself, over the top and at the top of the ramp.
    {
        float px, py, nx, ny;
        chart_pt(L, 4, ax[4], &px, &py);
        for (int i = 0; i < 5; i++) {
            chart_pt(L, i, ax[i], &nx, &ny);
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
        float lx = (float)L->cx + ox * (float)(L->r + SHIPVIEW_CHART_LABEL);
        float ly = (float)L->cy + oy * (float)(L->r + SHIPVIEW_CHART_LABEL);
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
static void draw_plan_view(const Lay* L, const ShipSpec* sp, int cls) {
    const VgShipPlan& pl = vg_ship_plan[(cls < SHIP_CLASSES) ? cls : 0];
    if (!pl.pts || pl.n < 2) return;
    (void)sp;

    // One scale for every class, from the roster's widest half-span. Fitting each
    // ship to the slot individually would make them all the same size, which is
    // the one thing the silhouette must not do.
    // BALLISTA's reverse wing, and it is the widest thing in the roster now. This
    // is the scale EVERY class is drawn against, so it has to track whichever hull
    // is broadest or that hull walks out of the slot.
    //
    // MEASURED NOW, NOT REMEMBERED. It was 0.58f written in by hand against a
    // roster whose widest hull is 0.575 -- near enough to be right, with nothing
    // anywhere that could tell when it stopped being. A fifth ship broader than
    // BALLISTA would have been drawn past the edge of its slot and over the panel
    // border, and the only report would have been that the new ship looked wrong.
    //
    // The drawing is 0.9% larger than it was, which is the rounding coming out.
    const float MAX_HALF_SPAN = plan_max_half_span();
    const float sy = ((float)L->mh * 0.46f) / MAX_HALF_SPAN;
    const float sx = sy;                            // uniform: no stretching
    const float cx = (float)(L->x + L->w / 2);
    const float cy = (float)(L->my + L->mh / 2);

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
// ...and that memory is VgShipView, which the CALLER owns -- it was four file
// statics here. See the note on the struct.

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
static void draw_plan_noise(const Lay* L, float p) {
    const float d = sinf(p * 3.14159265f);      // 0 at the ends, 1 in the middle
    if (d <= 0.02f) return;

    const int x0 = L->x + 8, w = L->w - 16;
    const int y0 = L->my,     h = L->mh;
    const uint32_t bucket = (uint32_t)(vg.state_t * SHIP_TEAR_RATE);

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

// ---------------------------------------------------------------------------
// WHAT THE PANEL SAYS ABOUT A CLASS, as a list.
//
// It was two calls written out inside the draw function. That is fine for two
// and it is the wrong shape for the question: "what do we show about a ship" is
// a list, so it should be one. A third row is a line here, where before it was a
// paragraph of arithmetic -- the rows stack against each other's heights, which
// nothing said out loud.
//
// THE VALUE IS WRITTEN INTO A BUFFER rather than returned, so that a row can be
// a NUMBER. Both rows today are strings off the weapon system and would have
// been happy returning a const char*; the first row that wants to say HULL 330
// would not, and changing the table's shape to admit one later is exactly the
// change a table is supposed to save.
//
// THE NAME IS NOT A ROW. It is the heading -- bigger, centred, tracked -- and a
// list of captioned fields is not what it is.
struct VgSpecField {
    const char* label;                                   // the inverse-video tab
    void      (*value)(const ShipSpec* s, char* out, int n);
    uint16_t    col;
};

static void fld_msl(const ShipSpec* s, char* out, int n) {
    snprintf(out, (size_t)n, "%s", vg_wpn_name(s->wpn));
}

static void fld_wpn(const ShipSpec* s, char* out, int n) {
    snprintf(out, (size_t)n, "%s", vg_wpn_desc(s->wpn));
}

// MSL is what it CARRIES, WPN is what the system DOES with it. MSL is not a new
// word either: the rack instrument in flight is labelled MSL, so it already
// means "the round" to anyone who has flown.
//
// THERE USED TO BE A TAGLINE ABOVE THESE and there is not any more. It was the
// one line on the panel still drawn at scale 1, which on this display is 0.57mm
// -- and rather than find it room at a readable size, it went. A tagline tells
// you how to feel about a hull before you have flown it; these two rows tell you
// what the hull actually does. Let the player decide which they want; the
// specification is the part that is any use in deciding.
static const VgSpecField SPEC_FIELDS[] = {
    { "MSL", fld_msl, INK_BRIGHT },
    { "WPN", fld_wpn, INK },
};

#define SPEC_FIELD_N ((int)(sizeof(SPEC_FIELDS) / sizeof(SPEC_FIELDS[0])))

// The heading, and then the rows under it. Each row is stacked on the BOTTOM of
// the one above -- field_wrap returns it -- because a value may take two lines
// and the caller cannot know which ones will.
static void draw_head(const Lay* L, const ShipSpec* sp) {
    vg_text_track(L->x
                      + (L->w - vg_text_track_width(sp->name, 3,
                                                    SHIPVIEW_TITLE_TRACK)) / 2,
                  L->y + SHIPVIEW_NAME_DY, sp->name, INK_MAX, 3,
                  SHIPVIEW_TITLE_TRACK);

    int fy = L->y + SHIPVIEW_HEAD_DY;
    for (int i = 0; i < SPEC_FIELD_N; i++) {
        char buf[40];
        SPEC_FIELDS[i].value(sp, buf, (int)sizeof(buf));
        fy = field_wrap(L->bx, fy, L->bw, SPEC_FIELDS[i].label, buf,
                        SPEC_FIELDS[i].col) + SHIPVIEW_FIELD_GAP;
    }
}

// ---------------------------------------------------------------------------

void vg_shipview_reset(VgShipView* v) {
    if (!v) return;
    v->to = v->from = -1;
    v->t0 = -1.0f;
    for (int i = 0; i < 5; i++) v->ax[i] = 0.0f;
}

void vg_shipview_draw(VgShipView* v, int cls, int x, int y, int w, int h) {
    if (!v) return;

    const Lay  box = lay_of(x, y, w, h);
    const Lay* L   = &box;
    const ShipSpec* sp = vg_spec((ShipClass)cls);

    // A NEW SELECTION. The chart is snapshotted AS SHOWN rather than as the class
    // it was heading for, so spinning the wheel fast leaves from wherever the
    // shape had actually reached instead of jumping back to the last full stop.
    float ax_now[5];
    vg_ship_axes(sp, ax_now);
    if (cls != v->to) {
        if (v->to >= 0) {
            const float q = (v->t0 < 0.0f) ? 1.0f
                          : (vg.state_t - v->t0) / SHIP_MORPH_TIME;
            const float e = (q >= 1.0f) ? 1.0f : (q <= 0.0f ? 0.0f : q * q * (3.0f - 2.0f * q));
            float ax_prev[5];
            vg_ship_axes(vg_spec((ShipClass)v->to), ax_prev);
            for (int i = 0; i < 5; i++)
                v->ax[i] = v->ax[i] + (ax_prev[i] - v->ax[i]) * e;
        } else {
            for (int i = 0; i < 5; i++) v->ax[i] = ax_now[i];
        }
        v->from = v->to;
        v->to   = (int8_t)cls;
        v->t0   = vg.state_t;
    }

    float p = (v->t0 < 0.0f) ? 1.0f : (vg.state_t - v->t0) / SHIP_MORPH_TIME;
    if (p < 0.0f) p = 0.0f;
    if (p > 1.0f) p = 1.0f;
    const float ease = p * p * (3.0f - 2.0f * p);        // smoothstep

    // ONLY THE HULL LAGS. The words follow the choice immediately, because the
    // wheel is what the thumb just moved and a panel that disagrees with it reads
    // as the screen not having noticed. The hull has to lag -- it cannot tween,
    // so it is swapped at the half way mark -- and that mismatch is invisible
    // because it happens under the thickest part of the noise.
    const int shown = (ease < 0.5f && v->from >= 0) ? v->from : cls;

    vg_rect(L->x, L->y, L->w, L->h, INK_TRACE);

    draw_head(L, sp);

    {
        float ax[5];
        for (int i = 0; i < 5; i++)
            ax[i] = v->ax[i] + (ax_now[i] - v->ax[i]) * ease;
        draw_chart(L, ax);
    }

    vg_fill_rect(L->x + 8, L->my - 8, L->w - 16, 1, INK_TRACE);
    draw_plan_view(L, vg_spec((ShipClass)shown), shown);
    draw_plan_noise(L, p);
}
