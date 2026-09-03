#include "vg_draw.h"
#include "vg_cine.h"
#include "vg_flight.h"
#include "vg_game.h"
#include "vg_prof.h"
#include <Arduino.h>
#include <math.h>

// Everything with a position in the world: stars, dust, rocks, fighters,
// missiles, wreckage.

// `world`'s five parts plus the fireballs, defined beside the loops that time them.
// Read the RANGE of these rather than any one -- see g_w_motes in vg_prof.h.
uint32_t g_w_motes, g_w_rocks, g_w_trails, g_w_ships, g_w_msl, g_w_fire;

void vg_draw_starfield(const VgCam& cam) {
    static const uint16_t shades[3] = { COL_STAR_DIM, COL_STAR_MID, COL_STAR_BRIGHT };
    for (int i = 0; i < NUM_STARS; i++) {
        Vec3 s = vg_view(cam, vg.star[i]);
        if (s.z < NEAR_Z) continue;
        float x, y;
        if (!vg_project(cam, s, &x, &y)) continue;
        vg_point((int)x, (int)y, shades[vg.star_b[i]]);
    }
}

// The streak, which is drawing policy and lives here rather than in cfg_world.h:
// nothing else reads any of it. Where the field IS stays central -- the world
// step, the cutscene and the model builder all place motes and have to agree
// about the volume.
#define MOTE_STREAK_SEC      0.085f   // streak length, in seconds of travel
// Streaks grow SUPER-linearly with speed. Linear growth read as slightly longer
// dashes; squaring smears them into warp lines at full throttle.
#define MOTE_STREAK_BOOST    1.35f
#define MOTE_THICK_AT        0.55f    // speed fraction above which streaks thicken
#define MOTE_FADE_IN         0.18f    // speed fraction below which they hide

// Near-field dust streaking past. Stars sit at infinity and only rotate, so they
// convey no sense of speed whatsoever, and the asteroid field is far too sparse
// to. These are what make the throttle *feel* like a throttle.
//
// Each mote trails along +z: in view space the world slides toward -z, so a
// mote's tail lies further away than its head, and the streaks therefore splay
// outward from the vanishing point exactly as they should.
static void draw_motes(const VgCam& cam) {
    // NO DUST ASTERN, BY THE AUTHOR'S DECISION, and the reasoning it replaces is
    // worth keeping so nobody tries the third round.
    //
    // The field is never turned for the mirror: motes spawn between z 450 and 950
    // AHEAD and are recycled fifty units after they pass, so there is no dust
    // behind the ship at all and a turned camera correctly showed an empty mirror.
    // Drawing the forward field untouched was the trick that filled it -- dust has
    // no identity, so the field behind is the field ahead and reflecting it is
    // undetectable.
    //
    // What never came out right was WHICH WAY THE STREAK POINTS. A streak is laid
    // along z, so laying it away puts the tail near the vanishing point and laying
    // it toward puts the tail at the edge; astern it was drawn outward, longer and
    // fainter, to read as a wake being left behind. Two rounds of that reasoning
    // and it still did not read correctly on the panel, and a mirror whose dust
    // moves the wrong way states something false. The forward view already says
    // "you are moving", which is the only thing dust is for.
    //
    // It also pays: band 1 is where the mirror patch lands and it was the tallest
    // band in every combat window. A third of 160 motes, one primitive each, was
    // the mirror's biggest submission after the grid.
    if (cam.rear) return;

    // Not while paused. Motion is the only thing dust communicates, and a
    // frozen field of streaks is 160-320 primitives saying nothing.
    if (vg.state == VG_PAUSE) return;
    float sn = (vg.speed - vg.spec->speed_min)
             / (vg.spec->speed_max - vg.spec->speed_min);
    if (sn <= MOTE_FADE_IN) return;

    float f = (sn - MOTE_FADE_IN) / (1.0f - MOTE_FADE_IN);
    if (f > 1.0f) f = 1.0f;

    // Super-linear streak growth, plus a thicker stroke and brighter tone at the
    // top end, so firewalling the throttle is a visibly different state rather
    // than just a bigger number.
    const float    boost  = 1.0f + MOTE_STREAK_BOOST * f * f;
    float          streak = vg.speed * MOTE_STREAK_SEC * boost;
    int            w      = (f > MOTE_THICK_AT) ? 2 : 1;
    float          lvl    = 0.14f + 0.86f * f;

    const uint16_t col = vg_dim(COL_MOTE, lvl);

    // Dust, and only ever drawn at speed -- which makes it part of the exact
    // condition where the frame is tightest. Every mote is a short streak, two
    // primitives once it thickens, and there is no such thing as a visible
    // stair-step on a two-pixel speck moving at 460 units a second.
    vg_line_aa_mode(false);
    // The tail is laid AWAY along z, which puts it nearer the vanishing point:
    // dust sweeps out past you and what it leaves behind points back at the
    // middle. The aft case that used to invert this is gone -- see the top.
    for (int i = 0; i < NUM_MOTES; i++) {
        Vec3 p = vg.mote[i];
        if (p.z < NEAR_Z) continue;
        // Toward the camera the tail can cross the near plane, where the
        // projection is meaningless. Stop it short instead of clipping: a
        // shortened streak on the closest motes is invisible, a wild one is not.
        float tz = p.z + streak;
        if (tz < NEAR_Z) tz = NEAR_Z;
        vg_edge_w(cam, p, v3(p.x, p.y, tz), col, w);
    }
}

static void draw_debris(const VgCam& cam) {
    // Fragments live under a second and tumble the whole time.
    //
    // HOT, THEN COOLING. They were one-pixel COL_DEBRIS dimmed by life, which is
    // right for a scrape and disappears entirely inside an explosion -- thirty
    // shards of dim orange hairline against a frame that already has fireballs
    // in it read as nothing at all. A piece coming off a hull that just let go is
    // incandescent for the first moment: white-hot, down through orange, out. The
    // early frames also get a second pixel of width, which is what separates
    // ejecta from the trail hairlines around it.
    vg_line_aa_mode(false);
    for (int i = 0; i < MAX_DEBRIS; i++) {
        const Debris* d = &vg.deb[i];
        if (!d->alive) continue;
        const float f = d->life / d->life0;          // 1 at birth, 0 at death
        // Same ignition as the fireballs, so the flash is one event across the
        // whole explosion rather than the spheres flashing and the ejecta not.
        uint16_t col;
        if      (f > 0.95f) col = COL_FLASH;
        else if (f > 0.78f) col = vg_mix(COL_DEBRIS, COL_FLASH,
                                         (f - 0.78f) * (1.0f / 0.17f));
        else                col = vg_dim(COL_DEBRIS, f * (1.0f / 0.78f));
        const Vec3 tip = vadd(d->pos, d->seg);
        // Thicker, and it stays thick most of the way. One pixel was a hairline
        // lost against the trails; the taper to a single pixel only at the end
        // is what makes a shard read as cooling rather than as retreating.
        if      (f > 0.80f) vg_edge_w(cam, d->pos, tip, col, 3);
        else if (f > 0.40f) vg_edge_w(cam, d->pos, tip, col, 2);
        else                vg_edge  (cam, d->pos, tip, col);
    }
}

// --- fireballs -------------------------------------------------------------
//
// THE COLOUR IS THE EFFECT. A vector renderer cannot draw a ball of burning gas,
// so what sells it is the sequence: dark at ignition, up through orange, a
// moment of yellow-white at the top of the heat, then away. Drawn as concentric
// rings whose inner shells are held EARLIER in that same sequence, so the middle
// is always hotter than the rim -- which is what a fireball looks like and what
// a single flat circle cannot say.
//
// Hue here is world, not interface, so the orange and red world constants are
// the sanctioned set. vg_hue_col is not: hue means identity and a fireball
// belongs to nobody.
// `fall` is the ball's own decay exponent -- see Fireball::fall. Below 1 the
// brightness collapses early, which means vg_dim returns 0 and draw_fireballs
// skips it: the ball is visibly gone while its slot is still alive. That is the
// mechanism by which some of a cluster die before the others.
static uint16_t fire_col(float t, float fall) {
    // IGNITION IS WHITE, and only for a moment. Coming up out of black through
    // orange had no event in it -- the thing faded in. Starting at flat white and
    // falling out of it inside a twentieth of a second gives the frame an edge:
    // the eye catches the flash first and then reads the fireball it left behind.
    if (t < 0.05f) return COL_FLASH;
    if (t < 0.20f) return vg_mix(COL_FLASH, COL_DEBRIS,            // white -> orange
                                 (t - 0.05f) * (1.0f / 0.15f));
    // Cooling: down through the reds while going out. Squared so it holds its
    // brightness a moment and then leaves quickly, rather than sagging.
    const float d = (t - 0.20f) * (1.0f / 0.80f);
    return vg_dim(vg_mix(COL_DEBRIS, COL_MSL_HOSTILE, d), 1.0f - powf(d, fall));
}

// Opens fast, then keeps creeping outward while it cools. The first pass stopped
// at its nominal radius and then shrank slightly, which made even a ship death
// read as a fixed-size flare. A big blast does not stop growing the moment it
// stops being bright -- it goes on opening out as it goes out, and that continued
// expansion is most of what says the area involved was large.
static float fire_scale(float t) {
    if (t < 0.30f) return 0.30f + 0.85f * (t * (1.0f / 0.30f));   // 0.30 -> 1.15
    return 1.15f + 0.32f * ((t - 0.30f) * (1.0f / 0.70f));        // 1.15 -> 1.47
}

// Screen-space ring by incremental rotation: two trig calls for the whole
// circle instead of two per segment. The sky fill pays for its trig the same
// way and for the same reason.
static void fire_ring(float cx, float cy, float r, int segs, uint16_t col) {
    const float dth = 6.28318531f / (float)segs;
    const float c = cosf(dth), s = sinf(dth);
    float vx = r, vy = 0.0f;
    for (int k = 0; k < segs; k++) {
        const float nx = vx * c - vy * s;
        const float ny = vx * s + vy * c;
        vg_line(cx + vx, cy + vy, cx + nx, cy + ny, col);
        vx = nx; vy = ny;
    }
}

static void draw_fireballs(const VgCam& cam) {
    for (int i = 0; i < MAX_FIREBALLS; i++) {
        const Fireball* f = &vg.fire[i];
        if (!f->alive) continue;

        const float t  = 1.0f - f->life / f->life0;
        const float rw = f->r * fire_scale(t);

        VgSpan sp;
        if (!vg_screen_size(cam, f->pos, rw, rw, &sp)) continue;
        if (!sp.vis) continue;      // a ball whose middle is behind you is gone

        const float cx = sp.cx, cy = sp.cy, rpx = sp.rpx;
        if (cx < -rpx || cx > SCR_W + rpx || cy < -rpx || cy > SCR_H + rpx) continue;

        const uint16_t col = fire_col(t, f->fall);
        if (!col) continue;               // collapsed early, or fully out

        // Same ladder the rocks use: a ring nobody can resolve is a pixel, and
        // paying sixteen primitives to draw a pixel is how a prim budget goes.
        if (rpx < VG_LOD_DOT)  { vg_point((int)cx, (int)cy, col); continue; }
        if (rpx < VG_LOD_BLOB) { vg_diamond(cx, cy, rpx, col, 1); continue; }

        const int segs = rpx > 40.0f ? 16 : (rpx > 16.0f ? 12 : 8);
        fire_ring(cx, cy, rpx, segs, col);
        // The core, held earlier in the ramp so it is always the hotter part.
        if (rpx > 10.0f) fire_ring(cx, cy, rpx * 0.55f, segs > 12 ? 12 : 8,
                                   fire_col(t * 0.55f, f->fall));
        if (rpx > 26.0f) fire_ring(cx, cy, rpx * 0.28f, 8,
                                   fire_col(t * 0.30f, f->fall));
    }
}

// ---------------------------------------------------------------------------
// Hidden-line solids
//
// Fill every front face in the background colour, then stroke those faces' edges
// over the top. The fills hide the model's own back edges AND whatever is behind
// it, so it reads as a solid body while staying pure vector.
//
// Back-face culling is what makes this cheap: the models are near-convex, so
// front faces never overlap each other on screen and no per-face depth sort is
// needed. It also halves both the fill and the edge work.
//
// All fills must precede all edges, or a neighbouring face's fill erases the
// shared edge just drawn.
// ---------------------------------------------------------------------------

static void draw_asteroid(const VgCam& cam, const Asteroid* a) {
    // vg_screen_size does the turn, the reject, the clamp and the divide, in
    // that order and in this camera's terms. It matters here for both of the
    // reasons that function states: rocks astern are the whole job of the
    // mirror, and the mirror's focal length is a third of the window's, so its
    // rocks should drop to the cheap forms sooner -- which is exactly right for
    // a 145 pixel instrument.
    VgSpan sp;
    if (!vg_screen_size(cam, a->pos, a->radius, a->radius, &sp)) return;

    const float z   = sp.z;
    const float rpx = sp.rpx;

    const float fade = vg_fade(z, 1.25f, 420.0f, 0.22f);

    // Within ~200 units: close enough to be an immediate hazard at any throttle.
    uint16_t col = (vlen2(a->pos) < 40000.0f) ? COL_WARN : vg_dim(COL_AST, fade);
    const float cx = sp.cx, cy = sp.cy;

    if (rpx < VG_LOD_DOT) {
        if (sp.vis) vg_point((int)cx, (int)cy, col);
        return;
    }
    if (rpx < VG_LOD_BLOB) {
        // Too small for the wireframe to resolve; a diamond stays readable and
        // costs 4 lines instead of 30.
        if (!sp.vis) return;
        vg_diamond(cx, cy, rpx, col, 1);
        return;
    }

    const AstModel* M = &vg_models[a->model];
    Mat3 S = mat3_euler(a->spin[0], a->spin[1], a->spin[2]);

    Vec3 wv[AST_VERTS];
    for (int i = 0; i < AST_VERTS; i++)
        wv[i] = vadd(a->pos, vmul(mat3_apply(S, M->v[i]), a->radius));

    bool front[AST_FACES];
    for (int f = 0; f < M->face_count; f++) {
        Vec3 A = vg_view(cam, wv[M->f[f][0]]),
             B = vg_view(cam, wv[M->f[f][1]]),
             C = vg_view(cam, wv[M->f[f][2]]);
        // Eye is the origin, so the view vector to the face is just A.
        front[f] = (vdot(vcross(vsub(B, A), vsub(C, A)), A) < 0.0f);
    }

    // The fills exist to occlude -- the rock's own back edges, and the grid
    // behind it. Below ~16 pixels there is nothing left to occlude that the eye
    // can resolve, and the fills are where the triangles come from: sixteen
    // rocks were submitting up to 285 triangles a frame, 1.9ms of band time, of
    // which the readable near ones were a handful. Measured before touched.
    // 28, NOT 16. The first threshold was chosen from what seemed small and
    // the validation flight showed it excluded almost nothing: rocks in this
    // game live close, and nearly all of them sat above 16px. T and the fill
    // time did not move. At 28 the far half of the field goes wireframe.
    // 40: the second revision. 28 still left T near 240, because the course's
    // rocks genuinely live close -- the spawner aims them at the player.
    const bool fills = (rpx >= 40.0f);

    for (int f = 0; fills && f < M->face_count; f++) {
        if (!front[f]) continue;
        Vec3 A = wv[M->f[f][0]], B = wv[M->f[f][1]], C = wv[M->f[f][2]];
        // Faces straddling the near plane would need polygon clipping; skip their
        // fill instead. Only happens when the rock is on top of you, which is
        // already a collision. The edges still clip properly.
        if (A.z < NEAR_Z || B.z < NEAR_Z || C.z < NEAR_Z) continue;
        float ax, ay, bx, by, cx2, cy2;
        if (!vg_project(cam, A, &ax, &ay))   continue;
        if (!vg_project(cam, B, &bx, &by))   continue;
        if (!vg_project(cam, C, &cx2, &cy2)) continue;
        vg_tri(ax, ay, bx, by, cx2, cy2, COL_BLACK);
    }

    // Shared edges get submitted twice (once per adjoining front face); a
    // duplicate line is invisible and costs less than building an edge-face map.
    //
    // Antialiased only when the rock is big enough to hold still in the eye.
    // The same argument that stripped AA from the arena grid: below ~30px a
    // rock is moving structure at distance, no step survives long enough to
    // see, and an AA span costs an order of magnitude per pixel. The sixteen
    // rocks' 480 edges were most of the frame's AA bill.
    // No antialiasing at ANY size, and this is the third revision of this
    // line: 30px excluded nothing, 48px still left the frame's aa cost at
    // 1.2ms, because the rocks that survive a gate are exactly the close ones
    // whose edges are long -- and an AA span bills by the pixel, so the gate
    // keeps the expensive members and discards the cheap ones. The arena
    // grid's argument applies at every size here: a rock tumbles continuously,
    // no stair-step survives long enough to see.
    vg_line_aa_mode(false);
    for (int f = 0; f < M->face_count; f++) {
        if (!front[f]) continue;
        Vec3 A = wv[M->f[f][0]], B = wv[M->f[f][1]], C = wv[M->f[f][2]];
        vg_edge(cam, A, B, col);
        vg_edge(cam, B, C, col);
        vg_edge(cam, C, A, col);
    }
}

// THE HULL, AND ONLY THE HULL.
//
// Lifted out of draw_enemy so the ship-select screen can turn a model without
// inheriting a fighter's exhaust, its depth fade, its contrail or its
// distance-based degradation to a single point -- none of which a menu wants and
// all of which read off a live Ship the menu does not have.
//
// Takes an orientation rather than a Ship, because the two callers get theirs
// from different places: a fighter's comes from its fwd/up/roll basis, and the
// menu's from a clock.
//
// Hidden-line the same way the fighter is: cull to front faces, fill them black
// so the far side cannot show through, then stroke the edges over the top.
void vg_draw_hull(const VgCam& cam, const Mat3& orient, Vec3 pos, float scale,
                  uint16_t col) {
    Vec3 wv[SHIP_VERTS];
    for (int i = 0; i < SHIP_VERTS; i++)
        wv[i] = vadd(pos, vmul(mat3_apply(orient, vg_ship_verts[i]), scale));

    bool front[SHIP_FACES];
    for (int f = 0; f < SHIP_FACES; f++) {
        Vec3 A  = vg_view(cam, wv[vg_ship_faces[f][0]]);
        Vec3 Bv = vg_view(cam, wv[vg_ship_faces[f][1]]);
        Vec3 C  = vg_view(cam, wv[vg_ship_faces[f][2]]);
        front[f] = (vdot(vcross(vsub(Bv, A), vsub(C, A)), A) < 0.0f);
    }

    for (int f = 0; f < SHIP_FACES; f++) {
        if (!front[f]) continue;
        Vec3 A  = wv[vg_ship_faces[f][0]];
        Vec3 Bv = wv[vg_ship_faces[f][1]];
        Vec3 C  = wv[vg_ship_faces[f][2]];
        if (A.z < NEAR_Z || Bv.z < NEAR_Z || C.z < NEAR_Z) continue;
        float ax, ay, bx, by, cx2, cy2;
        if (!vg_project(cam, A,  &ax,  &ay))  continue;
        if (!vg_project(cam, Bv, &bx,  &by))  continue;
        if (!vg_project(cam, C,  &cx2, &cy2)) continue;
        vg_tri(ax, ay, bx, by, cx2, cy2, COL_BLACK);
    }

    for (int f = 0; f < SHIP_FACES; f++) {
        if (!front[f]) continue;
        Vec3 A  = wv[vg_ship_faces[f][0]];
        Vec3 Bv = wv[vg_ship_faces[f][1]];
        Vec3 C  = wv[vg_ship_faces[f][2]];
        vg_edge(cam, A,  Bv, col);
        vg_edge(cam, Bv, C,  col);
        vg_edge(cam, C,  A,  col);
    }

    // The fin is a flat blade with no volume, so it has no facing to cull --
    // just stroke it over the hull.
    for (int e = 0; e < SHIP_FIN_EDGES; e++)
        vg_edge(cam, wv[vg_ship_fin[e][0]], wv[vg_ship_fin[e][1]], col);
}

// `hero` marks the cutscene ship: drawn on the amber ramp rather than in threat
// red, and faded over a far longer range because it is meant to be looked at
// from a distance the combat curve would have written off as a contact.
static void draw_enemy(const VgCam& cam, const Ship* s, bool hero = false) {
    // Both of these MUST use the ship's own scale, not the combat constant.
    // Measuring a 128-unit cutscene model as if it were a 7-unit fighter put it
    // under the two-pixel threshold at around z=1400 and switched it to the
    // single-point path -- so it appeared, flew a little way, and became one
    // invisible dot while its trail carried on streaking across the screen.
    // This function was missed when draw_asteroid was fixed, and the cost was
    // total: raw pos.z rejects everything behind the player, and behind the
    // player is the entire job of the rear-view patch, so ENEMIES NEVER
    // APPEARED IN THE MIRROR AT ALL. The one contact worth craning round for
    // was the one thing the instrument could not show. Going through
    // vg_screen_size is what stops a third site drifting the same way.
    //
    // The reach is three scale units because that is how far the hull actually
    // extends; the measure is one, because every gate below is written against
    // the ship's scale rather than its bounding radius.
    VgSpan sp;
    if (!vg_screen_size(cam, s->pos, s->scale, s->scale * 3.0f, &sp)) return;

    const float z    = sp.z;
    const float rpx  = sp.rpx;
    const float fade = hero ? vg_fade(z, 1.30f, 3400.0f, 0.35f)
                            : vg_fade(z, 1.3f,   700.0f, 0.35f);

    uint16_t col = hero ? vg_dim(INK_BRIGHT, fade)
                        : ((s->hit_flash > 0) ? COL_ENEMY_HIT
                                              : vg_dim(COL_ENEMY, fade));

    // 2.0 and not VG_LOD_DOT, and no diamond tier. See that constant: a hull is
    // worth reading at any size, and this threshold has already cost one model
    // its existence once.
    if (rpx < 2.0f) {
        if (sp.vis) vg_point((int)sp.cx, (int)sp.cy, COL_ENEMY);
        return;
    }

    // The hull is shared with the select screen -- see vg_draw_hull. What stays
    // here is everything that belongs to a ship that is FLYING: the exhaust below,
    // and the fade and colour worked out above. The basis is held rather than
    // asked for twice, because the exhaust hangs off the same one.
    const Mat3 B = vg_ship_basis(s);
    vg_draw_hull(cam, B, s->pos, s->scale, col);

    // Exhaust: length tracks throttle, so you can read their energy state.
    float t = (s->speed - s->spec->speed_min)
            / (s->spec->speed_max - s->spec->speed_min);
    if (t < 0) t = 0; else if (t > 1) t = 1;
    Vec3 tail  = vadd(s->pos, vmul(mat3_apply(B, v3(0, 0, -1.1f)), s->scale));
    Vec3 flame = vadd(s->pos, vmul(mat3_apply(B, v3(0, 0, -1.1f - 1.6f * t)), s->scale));
    vg_edge(cam, tail, flame, vg_dim(COL_EXHAUST, 0.4f + 0.6f * t));
}

// A ship's ribbon: where it has been, in the hue that identifies it.
//
// This is the whole justification for hue existing in a single-colour interface.
// At range a fighter is four pixels of wireframe and every one of them looks
// alike; its trail is a long stroke in a colour you can name, so colour answers
// "who is that" before the shape ever resolves.
//
// Drawn newest-first with a quadratic fade, and stroked thick only at the head:
// the near end is the part that carries the ship's current heading, and the tail
// only needs to say where it came from.
static void draw_ship_trail(const VgCam& cam, const ShipTrailRing& ring,
                            Vec3 from, float hue) {
    const int n    = ring.n;
    const int head = ring.head;
    if (n < 2) return;
    // ADDITIVE, AND IT USED TO BE OPAQUE. That is the whole of the black-trail bug.
    //
    // The fade multiplies the pilot's colour toward BLACK -- SHIP_TRAIL_MIN is 0.09, so
    // most of the ribbon is 9% of a hue. Written opaquely over a black starfield that
    // reads as a trail dimming into the distance, which is what it was tuned against.
    // Over a bright nebula the same pixels are DARKER than what they replace, so the
    // ribbon reads as a black line drawn across the sky. Reported from a playtest.
    //
    // Additive fixes it without changing what it looks like where it already worked: over
    // black, dst is 0, so dst+src IS src and every pixel is identical to before. Over a
    // bright sky the same value now adds light instead of replacing it, so the colour
    // survives whatever is behind it -- which it has to, because hue is IDENTITY here and
    // identity is the one cue that cannot be allowed to depend on the venue.
    //
    // It also makes the fade mean the right thing. A contrail emits light; it does not
    // paint the sky darker. The old code had the fade standing for "how black this line
    // is", which only ever agreed with the physics because the sky was black too.
    //
    // Antialiasing is still off, and this is how: the mode and the blend share one field
    // in the slice, so ADD replaces the OPAQUE this used to set rather than adding to it.
    // Every segment is dim and moving fast enough that no step survives to be seen.
    vg_line_blend(VG_LINE_ADD);
    const uint16_t col = vg_hue_col(hue);
    Vec3 prev = from;
    for (int t = 0; t < n; ) {
        int   idx = (head - t + SHIP_TRAIL * 2) % SHIP_TRAIL;
        Vec3  cur = ring.pt[idx];

        // Age fade, scaled by the throttle this point was laid down at, so a
        // burst of speed stays lit in the tail long after the ship has backed
        // off. Clamped rather than skipped: every segment is drawn, so the
        // ribbon is unbroken from the engine to the far end no matter how the
        // throttle was worked.
        float age = 1.0f - (float)t / (float)n;
        float pw  = SHIP_TRAIL_IDLE +
                    (1.0f - SHIP_TRAIL_IDLE) * ((float)ring.p[idx] * (1.0f / 255.0f));
        float f   = age * age * pw;
        if (f < SHIP_TRAIL_MIN) f = SHIP_TRAIL_MIN;

        // Single-width throughout. A thick stroke is not one primitive drawn
        // fatter -- vg_line_w emits a separate offset copy per pixel of width --
        // so the bright head of every ribbon was costing double, and submit
        // bills per primitive whether or not the rasteriser ever hides. With
        // three ships trailing, that head was several hundred primitives.
        // Wide at the near end only -- see SHIP_TRAIL_WIDE. A one-pixel diagonal moving
        // this fast is a dotted line, and the near end is the part being read.
        vg_edge_w(cam, prev, cur, vg_dim(col, f), (age > SHIP_TRAIL_WIDE) ? 2 : 1);
        prev = cur;

        // Level of detail along the ribbon: full resolution near the engine
        // where the curve is being read, coarser further back where it is
        // dimmer and usually farther away. This is what lets the trail be
        // nearly twice as long without nearly twice the primitives, and it
        // stays continuous because each segment starts where the last ended.
        t += (t < 28) ? 1 : (t < 72) ? 2 : 3;
    }
    // Put back, because the blend lives in the slice and everything after this expects
    // opaque -- the hulls go down next and a hull is a solid object, not a light.
    vg_line_blend(0);
}

// The entry gate: a lit plane in the pilot's colour, square to their travel,
// that the cutscene ship comes through. Filled rather than outlined, because
// the whole point is a bright surface a solid object emerges from -- an outline
// would read as a frame around empty space.
//
// This is the one place a filled quad appears in a renderer that is otherwise
// all edges and hidden-line fills, and it earns it: hue is identity here, so
// the plane announces WHO is arriving before the ship is resolvable at all.
static void draw_gate(const VgCam& cam) {
    if (vg_cine.gate_t <= 0.0f) return;

    const float e = GATE_TIME - vg_cine.gate_t;             // seconds since it opened

    // Wipes UP from its own bottom edge, holds, then wipes back down the same
    // way. The bottom edge never moves, so it reads as a surface being drawn
    // into existence rather than a rectangle being scaled -- which is the whole
    // difference between a materialisation and a pop.
    float open = 1.0f;
    if (e < GATE_SWIPE)                     open = e / GATE_SWIPE;
    else if (e > GATE_TIME - GATE_SWIPE)    open = (GATE_TIME - e) / GATE_SWIPE;
    if (open < 0.0f) open = 0.0f;
    if (open > 1.0f) open = 1.0f;

    // Brightness rides the wipe, so it fades in as it grows and out as it
    // retracts. Sells the leading edge as energy rather than as a moving line.
    const float fade = open;

    const float hw = GATE_SIZE;                        // half width, fixed
    const float hh = GATE_SIZE * 0.72f;                // half height, full open
    const Vec3  rr = vmul(vg_cine.gate_r, hw);

    // Bottom edge pinned; top edge climbs from it.
    const Vec3 lo = vmul(vg_cine.gate_u, -hh);
    const Vec3 hi = vmul(vg_cine.gate_u, -hh + 2.0f * hh * open);

    const Vec3 c[4] = {
        vadd(vg_cine.gate_pos, vadd(vmul(rr, -1.0f), lo)),
        vadd(vg_cine.gate_pos, vadd(rr,              lo)),
        vadd(vg_cine.gate_pos, vadd(rr,              hi)),
        vadd(vg_cine.gate_pos, vadd(vmul(rr, -1.0f), hi)),
    };

    float sx[4], sy[4];
    for (int i = 0; i < 4; i++)
        if (!vg_project(cam, vg_view(cam, c[i]), &sx[i], &sy[i])) return;

    const uint16_t col = vg_dim(vg_hue_col(vg_cine.gate_hue), fade * 0.85f);
    vg_tri(sx[0], sy[0], sx[1], sy[1], sx[2], sy[2], col);
    vg_tri(sx[0], sy[0], sx[2], sy[2], sx[3], sy[3], col);

    // Hot leading edge along the top, dimmer rim elsewhere. The bright line is
    // what the eye follows while the plane is wiping.
    const uint16_t rim  = vg_dim(INK, fade * 0.8f);
    vg_line_w(sx[0], sy[0], sx[1], sy[1], rim, 2);
    vg_line_w(sx[1], sy[1], sx[2], sy[2], rim, 2);
    vg_line_w(sx[3], sy[3], sx[0], sy[0], rim, 2);
    vg_line_w(sx[2], sy[2], sx[3], sy[3], vg_dim(INK_MAX, fade), 3);
}

static void draw_missile(const VgCam& cam, const Missile* m) {
    const bool friendly = m->from_player;

    // Your rounds fly in your colour. Hue is identity here and a missile you
    // launched is yours, so the same stripe that marks you on the bracket is
    // what streaks away from the rail. A broken seeker drops to the dead tone
    // regardless -- that is state, not identity, and it matters more than
    // whose missile it was.
    const uint16_t mine = vg_hue_col(vg.trail_hue);

    uint16_t trail_col = !m->locked ? COL_TRAIL_DEAD
                       : friendly   ? vg_dim(mine, 0.75f)
                                    : COL_TRAIL_HOSTILE;
    uint16_t head_col  = !m->locked ? COL_STAR_MID
                       : friendly   ? mine
                                    : COL_MSL_HOSTILE;

    // Trail, newest first, fading toward the tail. This is the arc, and it is the
    // thing the player is meant to read, so it is stroked thick at the head and
    // tapered to a single pixel at the tail rather than drawn as a hairline.
    // Same reasoning as the ship ribbons, and more so: a 3px-wide stroke is
    // already three primitives per segment, so this is the densest geometry in
    // the frame and the least able to show a stair-step.
    // Additive for the same reason the ship ribbon is -- see draw_ship_trail. This one
    // fades as f*f, so its tail goes darker still, and an incoming missile is the least
    // affordable thing in the game to lose against a bright sky.
    vg_line_blend(VG_LINE_ADD);
    Vec3 prev = m->pos;
    for (int t = 0; t < m->trail.n; ) {
        int idx = (m->trail.head - t + MISSILE_TRAIL * 2) % MISSILE_TRAIL;
        Vec3 cur = m->trail.pt[idx];
        float f = 1.0f - (float)t / (float)m->trail.n;
        // Was 3 at the head. Three offset copies per segment on every missile in
        // the air is the densest geometry in the frame, and it buys very little
        // on a stroke that is already the brightest thing on screen.
        int w = (f > 0.66f) ? 2 : 1;
        vg_edge_w(cam, prev, cur, vg_dim(trail_col, f * f), w);
        prev = cur;
        // See MISSILE_LOD_NEAR. The wide part of the stroke is the near part, so the
        // segments that cost two primitives are exactly the ones still drawn one for one.
        t += (t < MISSILE_LOD_NEAR) ? 1 : (t < MISSILE_LOD_MID) ? 2 : 3;
    }
    // Back to opaque before the body below. The body is drawn at FULL brightness, and a
    // full-brightness opaque stroke is legible over anything -- it is only the dimmed
    // geometry that needed the blend. Additive there would let a bright sky wash it out.
    vg_line_blend(0);

    // Body: a short, fat, bright segment along the heading. Screen-space width is
    // constant with range, which is deliberate -- a missile inbound from distance
    // still has to be impossible to miss.
    Vec3 nose = vadd(m->pos, vmul(m->dir, 3.4f));
    Vec3 tail = vsub(m->pos, vmul(m->dir, 3.4f));
    vg_edge_w(cam, tail, nose, head_col, 4);
}

// THE CUTSCENE IS A CLOSED SET. Nothing that lives in the arena is drawn during it.
//
// vg_spawn_opponent runs at MATCH SETUP, which is before the launch cutscene, so the real
// opponent is alive and flying the torus for the whole of it -- and nothing stopped it being
// drawn. So the introductions had a second ship wandering through them: in the player's shot
// it was the opponent in the opponent's colour, and in the OPPONENT's shot it was a second
// ship in the same colour sitting roughly where the player's had been.
//
// Reported as a colour bug -- "both ships use the npc's colour" -- and the colours were never
// wrong. There were simply two of them and one had no business being on set.
//
// The cutscene's own ship is deliberately kept out of vg.enemy so that neither the AI nor
// the collision pass can see it. This is the other half of that separation: the arena must
// not be visible from the cutscene either.
static inline bool cine_set(void) {
    return vg.state == VG_INTRO;
}

void vg_draw_world(const VgCam& cam) {
    // COUNTED FOR THE MAIN VIEW ONLY, because this function runs TWICE a frame.
    //
    // The rear-view repeater renders the world again with cam.lite set, and lite skips the
    // trails. Assigning these unconditionally meant the mirror's pass overwrote the real
    // one, so `trails` read 0 in the middle of a dogfight and three quarters of `world`
    // fell into the unnamed remainder. The mirror has its own counter already -- see
    // vg_render_mirror_us -- so this half of the split belongs to the main view alone.
    //
    // g_sub_world brackets the main call only, so these must too, or the parts would not
    // add up to the whole they are a split of.
    const bool bill = !cam.lite;
    uint32_t t_w = micros();
    draw_motes(cam);
    draw_debris(cam);
    if (bill) { g_w_motes = micros() - t_w; } t_w = micros();

    // Painter order: farthest rock first, so a nearer one's black fills occlude
    // it. Within a single rock, back-face culling already sorts things out.
    int order[MAX_ASTEROIDS];
    int nast = 0;
    for (int i = 0; i < MAX_ASTEROIDS; i++)
        if (vg.ast[i].alive) order[nast++] = i;

    for (int i = 1; i < nast; i++) {              // insertion sort, descending z
        int   key = order[i];
        float kz  = vg.ast[key].pos.z;
        int   j   = i - 1;
        while (j >= 0 && vg.ast[order[j]].pos.z < kz) { order[j + 1] = order[j]; j--; }
        order[j + 1] = key;
    }
    for (int i = 0; i < nast; i++) draw_asteroid(cam, &vg.ast[order[i]]);
    if (bill) { g_w_rocks = micros() - t_w; } t_w = micros();

    // Ships after the rocks and never occluded by them: losing the bandit behind
    // scenery in a dogfight is worse than the small inconsistency.
    // Trails go down before the hulls so a ribbon passing in front of its own
    // ship does not draw over the thing it belongs to.
    if (!cam.lite) {
        if (!cine_set())
        for (int i = 0; i < MAX_ENEMIES; i++) {
            const Ship* s = &vg.enemy[i];
            if (s->alive)
                draw_ship_trail(cam, s->trail, s->pos, s->hue);
        }
        // The player's own, streaming from the origin. Invisible dead ahead, but
        // a hard turn sweeps it into view -- so you can see the arc you just
        // flew. In the repeater it is dead astern by definition, so it is a
        // permanent bright smear straight down the middle of the one instrument
        // meant to show what is behind you.
        draw_ship_trail(cam, vg_trail, v3(0, 0, 0), vg.trail_hue);
    }

    // The trails end here; the gate and the hulls are the next span.
    if (bill) { g_w_trails = micros() - t_w; } t_w = micros();

    // Gate first: the ship comes THROUGH it, so it has to be behind.
    draw_gate(cam);

    // Nobody is flying this one: the fighter crossing the view during the launch
    // cutscene, or the player's own wreck after a death.
    if (vg_cine.on) {
        draw_ship_trail(cam, vg_cine.ship.trail,
                        vg_cine.ship.pos, vg_cine.ship.hue);
    }

    if (!cine_set())
    for (int i = 0; i < MAX_ENEMIES; i++)
        if (vg.enemy[i].alive) draw_enemy(cam, &vg.enemy[i]);

    // Amber, not threat red. In a cutscene the pair are being introduced, not
    // engaged, and one of them is the player's own ship -- painting it in the
    // colour reserved for hostiles said the wrong thing about both.
    if (vg_cine.on) draw_enemy(cam, &vg_cine.ship, true);

    if (bill) { g_w_ships = micros() - t_w; } t_w = micros();

    for (int i = 0; i < MAX_MISSILES; i++)
        if (vg.msl[i].alive) draw_missile(cam, &vg.msl[i]);
    // SPLIT FROM THE FIREBALLS, because `ord` held both and adding level of detail to the
    // missile trail moved it by nothing at all. One number for two unrelated kinds of work
    // cannot say which of them is the 1891.
    if (bill) { g_w_msl = micros() - t_w; } t_w = micros();

    // Last, over everything. A fireball is light, and light is in front of the
    // wreckage it came from -- drawn under the ships, the brightest thing in the
    // frame would be the one thing getting occluded by black hull fills.
    draw_fireballs(cam);
    if (bill) { g_w_fire = micros() - t_w; }
}
