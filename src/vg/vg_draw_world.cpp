#include "vg_draw.h"
#include "vg_game.h"
#include <math.h>

// Everything with a position in the world: stars, dust, rocks, fighters,
// missiles, wreckage.

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

// Near-field dust streaking past. Stars sit at infinity and only rotate, so they
// convey no sense of speed whatsoever, and the asteroid field is far too sparse
// to. These are what make the throttle *feel* like a throttle.
//
// Each mote trails along +z: in view space the world slides toward -z, so a
// mote's tail lies further away than its head, and the streaks therefore splay
// outward from the vanishing point exactly as they should.
static void draw_motes(const VgCam& cam) {
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

    // ASTERN THE DUST IS LEAVING, not arriving. The geometry already says so --
    // a streak is laid along +z in the camera's own frame, so looking forward it
    // runs toward the vanishing point ahead and looking aft it runs toward the
    // one behind, and motes converge on the middle of the mirror instead of
    // bursting out of it.
    //
    // What the geometry does not say is that this is the wake. Longer and
    // fainter is the difference between dust coming at you and dust being left
    // behind, and it is the whole reason to look: at full throttle the mirror
    // fills with something streaming away from the ship.
    if (cam.rear) {
        streak *= 1.7f;
        lvl    *= 0.72f;
        w       = 1;
    }
    const uint16_t col = vg_dim(COL_MOTE, lvl);

    // Dust, and only ever drawn at speed -- which makes it part of the exact
    // condition where the frame is tightest. Every mote is a short streak, two
    // primitives once it thickens, and there is no such thing as a visible
    // stair-step on a two-pixel speck moving at 460 units a second.
    vg_line_aa_mode(false);
    // THE FIELD IS NOT TURNED, and that is not a shortcut. Motes spawn between
    // z 450 and 950 ahead and are recycled at MOTE_CULL_Z, fifty units after
    // they pass -- so there is no dust behind the ship at all, and a turned
    // camera correctly showed an empty mirror.
    //
    // Dust has no identity. It is a uniform random field with nothing in it to
    // recognise, so the field behind is the field ahead, and reflecting it is
    // undetectable. Drawing it untouched gives the mirror a full field for no
    // extra motes, no extra primitives and no change to the forward view.
    //
    // The MOTION still comes out right, which is the part worth checking: the
    // streak runs along +z in this frame, so it always runs to the vanishing
    // point and the motes converge on the middle of the mirror -- which is what
    // dust being left behind does.
    // WHICH WAY THE STREAK POINTS is the whole difference between the two
    // views, and it is one sign.
    //
    // A streak is laid from the mote along z, and z is depth, so laying it
    // AWAY puts the tail nearer the vanishing point and laying it TOWARD puts
    // the tail out at the edge. Ahead, the tail belongs inward: dust sweeps out
    // past you and what it leaves behind points back at the middle. Astern it
    // belongs outward, and drawn the other way the mirror read as flying INTO
    // something rather than away from it.
    const float sdir = cam.rear ? -streak : streak;

    VgCam fwd = cam;
    fwd.rear = false;
    // Every third mote in the mirror: dust is a density, not an inventory, and
    // 160 motes at up to two primitives each was the mirror's single biggest
    // submission after the grid.
    const int stride = cam.lite ? 3 : 1;
    for (int i = 0; i < NUM_MOTES; i += stride) {
        Vec3 p = vg.mote[i];
        if (p.z < NEAR_Z) continue;
        // Toward the camera the tail can cross the near plane, where the
        // projection is meaningless. Stop it short instead of clipping: a
        // shortened streak on the closest motes is invisible, a wild one is not.
        float tz = p.z + sdir;
        if (tz < NEAR_Z) tz = NEAR_Z;
        vg_edge_w(fwd, p, v3(p.x, p.y, tz), col, w);
    }
    vg_line_aa_mode(true);
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
    vg_line_aa_mode(true);
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
    vg_line_aa_mode(true);
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

    Mat3 B = vg_ship_basis(s);
    Vec3 wv[SHIP_VERTS];
    for (int i = 0; i < SHIP_VERTS; i++)
        wv[i] = vadd(s->pos, vmul(mat3_apply(B, vg_ship_verts[i]), s->scale));

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
static void draw_ship_trail(const VgCam& cam, const Vec3* trail,
                            const uint8_t* power, int n, int head,
                            Vec3 from, float hue) {
    if (n < 2) return;
    // Trails are the single largest primitive source in a fight and the one
    // place antialiasing buys nothing: every segment is dim, one or two pixels
    // wide, and moving fast enough that no step survives long enough to see.
    vg_line_aa_mode(false);
    const uint16_t col = vg_hue_col(hue);
    Vec3 prev = from;
    for (int t = 0; t < n; ) {
        int   idx = (head - t + SHIP_TRAIL * 2) % SHIP_TRAIL;
        Vec3  cur = trail[idx];

        // Age fade, scaled by the throttle this point was laid down at, so a
        // burst of speed stays lit in the tail long after the ship has backed
        // off. Clamped rather than skipped: every segment is drawn, so the
        // ribbon is unbroken from the engine to the far end no matter how the
        // throttle was worked.
        float age = 1.0f - (float)t / (float)n;
        float pw  = SHIP_TRAIL_IDLE +
                    (1.0f - SHIP_TRAIL_IDLE) * ((float)power[idx] * (1.0f / 255.0f));
        float f   = age * age * pw;
        if (f < SHIP_TRAIL_MIN) f = SHIP_TRAIL_MIN;

        // Single-width throughout. A thick stroke is not one primitive drawn
        // fatter -- vg_line_w emits a separate offset copy per pixel of width --
        // so the bright head of every ribbon was costing double, and submit
        // bills per primitive whether or not the rasteriser ever hides. With
        // three ships trailing, that head was several hundred primitives.
        vg_edge_w(cam, prev, cur, vg_dim(col, f), 1);
        prev = cur;

        // Level of detail along the ribbon: full resolution near the engine
        // where the curve is being read, coarser further back where it is
        // dimmer and usually farther away. This is what lets the trail be
        // nearly twice as long without nearly twice the primitives, and it
        // stays continuous because each segment starts where the last ended.
        t += (t < 28) ? 1 : (t < 72) ? 2 : 3;
    }
    vg_line_aa_mode(true);
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
    if (vg.gate_t <= 0.0f) return;

    const float e = GATE_TIME - vg.gate_t;             // seconds since it opened

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
    const Vec3  rr = vmul(vg.gate_r, hw);

    // Bottom edge pinned; top edge climbs from it.
    const Vec3 lo = vmul(vg.gate_u, -hh);
    const Vec3 hi = vmul(vg.gate_u, -hh + 2.0f * hh * open);

    const Vec3 c[4] = {
        vadd(vg.gate_pos, vadd(vmul(rr, -1.0f), lo)),
        vadd(vg.gate_pos, vadd(rr,              lo)),
        vadd(vg.gate_pos, vadd(rr,              hi)),
        vadd(vg.gate_pos, vadd(vmul(rr, -1.0f), hi)),
    };

    float sx[4], sy[4];
    for (int i = 0; i < 4; i++)
        if (!vg_project(cam, vg_view(cam, c[i]), &sx[i], &sy[i])) return;

    const uint16_t col = vg_dim(vg_hue_col(vg.gate_hue), fade * 0.85f);
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
    vg_line_aa_mode(false);
    Vec3 prev = m->pos;
    for (int t = 0; t < m->trail_n; t++) {
        int idx = (m->trail_head - t + MISSILE_TRAIL * 2) % MISSILE_TRAIL;
        Vec3 cur = m->trail[idx];
        float f = 1.0f - (float)t / (float)m->trail_n;
        // Was 3 at the head. Three offset copies per segment on every missile in
        // the air is the densest geometry in the frame, and it buys very little
        // on a stroke that is already the brightest thing on screen.
        int w = (f > 0.66f) ? 2 : 1;
        vg_edge_w(cam, prev, cur, vg_dim(trail_col, f * f), w);
        prev = cur;
    }
    vg_line_aa_mode(true);

    // Body: a short, fat, bright segment along the heading. Screen-space width is
    // constant with range, which is deliberate -- a missile inbound from distance
    // still has to be impossible to miss.
    Vec3 nose = vadd(m->pos, vmul(m->dir, 3.4f));
    Vec3 tail = vsub(m->pos, vmul(m->dir, 3.4f));
    vg_edge_w(cam, tail, nose, head_col, 4);
}

void vg_draw_world(const VgCam& cam) {
    draw_motes(cam);
    draw_debris(cam);

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

    // Ships after the rocks and never occluded by them: losing the bandit behind
    // scenery in a dogfight is worse than the small inconsistency.
    // Trails go down before the hulls so a ribbon passing in front of its own
    // ship does not draw over the thing it belongs to.
    if (!cam.lite) {
        for (int i = 0; i < MAX_ENEMIES; i++) {
            const Ship* s = &vg.enemy[i];
            if (s->alive)
                draw_ship_trail(cam, s->trail, s->trail_p, s->trail_n,
                                s->trail_head, s->pos, s->hue);
        }
        // The player's own, streaming from the origin. Invisible dead ahead, but
        // a hard turn sweeps it into view -- so you can see the arc you just
        // flew. In the repeater it is dead astern by definition, so it is a
        // permanent bright smear straight down the middle of the one instrument
        // meant to show what is behind you.
        draw_ship_trail(cam, vg.trail, vg.trail_p, vg.trail_n, vg.trail_head,
                        v3(0, 0, 0), vg.trail_hue);
    }

    // Gate first: the ship comes THROUGH it, so it has to be behind.
    draw_gate(cam);

    // Nobody is flying this one: the fighter crossing the view during the launch
    // cutscene, or the player's own wreck after a death.
    if (vg.cine_on) {
        draw_ship_trail(cam, vg.cine.trail, vg.cine.trail_p,
                        vg.cine.trail_n, vg.cine.trail_head,
                        vg.cine.pos, vg.cine.hue);
    }

    for (int i = 0; i < MAX_ENEMIES; i++)
        if (vg.enemy[i].alive) draw_enemy(cam, &vg.enemy[i]);

    // Amber, not threat red. In a cutscene the pair are being introduced, not
    // engaged, and one of them is the player's own ship -- painting it in the
    // colour reserved for hostiles said the wrong thing about both.
    if (vg.cine_on) draw_enemy(cam, &vg.cine, true);

    for (int i = 0; i < MAX_MISSILES; i++)
        if (vg.msl[i].alive) draw_missile(cam, &vg.msl[i]);

    // Last, over everything. A fireball is light, and light is in front of the
    // wreckage it came from -- drawn under the ships, the brightest thing in the
    // frame would be the one thing getting occluded by black hull fills.
    draw_fireballs(cam);
}
