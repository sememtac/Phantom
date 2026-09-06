#include "vg_canopy_draw.h"
#include "vg_canopy_op.h"
#include "vg_raster.h"
#include "vg_raster_int.h"
#include "vg_port.h"
#include "vg_config.h"
#include "vg_sim.h"   // vg_frand01
#include <Arduino.h>
#include <string.h>
#include <math.h>
#include "vg_cockpit.h"
#include "vg_replay.h"   // vg_replay_mode: the split must not adapt under a replay

// ===========================================================================
// THE COCKPIT'S STATE, for the renderer that draws it
//
// This used to be a renderer. One drawing, one colour table, one warp, one
// coming-online sequence, and the row pass over all of it: the light delta, a
// cockpit applied as a CHANGE to the finished picture, lifted whole out of
// vg_band.cpp because the map called it a system trapped inside another one.
//
// The delta renderer is gone. Every hull's drawing became an opaque bake on
// 2026-09-05 -- see vg_canopy_op.h for what that is and why it won -- and once
// the last delta row in the generated set was nullptr, the band pass, its colour
// tables, its vector spans and its bench were code that could not execute. They
// were retired on 2026-09-06, about 1,300 lines of this file and two .S files.
//
// WHAT STAYS IS EVERYTHING THAT IS NOT ABOUT HOW THE DRAWING IS STORED. A cockpit
// moves in three ways -- it lags the turn on a spring, bends under the throttle,
// shears with the roll -- and it arrives a region at a time, takes hits, loses
// panels for good, and goes red at the wall. None of that is a property of the
// bake. It is a property of the MATCH, and there is one clock running all of it.
// The opaque renderer asks this module those questions (vg_canopy_motion,
// vg_canopy_gate_run, the zone accessors, vg_canopy_alarm_colour) and keeps its
// own inner loop, which is the boundary that was drawn when there were two
// cockpits and that turned out to be the right one when there was one again.
//
// The two-core split lives here too, because its balance point is built from the
// same per-column costs the warp needs.
// ===========================================================================

// HOW RED THE COCKPIT IS, 0 clear and 1 hard against the wall. The only wall warning there
// is now: the ring that used to sit beside this is deleted, above.
static float s_alarm  = 0.0f;

// The panel being looked through, worked out from the drawing when it is selected and
// never faulted. See canopy_find_centre.
static int8_t s_centre_z = -1;

// IS THE ARRIVAL SEQUENCE RUNNING. vg_canopy_intro_update walks the drawing's regions
// under this flag alone, so a true flag with no drawing selected is a fault --
// vg_canopy_intro_begin is what sets it and it checks first.
static bool s_intro_on = false;

// LOOKING AFT, so the cockpit must not be drawn.
//
// The canopy is the front of the ship. Draw it over a view out of the back and the picture
// says two things at once: the frame claims you are looking forward while everything inside
// it is behind you. From a playtest, and it is a continuity fault rather than a cosmetic one.
//
// It suppresses the FRAME ONLY, not the intro's world gate. Those share one primitive, and
// the gate is what holds the world black region by region while the cockpit comes online --
// so switching the whole primitive off in rear view would show the entire world at full
// brightness in the middle of the sequence.
static bool s_can_rear = false;

void vg_canopy_rear(bool on) { s_can_rear = on; }
// ...and asked for by the renderer, which is a different translation unit and has
// to make the same decision.
bool vg_canopy_rear_on(void)  { return s_can_rear; }
bool vg_canopy_intro_on(void) { return s_intro_on; }

// HOW MANY REGIONS THE COCKPIT THAT IS FLYING HAS. The arrival, the hits and the
// damage progression all step through the REGIONS of the drawing, and the count is
// the drawing's own -- the CHARIOT's bake has six.
static int canopy_zones(void) {
    const VgCanOp* o = vg_canopy_op_current();
    return o ? (int)o->zones : 0;
}

// IS THERE A COCKPIT AT ALL. Every entry point tolerates not having one: a hull with
// no drawing flies with no frame, and the boot chain still has to run for it.
static bool canopy_any(void) {
    return vg_canopy_op_current() != nullptr;
}

// THE WALL WARNING, ON THE COCKPIT INSTEAD OF ON THE VIEW.
//
// The ring tint is a per-pixel pass over its own area and costs about 1100 us at the wall,
// measured. This costs NOTHING per pixel: the cockpit's members are already written every
// frame, and their colour comes from a 256-entry table, so turning the frame red is a
// different table rather than more work.
//
// Quantised to sixteen steps so a steady approach changes the colour a few times instead
// of sixty a second. The renderer rebuilds its palette when the level moves, and this is
// what keeps that from happening every frame.
// s_alarm is declared further up, where the ring's own level used to sit beside it.
static int   s_alarm_q = 0;
static bool  s_alarm_white = false;

// TWO THINGS, because they are two different signals and the first attempt conflated them.
//
// `k` is how red the frame is, from the clearance. `white` is a STROBE: the frame goes to
// white for a fraction of a second and comes back. The author's words, after flying the
// first version and not seeing it: "the flashing i have in mind is not the canopy flashing
// its opacity, rather, it's the color flashing from split second of white to the current
// tint".
//
// That is why the first attempt was invisible. It swung the red LEVEL between a floor and
// full, and vg_mix(COL_HUD, COL_DANGER, k) cannot reach white at either end -- so the whole
// effect was a slight change of saturation on an already-red frame, which is precisely the
// "which shade means I am dead" problem it was meant to solve.
//
// The strobe is a bool and the level is quantised, so the colour moves on either edge of
// a flash and on a step of the ramp. At the top rate that is about eighteen palette
// rebuilds a second in the renderer, which is nothing beside a per-pixel pass.
void vg_canopy_alarm(float k, bool white) {
    if (k < 0.0f) k = 0.0f; else if (k > 1.0f) k = 1.0f;
    const int q = (int)(k * 16.0f + 0.5f);
    if (q == s_alarm_q && white == s_alarm_white) return;
    s_alarm_q     = q;
    s_alarm       = (float)q * (1.0f / 16.0f);
    s_alarm_white = white;
}

uint16_t vg_canopy_alarm_colour(float* level) {
    if (s_alarm_white) {
        if (level) *level = 1.0f;
        return CANOPY_ALARM_WHITE;
    }
    if (level) *level = s_alarm;
    return (s_alarm > 0.0f) ? vg_mix(COL_HUD, COL_DANGER, s_alarm) : COL_HUD;
}

// ===========================================================================
// THE SPHERE, the same one the instruments are drawn on.
//
// A radial warp is NOT separable: k = 1 + K*scale*r2 scales both axes by an amount that
// depends on both coordinates, so a y-map plus a column offset -- which is what this was --
// can only ever produce a shear and a zoom. It read as the frame growing and bending rather
// than bulging.
//
// Done properly in two halves, because the renderer iterates panel ROWS and can only lay runs
// along panel x within one:
//
//   the y axis is EXACT. dx is fixed for the whole column, so dx*dx hoists and each block
//   endpoint costs a subtract, two multiplies, two adds and a round -- about seven operations.
//   Both endpoints go through it and the length is the difference, so runs still touch.
//
//   the x axis is an inverse. For each output row, which drawing column lands here -- built by
//   walking the forward map, which is monotone for any |K| the panel uses. Evaluated at the
//   screen's own centre line, so a point's sideways displacement is taken as constant down its
//   column. That is the one approximation, and it is the same one the rear-view patch makes.
//
// Rebuilt only when the quantised amount changes, so the 480 forward evaluations and the
// inversion are paid a handful of times across a throttle sweep, not per frame.
static float   s_w_zk   = 0.0f;    // zoom * K * scale / R2
static float   s_w_zbase[SCR_H];   // zoom + zoom*K*scale*dx^2/R2, per SOURCE column
static int16_t s_wc[SCR_H];        // the bow's per-column shift, laid on top of the sphere
// WHICH DRAWING COLUMN A PANEL ROW SAMPLES, which is the other half of coming closer.
//
// Stretching the drawing's y alone made the frame taller, not nearer. A magnification needs
// both axes, and the other axis IS the column index -- so it is a lookup per panel row, 480 a
// frame, and free. Inverse-mapped because this samples rather than places: a frame magnified
// by s puts drawing column d at panel position cx + (d-cx)*s, so the panel row at p wants
// cx + (p-cx)/s.
//
// Nearest neighbour, so at a magnification of a tenth roughly every tenth column is read
// twice. On a cockpit frame that reads as the members thickening slightly, which is what
// something approaching does anyway. Every entry is a real column: the inversion
// clamps to the nearest edge, so a row off the drawing repeats the edge column and
// no sentinel is ever stored. (The -1 at the balance pass below is that call
// site's own range test on the row, not a value read from here.)
static int16_t s_wcol[SCR_H];

// WHERE A BAND BALANCES, for the two-core split.
//
// Built from where the drawing's work falls, and rebuilt with the warp maps: the inverse
// column map duplicates some columns and skips others, so the work slides along the band
// and a balance point taken at rest stops being the middle of it. A band costs whichever
// half is slower, so the error is paid in full and it grows with ZOOM -- which is the
// setting the look depends on. s_colcost is walked out of the bake once per drawing.
static uint8_t  s_wsplit[NUM_BANDS];
static uint16_t s_colcost[SCR_H];
static bool     s_colcost_ready = false;

// HOW MUCH WORK EACH PANEL ROW CARRIES, from the opaque bake's spans. Indexed by
// DRAWING COLUMN, which is what warp_build walks -- and a drawing column is a panel
// row read backwards.
static void canopy_colcost(void) {
    const VgCanOp* o = vg_canopy_op_current();
    for (int c = 0; c < SCR_H; c++) {
        uint32_t cost = 0;
        if (o) {
            const int py = SCR_H - 1 - c;
            for (uint16_t si = o->row[py]; si < o->row[py + 1]; si++)
                cost += (uint32_t)o->span[si].len + 1u;   // a span header is worth about a pixel
        }
        s_colcost[c] = (uint16_t)(cost > 0xFFFFu ? 0xFFFFu : cost);
    }
    s_colcost_ready = true;
}
static int     s_wq = -1;                 // the quantised amount the maps were built for
static int     s_wq_want = -1;            // ...and the one the last frame asked for
static bool    s_warp_on = false;

// THE AMOUNT, HELD BY HAND. Negative is the game's own throttle; 0..1 overrides it on
// every frame until it is released. This is an instrument and nothing else. The bend
// is driven by (1 - throttle), so a replayed fight bends the frame exactly as much as
// the pilot happened to fly it -- and the only way to ask what the STRETCH itself
// costs is to fly the same frames once flat and once at full bend. Set from the
// serial link ('f', 'F', 'n' in vg_capture.cpp) and by nothing in the game; it also
// overrides the canopy bench's own 0 and 1, so release it before reading that.
static float   s_warp_pin = -1.0f;
void vg_canopy_warp_pin(float k) { s_warp_pin = k; }
// ...AND THE LAG, HELD OFF, which with the bend pinned at zero is a RIGID frame:
// the cockpit as it first flew, before it moved at all, and the reference every
// cost of moving it is measured against.
static bool    s_lag_pin_off = false;
void vg_canopy_lag_pin_off(bool off) { s_lag_pin_off = off; }

// A DIFFERENT DRAWING IS A DIFFERENT SET OF COLUMN COSTS, and a different centre
// region, so both are worked out again for it. Down here rather than beside the
// drawing because two of the caches it drops are the warp's, declared just above.
//
// It matters because the hulls do not share a drawing: flying a LANCE after a
// CHARIOT would otherwise balance the LANCE's bands against the CHARIOT's spans.
void vg_canopy_op_changed(void) {
    s_centre_z      = -1;
    s_colcost_ready = false;
    s_wq            = -1;
    s_wq_want       = -1;
}

// THE FRAME LAGS THE SHIP, which is what makes it read as being inside something.
//
// A pixel offset from the DIFFERENCE between the turn now and a smoothed copy of it, not from
// the turn itself. Displacement then appears while the turn is starting and stopping and
// settles back to nothing through a steady one -- inertia rather than a permanent lean, which
// would just look like the drawing is off centre.
//
// Whole pixels, so it quantises itself like everything else here, and it moves the COLUMN the
// row samples: under the quarter turn a bank is movement along logical x, and logical x is the
// drawing's column. One index add per row.
// ALL THREE AXES, and each is a different shape of offset.
//
// Under the quarter turn the drawing's x is a panel COLUMN and its y is a panel row's x, so:
//
//   yaw   moves the frame along logical x  -> the column each row samples. One index add.
//   pitch moves it along logical y         -> every block's start. One add, per column.
//   roll  is a rotation, which no single offset can be -- but a small rotation about the
//         centre is a SHEAR, and a shear is a per-column offset that grows with the column.
//         So it is the same add as pitch with a term in c. One multiply per column, none per
//         block and none per pixel.
//
// Each rides the difference between the command and a smoothed copy of it, so the frame swings
// as a movement starts and stops and sits still through a held one. A canopy that stayed
// displaced through a sustained roll would be a canopy coming loose from the ship.
// A MASS ON A SPRING, not a difference.
//
// It used to be (command - smoothed) * gain. That eases on the way IN and cannot ease on the
// way out: lifting a finger drops the command to zero in one frame, the smoothed copy has not
// moved, so the difference -- and the frame with it -- jumps to its extreme instantly. Reported
// as the canopy snapping back and the ship losing its weight, which is exactly what a
// first-order difference does. The offset was a function of the instantaneous command, so any
// step in the command was a step in the picture.
//
// Now the displacement is STATE. The command's rate of change drives it, a spring pulls it
// home and damping settles it, so nothing the player does moves the frame in one frame -- only
// its acceleration changes. Releasing the stick gives an equal and opposite kick and the frame
// coasts back at the spring's own pace, overshooting slightly, which is what weight looks like.
//
// SPRING sets how long the return takes -- 0.030 is a period of about 36 frames, six tenths of
// a second. DAMP sets how much it overshoots: this pair is a damping ratio near 0.64, so it
// comes back with one soft rebound rather than ringing or arriving dead.
static float s_lag_q[3]  = { 0.0f, 0.0f, 0.0f };   // last command, per axis
static float s_lag_v[3]  = { 0.0f, 0.0f, 0.0f };   // velocity
static float s_lag_x[3]  = { 0.0f, 0.0f, 0.0f };   // displacement
static int   s_lag_px = 0;         // columns, from yaw
static int   s_lag_py = 0;         // pixels along the drawing's y, from pitch
static float s_lag_sh = 0.0f;      // pixels of y per column, from roll

void vg_canopy_lag(float yaw, float pitch, float roll, float scale) {
    const float cmd[3] = { yaw, pitch, roll };
    float out[3];
    for (int i = 0; i < 3; i++) {
        const float a = cmd[i] - s_lag_q[i];       // the ship's angular acceleration, near enough
        s_lag_q[i] = cmd[i];
        s_lag_v[i] += a * CANOPY_LAG_DRIVE
                    - s_lag_x[i] * CANOPY_LAG_SPRING
                    - s_lag_v[i] * CANOPY_LAG_DAMP;
        s_lag_x[i] += s_lag_v[i];
        out[i] = s_lag_x[i];
    }

    // The CLAMP scales with the airframe too. Scaling only the swing would have every hull
    // reach the same limit and arrive there at a different speed, which flattens exactly the
    // difference this is for -- a CHARIOT would clip where a BALLISTA never gets close.
    const float lim  = CANOPY_LAG_MAX * scale;
    const float gain = CANOPY_LAG_PX * scale;

    float dx = out[0] * gain;
    float dy = out[1] * gain;
    float dr = out[2] * gain * CANOPY_LAG_ROLL;
    if (dx >  lim) dx =  lim; else if (dx < -lim) dx = -lim;
    if (dy >  lim) dy =  lim; else if (dy < -lim) dy = -lim;
    if (dr >  lim) dr =  lim; else if (dr < -lim) dr = -lim;

    s_lag_px = (int)(dx + (dx < 0.0f ? -0.5f : 0.5f));
    s_lag_py = (int)(dy + (dy < 0.0f ? -0.5f : 0.5f));
    // Pixels of y at the frame's edge, spread linearly across the columns.
    s_lag_sh = dr / (float)(SCR_H / 2);
    if (s_lag_pin_off) { s_lag_px = 0; s_lag_py = 0; s_lag_sh = 0.0f; }
    if (s_lag_px || s_lag_py || s_lag_sh != 0.0f) s_warp_on = true;
}

// The sequence's own clock. Declared here because vg_canopy_warp is where the flex it
// suppresses is set, and defined below with the rest of the intro.
static float s_settle_t = -1.0f;          // < 0 means not settling

// THE AMOUNT IS CHEAP; THE TABLE IS NOT. This used to do both, inside group B of the
// submit split, and the rebuild is 480 forward evaluations, an inversion and a
// fifteen-band rebalance -- 1,175 us at worst, on whichever frame the quantised
// throttle crosses a step. A dogfight crosses them constantly.
//
// Submit runs BEFORE the transfer starts, so every microsecond there is frame time,
// and this one was landing inside the half that core 1 already waits on: the
// rendezvous gap is 214 us on the average fight frame and this was a fifth of its
// worst. So the amount is still recorded here, where the throttle is known, and the
// BUILD moves to the end of the frame -- see vg_canopy_warp_build.
void vg_canopy_warp(float k) {
    if (!canopy_any()) return;             // no cockpit: nothing to bend
    if (s_warp_pin >= 0.0f) k = s_warp_pin;
    if (k < 0.0f) k = 0.0f;
    if (k > 1.0f) k = 1.0f;
    const int q = (int)(k * CANOPY_WARP_STEPS + 0.5f);
    s_warp_on = (q != 0) || s_lag_px || s_lag_py || (s_lag_sh != 0.0f);
    s_wq_want = q;
    // A DRAWING WITH NO TABLE CANNOT WAIT. s_wq is -1 on the first frame and after
    // vg_canopy_op_changed invalidates it, and the band pass is about to read the maps -- so
    // that one case still builds here, where it always did.
    if (s_wq < 0) vg_canopy_warp_build();
}

// Deferred from vg_canopy_warp, and called after the band pass has finished DRAWING --
// the last bands are still going out over the wire, so this overlaps the transfer
// instead of standing in front of it. The maps it leaves are the ones the NEXT frame
// draws with, which is one frame of latency on a step crossing. The bend is quantised
// and already lags the ship through a spring and a damper, so a frame is not visible;
// what it is NOT is free of the pixel record, and the regression baseline moves with it.
void vg_canopy_warp_build(void) {
    // THIS GUARD ASKED FOR THE DELTA DRAWING until every hull went opaque.
    // Left alone it returned before building anything, so s_w_zbase and s_wcol stayed
    // zero -- and a zero sphere maps every column of the frame onto the centre line,
    // which collapses every span to no width. The cockpit did not draw at all.
    if (!canopy_any() || s_wq_want == s_wq) return;
    const int q = s_wq_want;
    s_wq = q;

    const float a    = (float)q / (float)CANOPY_WARP_STEPS;
    const float zoom = 1.0f + CANOPY_WARP_ZOOM * a;
    const float R2   = (float)SCR_CX * SCR_CX + (float)SCR_CY * SCR_CY;
    const float krn  = HUD_WARP_K * CANOPY_WARP_SPHERE * a / R2;
    s_w_zk   = zoom * krn;

    // The forward map along x at the centre line, then inverted. Monotone, so one walk.
    static int16_t fwd[SCR_H];
    for (int x = 0; x < SCR_H; x++) {
        const float dx = (float)x - (float)SCR_CX;
        fwd[x] = (int16_t)(int)((float)SCR_CX + dx * zoom * (1.0f + krn * dx * dx) + 0.5f);
        // ...and what a block endpoint in this column will need, less its dy terms.
        s_w_zbase[x] = zoom + s_w_zk * dx * dx;
    }
    {
        int x = 0;
        for (int xp = 0; xp < SCR_H; xp++) {
            while (x < SCR_H - 1 && fwd[x] < xp) x++;
            int best = x;
            if (x > 0 && (xp - fwd[x - 1]) < (fwd[x] - xp)) best = x - 1;
            s_wcol[xp] = (int16_t)best;
        }
    }
    // And where each band now balances, from the columns the rows actually read.
    if (!s_colcost_ready) canopy_colcost();
    for (int b = 0; b < NUM_BANDS; b++) {
        uint32_t rowc[BAND_H], total = 0;
        for (int r = 0; r < BAND_H; r++) {
            const int lx = SCR_H - 1 - (b * BAND_H + r);
            const int sc = (lx >= 0 && lx < SCR_H) ? s_wcol[lx] : -1;
            rowc[r] = (sc >= 0) ? s_colcost[sc] : 0u;
            total  += rowc[r];
        }
        int at = ROW_SPLIT;
        if (total) {
            uint32_t run = 0, best = 0xFFFFFFFFu;
            for (int r = 1; r < BAND_H; r++) {
                run += rowc[r - 1];
                const uint32_t gap = (run > total - run) ? run - (total - run)
                                                        : (total - run) - run;
                if (gap < best) { best = gap; at = r; }
            }
        }
        s_wsplit[b] = (uint8_t)at;
    }

    // The bow, on top of the sphere, for an author who wants more bend than the panel has.
    const float cx = (float)(SCR_H - 1) * 0.5f;
    for (int c = 0; c < SCR_H; c++) {
        const float t = ((float)c - cx) / cx;          // -1 .. 1
        s_wc[c] = (int16_t)(int)(CANOPY_WARP_BOW * a * t * t * (t < 0.0f ? -1.0f : 1.0f));
    }
}

// WARP OR NO WARP, settled at compile time.
//
// Asked per block, the flag cost 83 us of a 4042 us pass for a question whose answer is the
// same for the whole frame -- the same shape of waste the sky fill's tint flag was. Two
// instantiations, and the rigid one carries no warp code at all.
// One endpoint onto the sphere. `zbase` carries the column's dx term; only dy is left.
static inline int warp_y(int y, float zbase) {
    const float dy = (float)y - (float)SCR_CY;
    return (int)((float)SCR_CY + dy * (zbase + s_w_zk * dy * dy) + 0.5f);
}

// The three motions, answered for the renderer. See VgCanMotion in the header for
// why they live here rather than there.
bool vg_canopy_motion(int py, struct VgCanMotion* m) {
    if (!s_warp_on) return false;

    // Panel row to drawing column and back. The lag picks a DIFFERENT COLUMN, which
    // is the whole of the yaw swing. CLAMPED TO THE EDGE, NOT DROPPED: a row that
    // lands off the drawing repeats the edge column, which is what a texture sampler
    // does at its border and reads as the frame continuing past the screen. Dropping
    // it left the screen edge empty on the side the frame moved away from, so the
    // cockpit appeared to slide and tear rather than to move.
    int lx = SCR_H - 1 - py;
    lx += s_lag_px;
    if (lx < 0) lx = 0; else if (lx >= SCR_H) lx = SCR_H - 1;
    lx = s_wcol[lx];

    // FOLDED ABOUT THE CENTRE LINE, and that fold is what makes a bow a bow.
    //
    // s_wc is ANTISYMMETRIC in the column -- t*t*sign(t) -- so read straight through
    // it shifts the drawing's left half one way along its columns and the right half
    // the other. That is a TWIST, not a bend: one diagonal pair of corners curves and
    // the other pair curves against it, which is exactly what it looked like flown.
    //
    // The light delta this replaced never had the fault, and not because it guarded
    // against it: the CHARIOT's delta drawing was MIRRORED, so its right half read a
    // folded column index and picked up the negated bow for free. An opaque bake stores
    // every column -- a full-colour cockpit is allowed to be lopsided -- so the fold has
    // to be asked for rather than falling out of the storage.
    //
    // The roll shear is folded with it, which makes it symmetric about the centre too.
    // That is not obviously what a roll should do, but it is what the delta cockpit
    // always did and what has been flown since; the shear is a separate question for a
    // separate day.
    const int cc = (lx >= SCR_H / 2) ? (SCR_H - 1 - lx) : lx;

    m->row   = SCR_H - 1 - lx;
    m->xofs  = (int)s_wc[cc] + s_lag_py
             + (int)((float)(cc - SCR_H / 2) * s_lag_sh);
    m->zbase = s_w_zbase[cc];
    m->zk    = s_w_zk;
    return true;
}

// ===========================================================================
// THE CANOPY COMING ONLINE
//
// The match opens black with the instruments already lit, and the view arrives a region at a
// time. Per region, keyed off the ZONE the artist painted into the green channel:
//
//   the whole region flashes WHITE -- every pixel of it, not just the frame's members,
//   the world then dissolves in out of that white,
//   and the frame's blocks in that zone begin drawing at their authored level.
//
// THE GREEN SHAPE IS THE FLASH. That is the author's own word for it -- "the shapes act as
// mask" -- and the first version got it wrong: it flashed the frame's BLOCKS, which is the
// tenth of the region the cockpit actually covers, so a region lighting up read as a few
// members brightening instead of a piece of the view coming on.
//
// It also produced the overblown colour the author saw. Flashing the blocks meant lerping the
// panel's orange toward white in a per-zone table, which passes through a hot salmon at half
// flash -- and that was being ADDED to an already-bright view, so the channels clipped at
// different points and the hue swung. There is no per-zone colour table any more. The region
// flash is a flat fill and the frame draws its authored level throughout, which cannot clip.
//
// The frame's pixels still have to be withheld until their zone comes up: the gate runs
// BEFORE them in the row, so metal drawn early would sit lit on the black. The renderer
// asks vg_canopy_zone_live for that, region by region.
//
// The frame is held RIGID throughout. The gate and the frame have to agree pixel for pixel or
// the black would not end where the panel does, and the cheapest way to guarantee that is to
// have both read the same unwarped column. CANOPY_INTRO_SETTLE then ramps the flex in at the
// end, because the resting warp is a long way from flat and the cockpit would otherwise jump
// the moment the sequence released.
// THE MEMBERS LIGHT UP TOO, and this part started as a bug the author liked.
//
// The first version flashed the frame's BLOCKS instead of the region, by lerping the panel's
// orange toward white in a per-zone table. That was the wrong AREA -- and it also blew the hue
// out, because a near-white delta added onto an already-bright view clips the three channels at
// different points and the colour swings on the way. The author saw both, and asked for the
// second one back on top of the corrected first one.
//
// So it is deliberate now, and it has somewhere to live: not the white hold, where a member
// drawn additively onto white is invisible, but the DISSOLVE. As world cells come through, the
// members over them are hot and cooling, so the frame lights up as its region resolves and
// settles to its authored level afterwards.
//
// The saturation IS the effect. Nothing here reaches for a hue sweep, because the clip does it
// for free: red is five bits and saturates first, green has six and holds longer, so a rising
// white delta over a lit world passes through magenta and amber before it whites out. Note that
// CANOPY_INTRO_LIT_PEAK below is what governs how much of that is seen -- at 1.0 the members
// spend their first moments fully white, which is the part with no colour in it at all.
// s_intro_on is declared at the top of the file -- see the note there.
static float   s_intro_t    = 0.0f;
static uint8_t s_izon[VG_CANOPY_MAX_ZONES];   // 0 held, 255 fully dissolved to the world
static uint8_t s_ilive[VG_CANOPY_MAX_ZONES];  // whether this zone's blocks are drawn at all
static uint16_t s_ifill[VG_CANOPY_MAX_ZONES]; // a held pixel: black before the flash, white after
static uint8_t s_iglow[VG_CANOPY_MAX_ZONES];  // 255 white-hot members, 0 their authored level
static bool    s_icued     = false;       // the instruments' cue, latched -- see vg_canopy_intro_cued
// A DITHER, NOT A FADE, and it is both cheaper and a better fit.
//
// Holding a region is a store per pixel. Cross-fading one would be three multiplies per pixel,
// about 15 cycles against a handful, and across a 77,000-pixel zone that is milliseconds for a
// transition that lasts a third of a second.
//
// So the reveal is an ordered 4x4 threshold: a pixel gives up its hold once its cell's value is
// under the zone's reveal. Pixels come through in a fixed scatter instead of the whole region
// changing together, which reads as a picture being acquired rather than a lamp being turned
// down -- and it is the same language the backdrop's own reveal already speaks.
static const uint8_t BAYER4[16] = {
     0,  8,  2, 10,
    12,  4, 14,  6,
     3, 11,  1,  9,
    15,  7, 13,  5,
};

// One panel row of the gate. `lx` is the true screen column, never a warped one.
//
// Runs cover the whole column with no gap, so this walks them all and does nothing for a zone
// that is already fully in. Three bytes a run and about three runs a column, so the walk itself
// is free and the cost is entirely in the pixels it holds.
//
// A held pixel takes the zone's fill -- black before its flash, white during it -- so the same
// loop serves both. That is the whole reason the flash is affordable across a region rather than
// only across the frame: a flat fill is the cheapest thing this file does.
// ---------------------------------------------------------------------------
// A PANEL TAKING A HIT
//
// Per panel, one float. The gate below already fills a region's screen pixels with a
// colour, dithered as it dissolves -- so the flash costs nothing new, and the static is
// the same walk with a different value per pixel.
// ---------------------------------------------------------------------------

static float    s_hit_t[VG_CANOPY_MAX_ZONES];   // seconds left on this panel
static bool     s_gate_on = false;              // the intro is running, or a panel is hit
static uint32_t s_hit_seed = 0x9E3779B9u;       // advanced per frame, so static crawls

// WHITE FOR THE FIRST PART, STATIC FOR THE REST, as a fraction of the hit's life. The
// flash says WHERE and the static says the panel is still hurt -- one is a moment and the
// other is a condition, and a flash that lasted the whole time would read as a lamp.
#define CANOPY_HIT_TIME   1.60f
#define CANOPY_HIT_FLASH  0.82f

static inline uint32_t hit_hash(uint32_t x, uint32_t y, uint32_t s) {
    uint32_t h = x * 0x9E3779B9u ^ y * 0x85EBCA6Bu ^ s;
    h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12;
    return h;
}

// ---------------------------------------------------------------------------
// FAULTY PANELS
// ---------------------------------------------------------------------------

// How many panels are failing, worst first. A panel that has failed stays failed for the
// match: the hull does not heal, so neither does the cockpit.
static uint8_t s_fault_n   = 0;
static uint8_t s_fault_z[VG_CANOPY_MAX_ZONES];

// WHICH PANEL IS THE MIDDLE ONE, once per drawing.
//
// The baker reads the region at the middle of the panel and stores it, so the artist
// paints regions and never has to say which is the windscreen -- and a drawing whose
// panels move gets the right answer without anyone remembering to update a constant.
//
// This was a centroid search over a zone map, which asks which region is nearest the
// middle ON AVERAGE, and a big L-shaped region can average its way to the centre
// without covering it. The stored answer is the better one.
static void canopy_find_centre(void) {
    const VgCanOp* o = vg_canopy_op_current();
    s_centre_z = o ? (int8_t)o->centre : (int8_t)-1;
}

// HOW BROKEN THE HULL HAS TO BE before a panel goes, and how many go by the end.
//
// Nothing until the hull is genuinely in trouble: a cockpit that starts failing at the
// first scratch spends the whole match crying wolf, and the player stops reading it.
#define CANOPY_FAULT_START  0.55f    // hull fraction at which the first panel goes
#define CANOPY_FAULT_END    0.12f    // ...and at which as many as can be are gone

void vg_canopy_damage(float hull_frac) {
    // Whichever cockpit is flying -- the regions it fails are its own.
    if (!canopy_any() || canopy_zones() <= 0) return;
    if (s_centre_z < 0) canopy_find_centre();

    // Everything but the middle one may fail.
    const int usable = (canopy_zones() > 1) ? canopy_zones() - 1 : 0;
    if (!usable) return;

    float t = (CANOPY_FAULT_START - hull_frac)
            / (CANOPY_FAULT_START - CANOPY_FAULT_END);
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    // ROUNDED UP, so the first panel fails AT the threshold rather than a third of the way
    // past it. With four zones and one reserved, (int)(t * 3) needs t >= 1/3 before it
    // reaches one -- which put the first failure at 41% hull while the constant said 55%.
    int want = (int)(t * (float)usable + 0.9999f);
    if (t <= 0.0f) want = 0;

    // MONOTONIC. Panels are added and never removed, because repairs happen between
    // matches and the hull fraction wobbles upward inside one for no reason the player
    // would credit.
    while (s_fault_n < want && s_fault_n < usable) {
        int free_z[VG_CANOPY_MAX_ZONES];
        int n = 0;
        for (int z = 0; z < canopy_zones(); z++) {
            if (z == s_centre_z) continue;
            bool taken = false;
            for (int i = 0; i < s_fault_n; i++) if (s_fault_z[i] == z) taken = true;
            if (!taken) free_z[n++] = z;
        }
        if (!n) break;
        s_fault_z[s_fault_n++] = (uint8_t)free_z[(int)(vg_frand01() * (float)n) % n];
        s_gate_on = true;
    }
}

void vg_canopy_hit_clear(void) {
    s_fault_n  = 0;
    s_centre_z = -1;
    for (int i = 0; i < VG_CANOPY_MAX_ZONES; i++) s_hit_t[i] = 0.0f;
    s_gate_on = s_intro_on;
}

void vg_canopy_hit(void) {
    // WHICHEVER COCKPIT IS FLYING, and its own region count -- see canopy_zones.
    const int nz = canopy_zones();
    if (nz <= 0) return;
    // NOT ALREADY MARKED. Refreshing a panel that is already out would waste the hit and
    // read as nothing happening; spreading it is what makes a second hit cost more view
    // than the first.
    int free_z[VG_CANOPY_MAX_ZONES];
    int n = 0;
    for (int z = 0; z < nz; z++)
        if (s_hit_t[z] <= 0.0f) free_z[n++] = z;
    if (!n) return;
    const int z = free_z[(int)(vg_frand01() * (float)n) % n];
    s_hit_t[z] = CANOPY_HIT_TIME;
    s_gate_on  = true;
}

void vg_canopy_hit_step(float dt) {
    bool any = false;
    for (int z = 0; z < VG_CANOPY_MAX_ZONES; z++) {
        if (s_hit_t[z] > 0.0f) {
            s_hit_t[z] -= dt;
            if (s_hit_t[z] < 0.0f) s_hit_t[z] = 0.0f;
            else                   any = true;
        }
    }
    // Crawls rather than holding still: a static field that does not move is a texture.
    s_hit_seed = s_hit_seed * 1664525u + 1013904223u;
    // FAULTS COUNT TOO, and leaving them out was a bug that made the whole progression
    // invisible: vg_canopy_damage set this true when a panel failed, this line cleared it
    // on the very next frame, and the while loop in there never ran again because the fault
    // was already recorded. A faulty panel drew for one frame and then never again.
    s_gate_on  = any || s_intro_on || (s_fault_n > 0);
}

const uint8_t* vg_canopy_bayer_row(int y) { return &BAYER4[(y & 3) << 2]; }

uint8_t  vg_canopy_zone_reveal(int z) {
    return (z >= 0 && z < VG_CANOPY_MAX_ZONES) ? s_izon[z] : 255;
}
uint16_t vg_canopy_zone_fill(int z) {
    return (z >= 0 && z < VG_CANOPY_MAX_ZONES) ? s_ifill[z] : 0;
}

bool vg_canopy_gate_on(void)      { return s_gate_on; }
bool vg_canopy_zone_live(int z) {
    if (!s_intro_on) return true;              // nothing is holding anything back
    return (z >= 0 && z < VG_CANOPY_MAX_ZONES) && s_ilive[z] != 0;
}
uint8_t vg_canopy_zone_glow(int z) {
    if (!s_intro_on || z < 0 || z >= VG_CANOPY_MAX_ZONES) return 0;
    return s_iglow[z];
}

// ONE RUN OF ONE REGION, painted with whatever that region is doing: held black while it
// waits its turn, white on its flash, dissolving to the world, or lost to a hit or a
// fault. Lifted out of canopy_gate whole so that a cockpit stored some other way can be
// walked through the same arithmetic -- see the block at VgCanOpZone. Nothing here reads
// the drawing; it reads the CLOCK, and the clock is one.
void IRAM_ATTR vg_canopy_gate_run(uint16_t* row, int lx, int py, int y0, int n, int z) {
    const uint8_t* bay = &BAYER4[(py & 3) << 2];
    {

        // NOT WHILE THE COCKPIT IS ARRIVING.
        //
        // The intro owns every region until it finishes. A panel cannot crack before it
        // exists, and both branches below run AHEAD of the reveal -- so a fault left over
        // from the last match, or a round landing during the sequence, painted static over
        // a region the intro had not lit yet and went on flashing behind it.
        //
        // The reveal is the only thing that draws while s_intro_on. Damage resumes the
        // frame it ends.
        if (!s_intro_on) {
            // A FAULTY PANEL, which is a condition rather than an event: mostly it works,
            // and then it does not. The flicker is on the panel's OWN phase so two faulty
            // panels do not blink together, which would read as the whole cockpit strobing.
            bool faulty = false;
            for (int i = 0; i < s_fault_n; i++) if (s_fault_z[i] == z) faulty = true;
            if (faulty) {
                const uint32_t ph = hit_hash((uint32_t)z, 0u, s_hit_seed & ~0xFFu);
                // Out for a short slice of each cycle, and occasionally for a long one.
                const uint32_t beat = (s_hit_seed >> 3) + ph;
                const bool out = ((beat & 0x3Fu) < 7u) || ((beat & 0x3FFu) < 40u);
                if (out) {
                    uint16_t* qf = &row[y0];
                    for (int i = 0; i < n; i++) {
                        const uint32_t r = hit_hash((uint32_t)lx, (uint32_t)(y0 + i), s_hit_seed);
                        if ((r & 0xFFu) < 190u)
                            qf[i] = (r & 0x100u) ? 0xFFFF : 0x0000;
                    }
                    return;
                }
            }

            // A HIT PANEL IS DRAWN INSTEAD OF THE REGION'S NORMAL STATE, and takes the
            // whole run: white while the flash lasts, then static that thins as it heals.
            // The world behind it is simply gone for that long, which is the point -- the
            // player has lost that piece of the view.
            const float ht = s_hit_t[z];
            if (ht > 0.0f) {
                uint16_t* qh = &row[y0];
                if (ht > CANOPY_HIT_TIME * CANOPY_HIT_FLASH) {
                    for (int i = 0; i < n; i++) qh[i] = 0xFFFF;
                } else {
                    // Thinning: as the hit heals, fewer pixels are taken, so the view comes
                    // back through the static rather than switching back on.
                    const uint32_t keep = (uint32_t)(255.0f * (ht / (CANOPY_HIT_TIME * CANOPY_HIT_FLASH)));
                    for (int i = 0; i < n; i++) {
                        const uint32_t r = hit_hash((uint32_t)lx, (uint32_t)(y0 + i), s_hit_seed);
                        if ((r & 0xFFu) < keep)
                            qh[i] = (r & 0x100u) ? 0xFFFF : 0x0000;
                    }
                }
                return;
            }
        }

        const uint32_t rev  = s_izon[z];
        if (rev >= 255u) return;                  // this region is all the way in
        const uint16_t fill = s_ifill[z];
        uint16_t* q = &row[y0];
        if (rev == 0u) {
            for (int i = 0; i < n; i++) q[i] = fill;  // held: black, or the flash
        } else {
            // 0..16, not 0..15. `rev >> 4` tops out at 15, which leaves one cell of the
            // sixteen still held at the very end of the dissolve -- a sixteenth of the region
            // popping through in one frame. *17>>8 reaches 16, which is past every cell.
            const uint32_t th = (rev * 17u) >> 8;
            for (int i = 0; i < n; i++)
                if ((uint32_t)bay[(y0 + i) & 3] >= th) q[i] = fill;
        }
    }
}

// WHERE THE SEQUENCE IS UP TO.
//
// The zones come up in the order the artist painted, which is already the zone INDEX: the baker
// sorts the green values it found and stores each block's position in that order, so zone 0 is
// simply first and this needs no table of its own.
void vg_canopy_intro_begin(void) {
    // AN INTACT CANOPY, because this is a new match and the last one's damage is not
    // this one's. Faults never heal by design, so nothing else was ever going to clear
    // them: vg_canopy_hit_clear ran once at boot, and a second match inherited the
    // panels the first one lost -- flashing through the intro and carrying on after it
    // over a hull at full health.
    //
    // Here rather than in the state code because both match-start paths call this, and a
    // third would have had to remember.
    vg_canopy_hit_clear();

    // NO COCKPIT, NO SEQUENCE -- BUT THE CHAIN STILL HAS TO RUN.
    //
    // The sequence is the cockpit arriving a region at a time, and with no drawing there are no
    // regions and no zone map to hold the world black with. So it does not play.
    //
    // The cue is latched anyway, and that is the part that matters: the instruments hang off it
    // -- draw_instruments is vg_cockpit.cued -- so returning early here left a hull with no canopy
    // showing no cockpit AND no instruments, for ever. The chain degrades to what the game did
    // before there were canopies: the panel catches, then the player is ready, then the radio.
    if (!canopy_any()) {
        s_intro_on = false;
        s_gate_on  = false;   // nothing to reveal and nothing broken
        s_icued    = true;
        s_settle_t = -1.0f;
        for (int i = 0; i < 3; i++) { s_lag_q[i] = s_lag_v[i] = s_lag_x[i] = 0.0f; }
        s_lag_px = s_lag_py = 0;
        s_lag_sh = 0.0f;
        return;
    }
    s_intro_on = true;
    s_gate_on  = true;   // the reveal IS the gate; set here so frame one has it
    s_intro_t  = 0.0f;
    s_settle_t = -1.0f;
    s_icued    = false;
    // ALL SIXTEEN, NOT THIS DRAWING'S OWN COUNT, and the difference was a bug you
    // could only see the second time you flew.
    //
    // This held the drawing's own count of regions, four on the CHARIOT's light delta -- and
    // its opaque bake has six. Regions four and five kept the s_ilive the LAST match left
    // set, so their share of the cockpit was simply present from the first frame of the
    // arrival: pieces of the canopy activated ahead of the sequence. On a cold boot the
    // array is zero and nothing shows, which is why it hid until a restart.
    //
    // Clearing state that no drawing has cannot be wrong, and it does not depend on which
    // cockpit has been selected yet -- which matters, because this runs from the top of
    // the cutscene and the selection has not necessarily happened.
    for (int z = 0; z < VG_CANOPY_MAX_ZONES; z++) {
        s_izon[z] = 0; s_ilive[z] = 0; s_ifill[z] = 0;   // held, and held BLACK
        s_iglow[z] = 255;                                // and white-hot the moment it lights
    }
    // Nothing left over on the spring, or the frame would start the sequence already leaning.
    for (int i = 0; i < 3; i++) { s_lag_q[i] = s_lag_v[i] = s_lag_x[i] = 0.0f; }
    s_lag_px = s_lag_py = 0;
    s_lag_sh = 0.0f;
}


// HAS THE SEQUENCE CALLED FOR THE INSTRUMENTS YET.
//
// A LATCH, and it is a latch because the obvious version was a bug. This started as a
// `progress() >= CANOPY_INTRO_HUD_AT` test against a progress function that returned 1.0 when
// nothing was running -- so the condition was satisfied by DEFAULT, and vg_hud_decay is reached
// from vg_world_step, which runs in the attract loop, the title and the cutscene. The cockpit's
// power-on sound played on the title screen.
//
// Worse, it was not idempotent across matches: vg_match_start is called from enter_intro, at the
// top of the CUTSCENE, so the game-side flag was cleared several seconds before the player ever
// took the seat -- and the cutscene then ran thousands of world steps with the cue armed.
//
// So the state cannot be inferred. Only the running sequence sets this, and only `reset` and
// `begin` clear it, which makes every ordering safe: nothing is cued at power-on, nothing is
// cued while a match is being built, and nothing is cued twice.
bool vg_canopy_intro_cued(void) { return s_icued; }

// HOW MANY REGIONS HAVE LIT IN THE RUNNING SEQUENCE, 0..canopy_zones() -- this drawing's
// count, not the format ceiling VG_CANOPY_MAX_ZONES.
//
// Reported rather than acted on, because the thing that wants it is a SOUND and this file is
// the band rasteriser -- it has no business talking to the mixer, and a cue fired from here
// would be a cue that does not happen when the panel is not being drawn. The caller watches
// this for a change and beeps, which also lets it pitch each one.
//
// ZERO WHEN NOTHING IS RUNNING, and that is not a detail. `reset` leaves every region live so
// the flight path draws the whole frame, so counting s_ilive unconditionally reports four the
// entire time a match is not booting -- and the caller, whose own counter was just zeroed,
// would have fired four beeps over the cutscene. That is the same trap the instruments' cue
// fell into: a query that answers for a state it is not in. The answer has to be scoped to the
// sequence, because that is the only thing the question means.
int vg_canopy_intro_lit(void) {
    if (!canopy_any() || !s_intro_on) return 0;
    int n = 0;
    for (int z = 0; z < canopy_zones(); z++) if (s_ilive[z]) n++;
    return n;
}

// Nothing running and nothing cued -- for when a match is BUILT, which happens at the top of the
// cutscene and is a long way from the player taking the seat.
void vg_canopy_intro_reset(void) {
    s_intro_on = false;
    s_icued    = false;
    s_settle_t = -1.0f;
    // A HULL WITH NO DRAWING REACHES HERE, and this used to read the drawing's zone count anyway.
    //
    // vg_match_start calls this from the top of the cutscene, before anything has selected a
    // cockpit for the hull about to fly -- so on a hull that HAS no cockpit, the drawing was null and
    // the load faulted. The panic rebooted the board, and from the seat that looks like the
    // match refusing to start and dropping back to the title. Reported exactly that way.
    //
    // Guarded here rather than at the call, because the three scalars above are the boot
    // chain's disarm and have to happen for every hull. Only the per-zone arrays need a
    // drawing to be about.
    if (!canopy_any()) return;
    // All sixteen, for the reason vg_canopy_intro_begin gives at its own loop.
    for (int z = 0; z < VG_CANOPY_MAX_ZONES; z++) { s_izon[z] = 255; s_ilive[z] = 1; s_iglow[z] = 0; }
}

// HOW MUCH FLEX THE FRAME IS ALLOWED, 0 through the sequence and 1 once it has settled.
//
// The caller multiplies both the warp and the lag by this. It is not an optimisation: the world
// gate reads the unwarped column, so a warped frame during the sequence would put the black
// somewhere other than where the panel ends. The ramp afterwards exists because the resting
// warp is full bulge -- releasing straight into it would pop.
float vg_canopy_intro_flex(void) {
    if (!canopy_any()) return 1.0f; // nothing to hold flat, and nothing to settle
    if (s_intro_on) return 0.0f;
    if (s_settle_t < 0.0f) return 1.0f;
    float a = s_settle_t / CANOPY_INTRO_SETTLE;
    if (a >= 1.0f) return 1.0f;
    return a * a * (3.0f - 2.0f * a);          // smoothstep, so it arrives without a corner
}

bool vg_canopy_intro_update(float dt) {
    if (!canopy_any()) return false;
    if (!s_intro_on) {
        if (s_settle_t >= 0.0f) {
            s_settle_t += dt;
            if (s_settle_t >= CANOPY_INTRO_SETTLE) s_settle_t = -1.0f;
        }
        return false;
    }
    s_intro_t += dt;

    // THE INSTRUMENTS' CUE, latched from inside the running sequence -- which is the only place
    // that can know the sequence is actually running. The length is derived here from the pacing
    // constants, so retuning them moves the cue with them rather than sliding it out of step.
    const float span = CANOPY_INTRO_LEAD + (float)(canopy_zones() - 1) * CANOPY_INTRO_STEP
                     + CANOPY_INTRO_FLASH + CANOPY_INTRO_DISSOLVE + CANOPY_INTRO_LIT;
    if (!s_icued && span > 0.0f && s_intro_t >= span * CANOPY_INTRO_HUD_AT) s_icued = true;

    for (int z = 0; z < canopy_zones(); z++) {
        const float e = s_intro_t - (CANOPY_INTRO_LEAD + (float)z * CANOPY_INTRO_STEP);
        if (e < 0.0f) continue;                  // this one's turn has not come
        s_ilive[z]  = 1;
        s_ifill[z]  = CANOPY_INTRO_WHITE;        // it has flashed, so a held pixel is white now

        // The region holds solid white for FLASH, then the world dissolves out of it. A flash
        // has an instant onset by definition, so there is no ramp up -- the fill switches from
        // black to white on the frame the zone's turn arrives.
        float r = (e - CANOPY_INTRO_FLASH) / CANOPY_INTRO_DISSOLVE;
        if (r < 0.0f) r = 0.0f;
        if (r > 1.0f) r = 1.0f;
        s_izon[z] = (uint8_t)(r * 255.0f + 0.5f);

        // The members HOLD at full heat through the dissolve and cool only afterwards.
        //
        // Cooling them from the moment the region lit was the obvious reading and it wasted the
        // effect: a member is invisible against the region's own white fill, so at the instant the
        // glow peaked there was nothing behind it to clip against, and by the time world cells
        // came through the colour had already gone. The heat has to still be there when the world
        // arrives, because the world is what it saturates against.
        //
        // So the frame is the LAST thing to settle, cooling over LIT after its region is fully in.
        float f = 1.0f - (e - CANOPY_INTRO_FLASH - CANOPY_INTRO_DISSOLVE) / CANOPY_INTRO_LIT;
        if (f < 0.0f) f = 0.0f;
        if (f > 1.0f) f = 1.0f;
        f *= CANOPY_INTRO_LIT_PEAK;
        // Quantised, so the renderer's per-region tables rebuild a couple of dozen times
        // across a cool-down rather than once a frame. It reads this through
        // vg_canopy_zone_glow and keeps its own record of what each table was built for.
        const int q = (int)(f * CANOPY_INTRO_QSTEP + 0.5f);
        s_iglow[z] = (uint8_t)((q * 255) / CANOPY_INTRO_QSTEP);
    }

    // Over when the last zone's members have finished cooling, which is now the last thing to
    // happen anywhere in the sequence. The settle that follows is not part of it: the view is
    // fully in well before this and the frame is merely taking up its flex.
    const float end = CANOPY_INTRO_LEAD + (float)(canopy_zones() - 1) * CANOPY_INTRO_STEP
                    + CANOPY_INTRO_FLASH + CANOPY_INTRO_DISSOLVE + CANOPY_INTRO_LIT;
    if (s_intro_t >= end) {
        s_intro_on = false;
        s_settle_t = 0.0f;
        for (int z = 0; z < VG_CANOPY_MAX_ZONES; z++) {
            s_izon[z] = 255; s_ilive[z] = 1; s_iglow[z] = 0;
        }
        return false;
    }
    return true;
}

// Which balance point applies, for draw_band's two-core split.
// BALANCING TIME, NOT PIXELS.
//
// The baked split puts the cut where the DRAWING costs the same either side, and measured
// in a course run that still left core 1 finishing its half in 6,701 us and then standing
// at the rendezvous for 2,217 while core 0 took 8,918 for an equal share of the picture.
//
// The two cores are not equally fast at the same work. Core 0 carries the audio task and
// the system's own, so an even share of pixels is an uneven share of time, and a band costs
// the SLOWER half. No table baked from the drawing can know that -- it is a property of what
// else is running, and it moves.
//
// So it is measured. The bias nudges toward whichever side is waiting, one row at a time,
// and settles wherever the two halves finish together. One row of 32 is about 3% of a band,
// small enough that a wrong step costs nothing and slow enough that it does not chase noise.
//
// IT BALANCES THE WHOLE PRIMITIVE PASS NOW, not the canopy alone. draw_band's fork grew
// from the canopy case to take every primitive of the band, and the half/wait pair feeding
// this comes off brackets around that whole fork -- so the bias settles where the two
// cores' shares of ALL the drawing finish together. The baked table still supplies the
// starting cut, because the canopy remains the heaviest and most uneven item in the pass;
// the bias absorbs everything else, the same way it already absorbed the other core's load.
static int s_split_bias = 0;

void vg_canopy_split_nudge(uint32_t half_us, uint32_t wait_us) {
    // Only while there is something to balance. A frame with no canopy reports both at zero
    // and must not drag the bias anywhere.
    if (!half_us) return;
    // A FORTIETH of the half. A tenth parked the bias at 18 rows with 613 us still showing,
    // and tightening it did close that -- canw fell to 157 and the cut moved to 19.6.
    //
    // IT BOUGHT NOTHING, and that is worth writing down rather than tuning further. A band
    // costs max(half, half), and core 1 simply absorbed the work core 0 put down: 7,841/8,454
    // became 8,314/8,471. Both settings sit on the flat bottom of the same curve. The tighter
    // one is kept because a balance held to 157 us survives a change of scene better than one
    // held to 613, not because it is faster.
    //
    // The split is finished. What is left in the canopy is its work, not its scheduling.
    const uint32_t dead = half_us / 40u;
    if (wait_us > dead) {
        // This core waited: the other side is slower, so give it less by cutting later.
        if (s_split_bias < BAND_H / 3) s_split_bias++;
    } else if (wait_us < dead / 2u) {
        // It did not wait at all, so this side is the slow one. Cut earlier.
        if (s_split_bias > -BAND_H / 3) s_split_bias--;
    }
}

int vg_canopy_split_at(int band_index) {
    if (!canopy_any()) return ROW_SPLIT;
    if (s_intro_on)  return ROW_SPLIT;   // the world gate dwarfs the frame; midpoint is right
    // The warp's table, which warp_build fills from the column costs above whatever the
    // bend is -- at zero bend that IS the unwarped balance.
    int at = (int)s_wsplit[band_index];
    // THE ADAPTIVE TERM IS DROPPED UNDER A REPLAY, and this is what makes a render
    // reproducible at all.
    //
    // s_split_bias is nudged by MEASURED TIME, so it settles somewhere slightly
    // different on every run of the same build -- and with SPLIT_LINE_CLAMPED the
    // seam carries a +-1 px jog, so a cut one row further down moves pixels. Two
    // runs of one build therefore render two different pictures, which is what had
    // phantom_regress reporting frames DIFFERENT for changes that cannot move a
    // pixel, and what this project has been recording as an "FPU ghost" since the
    // band checksum first disagreed with itself.
    //
    // The BAKED per-band split stays -- it is a property of the drawing, not of the
    // clock -- so a replay still forks, still costs what it costs, and still
    // exercises both cores. Only the term that listens to a stopwatch goes.
    if (vg_replay_mode() == VG_RP_OFF) at += s_split_bias;
    // Clamped well inside the band: a split at 0 or BAND_H is not a split, and the pass
    // would silently go back to one core.
    if (at < 2)              at = 2;
    if (at > BAND_H - 2)     at = BAND_H - 2;
    return at;
}

// AND `oth` SPLIT INTO ITS THREE, for the same reason the canopy got its own counter.
// A DEAD COUNTER FOR AS LONG AS THE TINT HAS BEEN DRAWN AT THE SOURCE.
//
// s_tint_us was declared, reset and read, and nothing ever wrote to it, so `tnt` reported
// 0 for a warning that costs over a millisecond. It cost real time to trust: it was quoted
// three times as evidence the boundary effect was cheap, and then as evidence it was
// expensive, and the number behind both was never measured at all.
//
// THE COST IS NOT SEPARABLE ANY MORE, which is why this returns a LEVEL and not a time.
// The tint moved into the sky fill and into submit, and then out of both --
// so its work is interleaved with the fill it colours, and bracketing it would mean a
// second pass to measure the first. What it costs is visible directly in `sky`, measured:
// 1792 us with the warning off and 3028 with it on, up to 3445 hard against the wall.
//
// So `tnt` reports HOW HARD THE WARNING IS RUNNING, 0 to 100. That is the number that was
// actually missing: without it there is no way to tell which telemetry windows had the
// effect active, so an average over a flight buries it -- which is exactly how a 1240 us
// cost and six frames a second were measured as "no change".
// HOW HARD THE WALL WARNING IS RUNNING, 0 to 100. Not microseconds, and it has been three
// different wrong things.
//
// It was a dead counter for as long as the tint was drawn at the source: s_tint_us was
// declared, reset and read and nothing ever wrote to it, so `tnt` reported 0 for an effect
// costing over a millisecond. Then it was pointed at s_tint_k -- in the same commit that
// switched the ring off by handing vg_rast_tint a zero -- so it faithfully reported that
// zero while the cockpit was visibly red on the panel. Two flights each time.
//
// The level is what was actually needed. Without it there is no way to tell which telemetry
// windows had the warning up, so an average over a flight buries the effect entirely -- which
// is how 1240 us and six frames a second were once measured as "no change".
uint32_t vg_rast_tint_us(void) {
    return (uint32_t)(s_alarm * 100.0f + 0.5f);
}

// WHAT THE DRAWING COSTS, on the device and without flying.
//
// The canopy only draws inside a match, so reading its cost off the frame counter needs
// someone at the controls -- and that counter reports one band's worse half plus the
// rendezvous, which is not a number the baker can predict from a table. This runs the
// whole pass on one core over a scratch row and reports it straight.
//
// Row by row, because a real band buffer is 30 KB and there is not that much heap free
// at runtime. Each panel row is an independent column of the drawing, so a row at a time
// is the same work in the same order; internal SRAM is uniform, so a reused row costs
// what a spread-out band costs.
//
// The scratch is refilled between rows, outside the timing. Blending into the same row
// 480 times would drive every channel to the rail and leave every saturation branch
// taken, which is not what the pass meets in a frame.
static uint16_t s_bench_row[SCR_W];

// THE OPAQUE PASS ON THE SAME TERMS: the whole pass, one core, a scratch row at a
// time, so the `can` the replay reports -- one band's slower half -- can be read
// against the work it is a half of. Rigid first, then at full bend with no lag.
// The warp maps are left built for full bend and s_wq says so, so the next
// frame's build call puts back whatever the game wants.
void vg_canopy_op_bench(uint32_t* rigid_us, uint32_t* full_us) {
    *rigid_us = *full_us = 0;
    if (!vg_canopy_op_current()) return;
    const int   save_want = s_wq_want;
    const bool  save_on = s_warp_on, save_in = s_intro_on, save_gate = s_gate_on;
    const int   save_px = s_lag_px, save_py = s_lag_py;
    const float save_sh = s_lag_sh;
    s_intro_on = false;
    s_gate_on  = false;
    s_warp_on  = false;
    uint32_t cyc = 0;
    for (int py = 0; py < SCR_H; py++) {
        for (int x = 0; x < SCR_W; x++) s_bench_row[x] = 0x1084;
        const uint32_t t0 = esp_cpu_get_cycle_count();
        vg_canopy_op_rows(s_bench_row, py, 0, 1);
        cyc += esp_cpu_get_cycle_count() - t0;
    }
    *rigid_us = cyc / 240u;

    s_lag_px = 0; s_lag_py = 0; s_lag_sh = 0.0f;
    s_wq_want = CANOPY_WARP_STEPS;
    vg_canopy_warp_build();
    s_warp_on = true;
    cyc = 0;
    for (int py = 0; py < SCR_H; py++) {
        for (int x = 0; x < SCR_W; x++) s_bench_row[x] = 0x1084;
        const uint32_t t0 = esp_cpu_get_cycle_count();
        vg_canopy_op_rows(s_bench_row, py, 0, 1);
        cyc += esp_cpu_get_cycle_count() - t0;
    }
    *full_us = cyc / 240u;

    s_wq_want = save_want; s_warp_on = save_on;
    s_intro_on = save_in;  s_gate_on = save_gate;
    s_lag_px = save_px; s_lag_py = save_py; s_lag_sh = save_sh;
}

