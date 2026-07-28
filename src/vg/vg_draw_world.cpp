#include "vg_draw.h"
#include "vg_game.h"
#include <math.h>

// Everything with a position in the world: stars, dust, rocks, fighters,
// missiles, wreckage.

void vg_draw_starfield(const VgCam& cam) {
    static const uint16_t shades[3] = { COL_STAR_DIM, COL_STAR_MID, COL_STAR_BRIGHT };
    for (int i = 0; i < NUM_STARS; i++) {
        Vec3 s = vg.star[i];
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
    float sn = (vg.speed - vg.spec->speed_min)
             / (vg.spec->speed_max - vg.spec->speed_min);
    if (sn <= MOTE_FADE_IN) return;

    float f = (sn - MOTE_FADE_IN) / (1.0f - MOTE_FADE_IN);
    if (f > 1.0f) f = 1.0f;

    // Super-linear streak growth, plus a thicker stroke and brighter tone at the
    // top end, so firewalling the throttle is a visibly different state rather
    // than just a bigger number.
    const float    boost  = 1.0f + MOTE_STREAK_BOOST * f * f;
    const float    streak = vg.speed * MOTE_STREAK_SEC * boost;
    const int      w      = (f > MOTE_THICK_AT) ? 2 : 1;
    const uint16_t col    = vg_dim(COL_MOTE, 0.14f + 0.86f * f);

    for (int i = 0; i < NUM_MOTES; i++) {
        Vec3 p = vg.mote[i];
        if (p.z < NEAR_Z) continue;
        vg_edge_w(cam, p, v3(p.x, p.y, p.z + streak), col, w);
    }
}

static void draw_debris(const VgCam& cam) {
    for (int i = 0; i < MAX_DEBRIS; i++) {
        const Debris* d = &vg.deb[i];
        if (!d->alive) continue;
        float f = d->life / d->life0;
        vg_edge(cam, d->pos, vadd(d->pos, d->seg), vg_dim(COL_DEBRIS, f));
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
    if (a->pos.z + a->radius < NEAR_Z) return;

    float z   = a->pos.z > NEAR_Z ? a->pos.z : NEAR_Z;
    float rpx = FOCAL * a->radius / z;

    float fade = 1.25f - z / 420.0f;
    if (fade > 1.0f)  fade = 1.0f;
    if (fade < 0.22f) fade = 0.22f;

    // Within ~200 units: close enough to be an immediate hazard at any throttle.
    uint16_t col = (vlen2(a->pos) < 40000.0f) ? COL_WARN : vg_dim(COL_AST, fade);
    float cx, cy;

    if (rpx < 2.5f) {
        if (vg_project(cam, a->pos, &cx, &cy)) vg_point((int)cx, (int)cy, col);
        return;
    }
    if (rpx < 7.0f) {
        // Too small for the wireframe to resolve; a diamond stays readable and
        // costs 4 lines instead of 30.
        if (!vg_project(cam, a->pos, &cx, &cy)) return;
        vg_line(cx - rpx, cy, cx, cy - rpx, col);
        vg_line(cx, cy - rpx, cx + rpx, cy, col);
        vg_line(cx + rpx, cy, cx, cy + rpx, col);
        vg_line(cx, cy + rpx, cx - rpx, cy, col);
        return;
    }

    const AstModel* M = &vg_models[a->model];
    Mat3 S = mat3_euler(a->spin[0], a->spin[1], a->spin[2]);

    Vec3 wv[AST_VERTS];
    for (int i = 0; i < AST_VERTS; i++)
        wv[i] = vadd(a->pos, vmul(mat3_apply(S, M->v[i]), a->radius));

    bool front[AST_FACES];
    for (int f = 0; f < M->face_count; f++) {
        Vec3 A = wv[M->f[f][0]], B = wv[M->f[f][1]], C = wv[M->f[f][2]];
        // Eye is the origin, so the view vector to the face is just A.
        front[f] = (vdot(vcross(vsub(B, A), vsub(C, A)), A) < 0.0f);
    }

    for (int f = 0; f < M->face_count; f++) {
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
    for (int f = 0; f < M->face_count; f++) {
        if (!front[f]) continue;
        Vec3 A = wv[M->f[f][0]], B = wv[M->f[f][1]], C = wv[M->f[f][2]];
        vg_edge(cam, A, B, col);
        vg_edge(cam, B, C, col);
        vg_edge(cam, C, A, col);
    }
}

static void draw_enemy(const VgCam& cam, const Ship* s) {
    if (s->pos.z + ENEMY_SCALE * 3.0f < NEAR_Z) return;

    float z    = s->pos.z > NEAR_Z ? s->pos.z : NEAR_Z;
    float rpx  = FOCAL * ENEMY_SCALE / z;
    float fade = 1.3f - z / 700.0f;
    if (fade > 1.0f)  fade = 1.0f;
    if (fade < 0.35f) fade = 0.35f;

    uint16_t col = (s->hit_flash > 0) ? COL_ENEMY_HIT : vg_dim(COL_ENEMY, fade);

    float cx, cy;
    if (rpx < 2.0f) {
        if (vg_project(cam, s->pos, &cx, &cy)) vg_point((int)cx, (int)cy, COL_ENEMY);
        return;
    }

    Mat3 B = vg_ship_basis(s);
    Vec3 wv[SHIP_VERTS];
    for (int i = 0; i < SHIP_VERTS; i++)
        wv[i] = vadd(s->pos, vmul(mat3_apply(B, vg_ship_verts[i]), ENEMY_SCALE));

    bool front[SHIP_FACES];
    for (int f = 0; f < SHIP_FACES; f++) {
        Vec3 A  = wv[vg_ship_faces[f][0]];
        Vec3 Bv = wv[vg_ship_faces[f][1]];
        Vec3 C  = wv[vg_ship_faces[f][2]];
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
    Vec3 tail  = vadd(s->pos, vmul(mat3_apply(B, v3(0, 0, -1.1f)), ENEMY_SCALE));
    Vec3 flame = vadd(s->pos, vmul(mat3_apply(B, v3(0, 0, -1.1f - 1.6f * t)), ENEMY_SCALE));
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

        vg_edge_w(cam, prev, cur, vg_dim(col, f), (f > 0.55f) ? 2 : 1);
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

static void draw_missile(const VgCam& cam, const Missile* m) {
    const bool friendly = m->from_player;

    uint16_t trail_col = !m->locked ? COL_TRAIL_DEAD
                       : friendly   ? COL_TRAIL_FRIEND
                                    : COL_TRAIL_HOSTILE;
    uint16_t head_col  = !m->locked ? COL_STAR_MID
                       : friendly   ? COL_MSL_FRIEND
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
        int w = (f > 0.66f) ? 3 : (f > 0.33f) ? 2 : 1;
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
    for (int i = 0; i < MAX_ENEMIES; i++) {
        const Ship* s = &vg.enemy[i];
        if (s->alive)
            draw_ship_trail(cam, s->trail, s->trail_p, s->trail_n, s->trail_head,
                            s->pos, s->hue);
    }
    // The player's own, streaming from the origin. Invisible dead ahead, but a
    // hard turn sweeps it into view -- so you can see the arc you just flew.
    draw_ship_trail(cam, vg.trail, vg.trail_p, vg.trail_n, vg.trail_head,
                    v3(0, 0, 0), vg.trail_hue);

    for (int i = 0; i < MAX_ENEMIES; i++)
        if (vg.enemy[i].alive) draw_enemy(cam, &vg.enemy[i]);

    for (int i = 0; i < MAX_MISSILES; i++)
        if (vg.msl[i].alive) draw_missile(cam, &vg.msl[i]);
}
