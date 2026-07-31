#include "vg_draw.h"
#include "vg_game.h"
#include "vg_voice.h"
#include "vg_arena.h"
#include "vg_course.h"
#include "vg_sfx.h"
#include "vg_screens.h"
#include <stdio.h>
#include <math.h>

// The instrument panel. Everything here is drawn through the spherical warp
// (vg_render.cpp brackets the call), EXCEPT the pieces that have to line up with
// the world or with the player's finger -- those are called outside it.

// AmberConsole-style panel: a 2px frame with an inverse-video label tab riding
// the top edge. No shadow, no elevation, no radius, no second hue -- the label
// reads by inversion, not by colour.
static void hud_panel(int x, int y, int w, int h, const char* label) {
    // Sink the panel into a near-black well first. Instruments used to sit on a
    // pure black screen; against a lit nebula, thin amber strokes lose contrast
    // and read as ragged. This gives them their own dark ground back.
    vg_fill_rect(x, y, w, h, INK_WELL);

    const int s = HUD_STROKE;
    vg_fill_rect(x,         y,         w, s, INK);
    vg_fill_rect(x,         y + h - s, w, s, INK);
    vg_fill_rect(x,         y,         s, h, INK);
    vg_fill_rect(x + w - s, y,         s, h, INK);

    if (!label) return;
    int lw = vg_text_width(label, 1);
    vg_fill_rect(x + 5, y - 5, lw + 8, 11, INK);
    vg_text(x + 9, y - 3, label, INK_ONFILL, 1);
}

static void draw_reticle(void) {
    const int cx = SCR_W / 2, cy = SCR_H / 2;
    const int g = 11, len = 9;
    vg_line(cx - g - len, cy, cx - g, cy, COL_HUD);
    vg_line(cx + g, cy, cx + g + len, cy, COL_HUD);
    vg_line(cx, cy - g - len, cx, cy - g, COL_HUD);
    vg_line(cx, cy + g, cx, cy + g + len, COL_HUD);
    vg_point(cx, cy, COL_STAR_BRIGHT);
}

// Hull integrity. The fill slides amber -> red as it drops and blinks below the
// self-repair floor, so "I am below 30% and this damage is now permanent" is
// legible without a number.
static void draw_health(void) {
    const int x = 14, y = 16, w = 180, h = 26;
    hud_panel(x, y, w, h, "HULL");

    // Hull is absolute points now, so the gauge shows the FRACTION -- a CHARIOT
    // at 70/70 reads as full, same as an AEGIS at 110/110.
    float hp = (vg.health_max > 0.0f) ? (vg.health / vg.health_max) : 0.0f;
    if (hp < 0.0f) hp = 0.0f;
    if (hp > 1.0f) hp = 1.0f;

    uint16_t c = vg_mix(COL_HEALTH_LOW, INK_BRIGHT, hp);
    if (hp < HEALTH_LOW && fmodf(vg.state_t, 0.52f) < 0.28f) c = INK_TRACE;

    int fw = (int)((float)(w - 8) * hp);
    if (fw > 0) vg_fill_rect(x + 4, y + 4, fw, h - 8, c);

    // Segment ticks, in the on-fill tone: they show across the filled part and
    // vanish into the empty part, which reads as a real segmented gauge.
    for (int i = 1; i < 5; i++)
        vg_fill_rect(x + 4 + (w - 8) * i / 5, y + 4, 2, h - 8, INK_ONFILL);

}

// Radio traffic from the other pilot. Callsign in inverse video, message beside
// it -- the panel idiom, so it reads as an instrument rather than a subtitle.
//
// Aligned with the TOP of the throttle strip, which is as low as it can sit and
// still be read. It was below the reticle, which is the emptiest band of the
// HUD and looked like the obvious home for it -- but on a device this size the
// hands wrap the bottom corners to hold it, and a thumb reaching for the
// throttle covers anything down there. Nobody grips a screen by its top edge,
// so the upper half is the part that is reliably visible.
static void draw_comms(void) {
    if (!vg.comms_line || vg.comms_t <= 0.0f) return;

    // +4 so the tag block's top edge, not its baseline, lines up with the strip.
    const int   y  = THROTTLE_TOP + 4;
    const bool  badge = vg.comms_mark;
    const int   tw = badge ? vg_text_width(vg.comms_tag, 2) : 0;
    const int   gap = badge ? 14 : 0;
    const int   mw = vg_text_width(vg.comms_line, 2);
    const int   x  = (SCR_W - (tw + gap + mw)) / 2;

    // A last transmission blinks. Brightness and blink are the whole vocabulary
    // here, and a death is the one line worth interrupting the player for.
    const bool  dying = (vg.comms_pri == (uint8_t)VOICE_DEATH);
    if (dying && fmodf(vg.comms_t, 0.44f) < 0.16f) return;

    if (badge) {
        vg_fill_rect(x - 5, y - 4, tw + 10, 22, dying ? INK_MAX : INK);
        vg_text(x, y, vg.comms_tag, INK_ONFILL, 2);
    }
    vg_text(x + tw + gap, y, vg.comms_line, dying ? INK_MAX : INK_BRIGHT, 2);
}

// A caution annunciator: a solid block with the label knocked out of it.
//
// The strongest thing a single-hue interface can do, and the reason both alerts
// use it instead of red text. They used to be COL_DANGER and COL_WARN, which
// spent hue on urgency -- and hue is reserved for identity here, so a red word
// was competing with the one thing colour is allowed to mean.
static void hud_annunciator(int y, const char* s, int scale, uint16_t ink) {
    const int tw = vg_text_width(s, scale);
    const int bw = tw + 28;
    const int bh = 7 * scale + 14;
    const int bx = (SCR_W - bw) / 2;

    vg_fill_rect(bx, y, bw, bh, ink);
    // INK_ONFILL, not COL_BLACK: vg_text discards colour 0, so a black label
    // would leave a blank block.
    vg_text(bx + 14, y + 7, s, INK_ONFILL, scale);
    // Outer rule standing off the block -- the bracketed-caution motif.
    vg_rect(bx - 5, y - 5, bw + 10, bh + 10, ink);
}

// Is an alert lit this instant?
//
// `k` is 0 at the far edge of the alert's range and 1 at the thing itself. The
// rate is the range: a player who cannot spare attention for a number can still
// feel a rhythm getting faster. Both alerts share this because they now behave
// identically -- the missile alert had its own double-beat shape and it read as a
// flicker.


// Incoming missile. Flashing accelerates as the seeker closes, and that is all
// it does. There used to be a final stage that held the block solid inside
// MSL_ALERT_SOLID, on the theory that evasion was no longer the question -- but
// a warning that stops moving stops being read, and the fastest flash already
// says everything the solid one did.
static void draw_missile_alert(void) {
    // Decided in the update -- range, cadence and beep together. See
    // update_alerts() in vg_game.cpp.
    if (!vg.alert_msl_lit) return;

    hud_annunciator(62, "MISSILE", 2, INK_MAX);
}

// The boundary. Same cadence, and RED -- the only place in this interface that
// spends hue on urgency. The rule is that hue means identity, and the exception
// earns itself: a collision is instant death, so this is the one warning where
// being ignored ends the run rather than costing some hull.
static void draw_boundary_alert(void) {
    // Decided in the update, alongside the beep, so the two cannot drift apart.
    if (!vg.alert_wall_lit) return;
    hud_annunciator(128, "BOUNDARY", 2, COL_DANGER);
}

// The broadcast caption: a full-width band in WHITE, with the IFT mark where a
// pilot's callsign block would be.
//
// Distinguished by FORM as well as by colour, deliberately. Colour alone is a weak
// signal at a glance, and the wall tint can now push the whole frame red -- so the
// shape has to carry it too. A pilot speaks as a tag and a message on the throttle
// line; the IFT speaks as a rule across the screen, which reads as broadcast
// furniture rather than as a pilot who happens to be white.
//
// SCALE 3, because this is the system talking and it was reading as a footnote.
// That costs width: 18 pixels a character means about 24 fit across, against 34 at
// scale 2. See the note in vg_ift.cpp.
//
// NOT a lower third, despite that being where a broadcast puts one. The radar is
// centred at y=452 with a 78-pixel radius, so it owns 374 to the bottom edge, and
// the first version of this band sat straight on top of it -- invisible in the
// attract loop, where there is no HUD, and a mess in an actual match. This sits in
// the clear band between the reticle at 240 and the radar.
void vg_draw_ift(void) {
    if (!vg.ift_line || vg.ift_t <= 0.0f) return;

    const int y = 300;
    const int h = 46;

    // Scale 3 is the voice this thing speaks in, and it holds unless the line
    // genuinely will not fit. Dropping to 2 is a fallback, not a choice: the
    // alternative is overhanging both edges, and a sentence the author wrote is
    // not something to truncate to protect a font size.
    // Only the line that OPENS a run carries the mark. Everything after it is
    // the same voice continuing, so the badge is forty-odd pixels spent
    // repeating what the player already knows -- and it costs the line six
    // characters at scale 3, which is the difference between fitting and not.
    //
    // This works because the game is a duel. There is no third party and no
    // crowded channel: whoever started talking is still talking, and the only
    // question a badge ever answers is who just started.
    const bool badge = vg.ift_mark;
    const int mark_end = badge ? (8 + vg_text_width("IFT", 2) + 10 + 6) : 8;

    // Fit against the space the line can ACTUALLY use, which is everything right
    // of the mark. The test used to reserve the mark's width on BOTH sides, to
    // keep the line centred on the whole panel -- but the placement below already
    // gives that up and re-centres in the clear space when it has to. So the test
    // was stricter than the rule it was testing for, and dropped lines to scale 2
    // that would have fitted at 3 with room to spare.
    const int clear = SCR_W - mark_end - 10;

    int scale = 3;
    if (vg_text_width(vg.ift_line, scale) > clear) scale = 2;

    const int mw = vg_text_width(vg.ift_line, scale);

    // Centred on the whole width while it fits, because that is where a caption
    // belongs. A line long enough to reach the mark is re-centred in the clear
    // space to the right of it instead -- off-centre by twenty pixels is not
    // something anyone will see, and the first version ran the author's longest
    // line straight through the IFT badge.
    int mx = (SCR_W - mw) / 2;
    if (badge && mx < mark_end) {
        mx = mark_end + (SCR_W - mark_end - mw) / 2;
        if (mx < mark_end) mx = mark_end;
    }
    if (mx < 4) mx = 4;

    // Rules rather than a filled band: a solid white block would be the brightest
    // thing on a panel whose whole hierarchy is brightness, and it would bury the
    // instruments it is meant to sit behind.
    vg_fill_rect(0, y,                  SCR_W, HUD_STROKE, COL_IFT);
    vg_fill_rect(0, y + h - HUD_STROKE, SCR_W, HUD_STROKE, COL_IFT);
    // Vertically centred on the band whichever size it ended up at: a glyph is
    // 7 rows tall before scaling.
    vg_text(mx, y + (h - 7 * scale) / 2, vg.ift_line, COL_IFT, scale);

    // The mark, inverse-video at the left, where a callsign block sits on the
    // pilots' channel. First line of a run only.
    if (badge) {
        const int tw = vg_text_width("IFT", 2);
        vg_fill_rect(8, y + 12, tw + 10, 18, COL_IFT);
        vg_text(13, y + 15, "IFT", INK_ONFILL, 2);
    }
}

static void draw_throttle(void) {
    const int x0 = THROTTLE_X0, w = THROTTLE_W;
    const int y0 = THROTTLE_TOP, h = THROTTLE_BOT - THROTTLE_TOP;

    hud_panel(x0, y0, w, h, "THR");

    int fill = (int)(h * vg.throttle);
    if (fill > 4) vg_fill_rect(x0 + 4, y0 + h - fill, w - 8, fill - 3, INK_TRACE);

    // Bright cap at the setting -- brightness is the hierarchy, so the value line
    // is the brightest thing in the control.
    int ly = y0 + h - fill;
    if (ly < y0 + 2)     ly = y0 + 2;
    if (ly > y0 + h - 5) ly = y0 + h - 5;
    vg_fill_rect(x0 + 2, ly, w - 4, HUD_STROKE, INK_MAX);

    for (int i = 0; i <= 4; i++)
        vg_fill_rect(x0 + w + 4, y0 + (h * i) / 4, 7, HUD_STROKE, INK_TRACE);
}

// ---------------------------------------------------------------------------
// Radar
// ---------------------------------------------------------------------------

// A point on the dome. a=0 is the right end of the chord, a=pi/2 the top (dead
// ahead), a=pi the left end. rn is normalised range.
static void radar_pt(float a, float rn, float* px, float* py) {
    *px = RADAR_CX + RADAR_RX * rn * cosf(a);
    *py = RADAR_CY - RADAR_RY * rn * sinf(a);
}

static void radar_arc(float rn, uint16_t col, int segs, int w) {
    float px, py;
    radar_pt(0.0f, rn, &px, &py);
    for (int i = 1; i <= segs; i++) {
        float nx, ny;
        radar_pt(3.14159265f * (float)i / (float)segs, rn, &nx, &ny);
        vg_line_w(px, py, nx, ny, col, w);
        px = nx; py = ny;
    }
}

// Plot one contact. `half` is the blip half-size in pixels, so a fighter reads
// heavier than a missile at a glance.
static void radar_contact(Vec3 pos, uint16_t col, int half, bool box, uint16_t box_col) {
    float range = sqrtf(pos.x * pos.x + pos.z * pos.z);
    float rn = range / RADAR_RANGE;
    if (rn > 1.0f) rn = 1.0f;

    float bearing = atan2f(pos.x, pos.z);      // 0 ahead, + right, +-pi behind

    if (fabsf(bearing) <= 1.5707963f) {
        float px, py;
        radar_pt(1.5707963f - bearing, rn, &px, &py);
        vg_fill_rect((int)px - half, (int)py - half, half * 2 + 1, half * 2 + 1, col);
        if (box) {
            int b = half + 3;
            vg_rect((int)px - b, (int)py - b, b * 2 + 1, b * 2 + 1, box_col);
        }
    } else {
        // Behind: park it just under the chord on the correct side. Knowing
        // something is on your six is the single most useful thing a radar tells
        // you in a dogfight, so it must not simply vanish.
        float side = (bearing > 0) ? 1.0f : -1.0f;
        float px = RADAR_CX + side * RADAR_RX * rn;
        float py = RADAR_CY + 9.0f;
        float s  = (float)(half + 2);
        vg_line(px - s, py - s, px + s, py - s,         col);
        vg_line(px - s, py - s, px,     py + s * 0.75f, col);
        vg_line(px + s, py - s, px,     py + s * 0.75f, col);
    }
}

// Half-ellipse plan view: a circular radar seen in steep perspective. Forward is
// the top of the dome, the flat chord is the player's own 3-9 line. Fighters and
// incoming missiles only -- asteroids are terrain, not contacts.
static void draw_radar(void) {
    // The rim and the chord are the instrument's border, so they carry a heavier
    // stroke; the inner range rings stay hairlines so they read as graduations
    // rather than as structure.
    radar_arc(1.00f, COL_RADAR_RIM, 22, 2);
    radar_arc(0.66f, COL_RADAR, 16, 1);
    radar_arc(0.33f, COL_RADAR, 12, 1);

    vg_line_w(RADAR_CX - RADAR_RX, RADAR_CY, RADAR_CX + RADAR_RX, RADAR_CY,
              COL_RADAR_RIM, 2);

    for (int i = 1; i <= 3; i++) {
        float ex, ey;
        radar_pt(3.14159265f * (float)i / 4.0f, 1.0f, &ex, &ey);
        vg_line(RADAR_CX, RADAR_CY, ex, ey, COL_RADAR);
    }

    // Own ship, nose up.
    vg_line_w(RADAR_CX - 6, RADAR_CY + 4, RADAR_CX, RADAR_CY - 7, COL_HUD, 2);
    vg_line_w(RADAR_CX + 6, RADAR_CY + 4, RADAR_CX, RADAR_CY - 7, COL_HUD, 2);

    // Hostile missiles first so a bogey blip always draws over the top of one.
    for (int i = 0; i < MAX_MISSILES; i++) {
        const Missile* m = &vg.msl[i];
        if (!m->alive || m->from_player) continue;
        // A missile that has lost its lock is coasting and no longer hunting, so
        // it fades rather than disappearing -- it can still hit you by luck.
        uint16_t col = m->locked ? COL_RADAR_MSL : vg_dim(COL_RADAR_MSL, 0.45f);
        radar_contact(m->pos, col, 1, false, 0);
    }

    for (int i = 0; i < MAX_ENEMIES; i++) {
        const Ship* s = &vg.enemy[i];
        if (!s->alive) continue;
        bool is_lock = (i == vg.lock_target && vg.locked);
        // The blip stays red whatever its lock state; the lock reads as a box
        // around it, so colour always means "what is it" and never "what am I
        // doing about it".
        radar_contact(s->pos, COL_RADAR_BOGEY, 2, is_lock, INK_MAX);
    }
}

// ---------------------------------------------------------------------------
// World-anchored overlays (drawn OUTSIDE the warp -- they must line up with
// what they point at, or with the finger)
// ---------------------------------------------------------------------------

void vg_draw_steer_indicator(const VgInput* in) {
    if (!in->steering) return;
    const float ox = in->steer_ox, oy = in->steer_oy, r = 26.0f;

    float px = ox + r, py = oy;
    for (int i = 1; i <= 8; i++) {
        float a  = (float)i * (6.2831853f / 8.0f);
        float nx = ox + r * cosf(a), ny = oy + r * sinf(a);
        vg_line(px, py, nx, ny, COL_HUD_DIM);
        px = nx; py = ny;
    }
    vg_line(ox, oy, in->steer_x, in->steer_y, COL_HUD_DIM);
    vg_rect((int)in->steer_x - 4, (int)in->steer_y - 4, 9, 9, COL_HUD);
}

// Corner brackets around the tracked enemy: dim and wide while acquiring, tight
// and bright once locked. Carries the target's hull bar.
void vg_draw_lock_box(const VgCam& cam) {
    if (vg.lock_target < 0) return;
    const Ship* s = &vg.enemy[vg.lock_target];
    if (!s->alive) return;

    float cx, cy;
    if (!vg_project(cam, vg_view(cam, s->pos), &cx, &cy)) return;

    // Against the speed-scaled requirement, so the brackets visibly stop closing
    // when you are going too fast to get a lock at all.
    float need = (vg.lock_need > 0.01f) ? vg.lock_need : vg.spec->lock_time;
    float prog = vg.lock_t / need;
    if (prog > 1.0f) prog = 1.0f;

    float base = FOCAL * (ENEMY_SCALE * 2.6f) / (s->pos.z > NEAR_Z ? s->pos.z : NEAR_Z);
    if (base < 16.0f) base = 16.0f;
    float r = base + (1.0f - prog) * 42.0f;
    float L = r * 0.42f;

    // Monochrome hierarchy: a lock reads as maximum brightness plus blink, never
    // as a different hue.
    uint16_t col = vg.locked ? INK_MAX : INK_TRACE;
    if (vg.locked && fmodf(vg.state_t, 0.5f) > 0.38f) col = INK;

    const float xs[2] = { cx - r, cx + r };
    const float ys[2] = { cy - r, cy + r };
    for (int i = 0; i < 2; i++) {
        for (int j = 0; j < 2; j++) {
            float sx = xs[i], sy = ys[j];
            float dx = (i == 0) ? L : -L;
            float dy = (j == 0) ? L : -L;
            vg_line(sx, sy, sx + dx, sy, col);
            vg_line(sx, sy, sx, sy + dy, col);
        }
    }

    // Enemy hull, sized off the bracket so it tracks the target and shrinks with
    // distance like the rest of the reticle. Shown while acquiring too, since
    // that is when you decide whether the target is worth a round.
    float bw = r * 1.6f;
    if (bw < 38.0f)  bw = 38.0f;
    if (bw > 130.0f) bw = 130.0f;

    float hp = s->hull / s->spec->hull;
    if (hp < 0.0f) hp = 0.0f;
    if (hp > 1.0f) hp = 1.0f;

    const int bx = (int)(cx - bw * 0.5f);
    const int by = (int)(cy - r - 15.0f);

    vg_rect(bx, by, (int)bw, 8, INK_FAINT);
    int fw = (int)((bw - 4.0f) * hp);
    if (fw > 0)
        vg_fill_rect(bx + 2, by + 2, fw, 4, vg_mix(COL_HEALTH_LOW, INK_BRIGHT, hp));

    // What you are up against decides whether to close or extend, so name it.
    vg_text(bx, by - 12, s->spec->name, INK_FAINT, 1);

    if (vg.locked) vg_text((int)(cx - 24), (int)(cy - r - 36), "LOCK", INK_MAX, 2);
}

// Triangle sitting on a ring around the crosshair, pointing outward along `ang`.
static void draw_ring_arrow(float ang, float radius, float size, uint16_t col, int w) {
    float ax = SCR_CX + cosf(ang) * radius;
    float ay = SCR_CY + sinf(ang) * radius;

    float tipx = ax + cosf(ang) * size,               tipy = ay + sinf(ang) * size;
    float l1x  = ax + cosf(ang + 2.5f) * size * 0.8f, l1y  = ay + sinf(ang + 2.5f) * size * 0.8f;
    float l2x  = ax + cosf(ang - 2.5f) * size * 0.8f, l2y  = ay + sinf(ang - 2.5f) * size * 0.8f;

    vg_line_w(tipx, tipy, l1x, l1y, col, w);
    vg_line_w(l1x, l1y, l2x, l2y, col, w);
    vg_line_w(l2x, l2y, tipx, tipy, col, w);
}

// Direction to a view-space point in screen terms. Returns true when it is in
// front of the camera (in which case sx/sy hold its projected position); `ang` is
// filled either way, so callers can point at things that are behind the player.
static bool screen_dir(const VgCam& cam, Vec3 p, float* sx, float* sy, float* ang) {
    // Aft first. "In front of the camera" means in front of where it is
    // POINTING, and the whole job of this function is to tell on-screen from
    // off-screen -- reading the unturned z would put every contact behind the
    // player on the wrong side of that test the moment they looked back.
    p = vg_view(cam, p);
    if (p.z >= NEAR_Z && vg_project(cam, p, sx, sy)) {
        *ang = atan2f(*sy - SCR_CY, *sx - SCR_CX);
        return true;
    }
    // Behind: mirror the lateral offset and apply the bank by hand, since the
    // perspective projection is meaningless back there.
    float x = -p.x, y = -p.y;
    float rx = x * cam.bank_c - y * cam.bank_s;
    float ry = x * cam.bank_s + y * cam.bank_c;
    *ang = atan2f(-ry, rx);
    return false;
}

// One triangle per trackable bogey: a caret above it while on screen, an arrow on
// the ring showing which way to turn once it is not. Without this a target that
// slides off the edge simply vanishes and you have no idea which way to haul.
void vg_draw_target_markers(const VgCam& cam) {
    for (int i = 0; i < MAX_ENEMIES; i++) {
        const Ship* s = &vg.enemy[i];
        if (!s->alive) continue;
        if (vlen(s->pos) > vg.spec->lock_range) continue;

        uint16_t col = (i == vg.lock_target) ? INK_MAX : COL_HUD;

        float sx = 0, sy = 0, ang = 0;
        bool infront  = screen_dir(cam, s->pos, &sx, &sy, &ang);
        bool onscreen = infront && sx > 26 && sx < SCR_W - 26
                                && sy > 26 && sy < SCR_H - 26;

        if (onscreen) {
            float r = FOCAL * (ENEMY_SCALE * 2.6f) /
                      (s->pos.z > NEAR_Z ? s->pos.z : NEAR_Z);
            if (r < 14.0f) r = 14.0f;
            if (r > 90.0f) r = 90.0f;
            float ty = sy - r - 14.0f;      // hovers above, pointing down at it
            vg_line_w(sx - 9, ty - 11, sx + 9, ty - 11, col, 2);
            vg_line_w(sx - 9, ty - 11, sx,     ty,      col, 2);
            vg_line_w(sx + 9, ty - 11, sx,     ty,      col, 2);
        } else {
            draw_ring_arrow(ang, 150.0f, 17.0f, col, 2);
        }
    }
}

// Arrow pointing at the nearest incoming missile, including when it is behind you
// -- exactly when you most need to know. Sits on a wider ring than the bogey
// markers so the two never read as the same thing.
void vg_draw_threat_indicator(const VgCam& cam) {
    if (!vg.threat) return;
    if (fmodf(vg.state_t, 0.42f) > 0.28f) return;   // blink

    float sx = 0, sy = 0, ang = 0;
    screen_dir(cam, vg.threat_pos, &sx, &sy, &ang);
    draw_ring_arrow(ang, 196.0f, 16.0f, COL_DANGER, 3);
}

// ---------------------------------------------------------------------------

void vg_draw_hud(const VgCam& cam, const VgInput* in, float fps) {
    (void)cam; (void)in;
    char buf[40];

    draw_health();
    draw_comms();

    // Missile rack: one tick per round, hollow while rearming. Vertical on the
    // right edge to mirror the throttle and leave the bottom for the radar.
    // The rack fits itself to the magazine: BALLISTA carries three rounds and
    // CHARIOT twelve, and at a fixed 22px pitch a twelve-round stack would run
    // straight down through the radar.
    const int mag   = vg.spec->magazine;
    int       pitch = 22;
    if (mag > 0 && mag * pitch > 250) pitch = 250 / mag;
    int cell = pitch - 6;
    if (cell < 6) cell = 6;

    // A reload is ONE BODY FILLING, and it has to look like one.
    //
    // The first version lit the cells one at a time as the clock ran down, which
    // is a perfectly good progress bar and taught precisely the wrong rule: the
    // magazine arrives all at once, and a player watching rounds appear one by
    // one will believe -- correctly, on the evidence in front of them -- that
    // they can fire as soon as the first one shows. They cannot. Nothing is
    // loaded until everything is.
    //
    // So the progress is drawn as a continuous column that ignores the cell
    // divisions entirely. It cannot be read as rounds arriving because it is
    // visibly not made of rounds, and it fills from the bottom, mirroring the
    // direction the rack empties.
    const float rl = (vg.reload_t > 0.0f && vg.spec->reload > 0.0f)
                   ? (1.0f - vg.reload_t / vg.spec->reload) : 0.0f;

    hud_panel(SCR_W - 40, 140, 30, mag * pitch + 8, "MSL");

    for (int i = 0; i < mag; i++) {
        int y = 148 + i * pitch;
        if (i < vg.missiles) vg_fill_rect(SCR_W - 34, y, 18, cell, INK_BRIGHT);
        else                 vg_rect(SCR_W - 34, y, 18, cell, INK_TRACE);
    }

    if (rl > 0.0f) {
        const int top = 148;
        const int bot = 148 + (mag - 1) * pitch + cell;
        int h = (int)((float)(bot - top) * rl);
        if (h > 0) vg_fill_rect(SCR_W - 34, bot - h, 18, h, INK);
    }

    // Speed reads top-centre, right where the eye already is for the crosshair,
    // with an inverse-video label beneath it.
    // Rounded, not truncated: 419.97 reading as "419" is its own way of looking
    // like the throttle never quite arrives.
    snprintf(buf, sizeof(buf), "%d", (int)(vg.speed + 0.5f));
    vg_text((SCR_W - vg_text_width(buf, 3)) / 2, 14, buf, INK_MAX, 3);
    {
        int lw = vg_text_width("SPD", 1);
        vg_fill_rect((SCR_W - lw - 8) / 2, 44, lw + 8, 11, INK);
        vg_text((SCR_W - lw) / 2, 46, "SPD", INK_ONFILL, 1);
    }

    snprintf(buf, sizeof(buf), "%d FPS", (int)(fps + 0.5f));
    vg_text(16, 452, buf, INK_TRACE, 1);
    vg_text(16, 464, vg_arena_name(), INK_TRACE, 1);

    draw_reticle();
    draw_throttle();
    draw_radar();

    // The streak, on the course only. It has to be persistent rather than a
    // line on the broadcast channel: the count IS the game here, and an IFT line
    // clears itself after a few seconds.
    if (vg.state == VG_COURSE) {
        char cbuf[16];
        snprintf(cbuf, sizeof(cbuf), "%d/%d", (int)vg.course_hits, COURSE_TARGET);
        const int cw = vg_text_width(cbuf, 3);
        vg_fill_rect((SCR_W - cw - 16) / 2, 60, cw + 16, 7 * 3 + 10, INK);
        vg_text((SCR_W - cw) / 2, 65, cbuf, INK_ONFILL, 3);
    }

    draw_missile_alert();
    draw_boundary_alert();
}
