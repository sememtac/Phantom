#include "vg_sim.h"
#include <Arduino.h>
#include <math.h>

// Procedural geometry and the shared maths the rest of the simulation is built
// on. Nothing here touches game state except the fields it fills at startup.

AstModel vg_models[NUM_MODELS];

// ---------------------------------------------------------------------------
// Fighter hull
//
// Verts 0..4 form a CLOSED hull (nose apex over a four-vertex rear ring), which
// is what hidden-line rendering needs. The tail fin is a flat blade with no
// volume, so it stays wireframe.
// ---------------------------------------------------------------------------

const Vec3 vg_ship_verts[SHIP_VERTS] = {
    { 0.00f,  0.00f,  2.40f},  // 0 nose      (hull apex)
    {-1.80f, -0.10f, -1.20f},  // 1 wing L    (rear ring)
    { 1.80f, -0.10f, -1.20f},  // 2 wing R    (rear ring)
    { 0.00f,  0.38f, -1.40f},  // 3 tail top  (rear ring)
    { 0.00f, -0.50f, -1.15f},  // 4 tail belly(rear ring)
    { 0.00f,  1.20f, -1.55f},  // 5 fin tip   (blade only)
    { 0.00f,  0.30f,  0.30f},  // 6 spine     (blade only)
};

// Nose fanned to each edge of the rear ring (1 -> 3 -> 2 -> 4), plus a two
// triangle rear cap. Winding is corrected at init.
uint8_t vg_ship_faces[SHIP_FACES][3] = {
    {0,1,3}, {0,3,2}, {0,2,4}, {0,4,1},
    {1,3,2}, {1,2,4},
};

const uint8_t vg_ship_fin[SHIP_FIN_EDGES][2] = { {3,5}, {5,6}, {6,3} };

// ---------------------------------------------------------------------------
// RNG
// ---------------------------------------------------------------------------

static uint32_t s_rng = 0x9E3779B9u;

void vg_rng_seed(uint32_t s) { s_rng = s | 1u; }
uint32_t vg_rng_peek(void) { return s_rng; }   // DIAGNOSTIC: determinism hash

static inline uint32_t rnd(void) {
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return s_rng;
}

float vg_frand01(void)           { return (rnd() >> 8) * (1.0f / 16777216.0f); }
float vg_frand(float a, float b) { return a + (b - a) * vg_frand01(); }

Vec3 vg_rand_unit(void) {
    float z = vg_frand(-1.0f, 1.0f);
    float r = sqrtf(1.0f - z * z);
    float a = vg_frand(0.0f, 6.2831853f);
    return v3(r * cosf(a), r * sinf(a), z);
}

// ---------------------------------------------------------------------------
// Shared maths
// ---------------------------------------------------------------------------

Vec3 vg_turn_toward(Vec3 from, Vec3 to, float max_ang) {
    Vec3  d = vnorm(to);
    float c = vdot(from, d);
    if (c >  1.0f) c =  1.0f;
    if (c < -1.0f) c = -1.0f;
    float ang = acosf(c);

    if (ang <= 1e-4f)   return from;
    if (ang <= max_ang) return d;

    // Component of the target direction perpendicular to the current heading.
    Vec3  perp = vsub(d, vmul(from, c));
    float pl   = vlen(perp);
    if (pl < 1e-6f) return from;          // exactly antiparallel: no unique plane
    perp = vmul(perp, 1.0f / pl);

    return vnorm(vadd(vmul(from, cosf(max_ang)), vmul(perp, sinf(max_ang))));
}

Mat3 vg_ship_basis(const Ship* s) {
    Vec3 f = vnorm(s->fwd);
    Vec3 u = s->up;
    u = vnorm(vsub(u, vmul(f, vdot(u, f))));      // re-orthogonalise
    Vec3 r = vcross(u, f);                        // +x right, given x cross y = z

    float cr = cosf(s->roll_vis), sr = sinf(s->roll_vis);
    Vec3 r2 = vadd(vmul(r, cr), vmul(u, sr));
    Vec3 u2 = vsub(vmul(u, cr), vmul(r, sr));

    Mat3 M;
    M.m[0] = r2.x; M.m[1] = u2.x; M.m[2] = f.x;
    M.m[3] = r2.y; M.m[4] = u2.y; M.m[5] = f.y;
    M.m[6] = r2.z; M.m[7] = u2.z; M.m[8] = f.z;
    return M;
}

// ---------------------------------------------------------------------------
// Model building
// ---------------------------------------------------------------------------

// Point every face's normal away from the hull's centroid, so the renderer can
// cull with one dot product -- the same contract the asteroid models use.
static void build_ship_model(void) {
    Vec3 c = v3(0, 0, 0);
    for (int i = 0; i < 5; i++) c = vadd(c, vg_ship_verts[i]);
    c = vmul(c, 1.0f / 5.0f);

    for (int f = 0; f < SHIP_FACES; f++) {
        Vec3 A = vg_ship_verts[vg_ship_faces[f][0]];
        Vec3 B = vg_ship_verts[vg_ship_faces[f][1]];
        Vec3 C = vg_ship_verts[vg_ship_faces[f][2]];
        Vec3 n = vcross(vsub(B, A), vsub(C, A));
        if (vdot(n, vsub(A, c)) < 0.0f) {
            uint8_t t = vg_ship_faces[f][1];
            vg_ship_faces[f][1] = vg_ship_faces[f][2];
            vg_ship_faces[f][2] = t;
        }
    }
}

void vg_build_models(void) {
    const float P = 1.6180339887f;
    Vec3 base[AST_VERTS] = {
        { 0,  1,  P}, { 0, -1,  P}, { 0,  1, -P}, { 0, -1, -P},
        { 1,  P,  0}, {-1,  P,  0}, { 1, -P,  0}, {-1, -P,  0},
        { P,  0,  1}, {-P,  0,  1}, { P,  0, -1}, {-P,  0, -1},
    };
    for (int i = 0; i < AST_VERTS; i++) base[i] = vnorm(base[i]);

    // ADJACENCY, which is all the face builder below needs. On the unit sphere an
    // icosahedron's adjacent vertices sit 1.0515 apart and the next-nearest
    // 1.7013, so any threshold between them recovers the 30 edges without a
    // table. An explicit edge LIST was built here and stored on every model too;
    // nothing has read it since the renderer moved to hidden-line faces.
    bool    adj[AST_VERTS][AST_VERTS] = {};
    for (int i = 0; i < AST_VERTS; i++)
        for (int j = i + 1; j < AST_VERTS; j++)
            if (vlen(vsub(base[i], base[j])) < 1.3f) {
                adj[i][j] = adj[j][i] = true;
            }
    // Any three mutually adjacent vertices form a face -- that recovers all 20
    // without a table too. Wind each so the normal points away from the origin,
    // so the renderer can cull with a single dot product.
    uint8_t fa[32][3];
    int fcount = 0;
    for (int i = 0; i < AST_VERTS; i++)
        for (int j = i + 1; j < AST_VERTS; j++) {
            if (!adj[i][j]) continue;
            for (int k = j + 1; k < AST_VERTS && fcount < 32; k++) {
                if (!adj[j][k] || !adj[i][k]) continue;
                uint8_t a = (uint8_t)i, b = (uint8_t)j, c = (uint8_t)k;
                Vec3 n = vcross(vsub(base[b], base[a]), vsub(base[c], base[a]));
                if (vdot(n, base[a]) < 0.0f) { uint8_t t = b; b = c; c = t; }
                fa[fcount][0] = a; fa[fcount][1] = b; fa[fcount][2] = c;
                fcount++;
            }
        }
    if (fcount > AST_FACES) fcount = AST_FACES;

    for (int m = 0; m < NUM_MODELS; m++) {
        AstModel* M = &vg_models[m];
        // Radial jitter only, and kept mild: scaling a vertex along its own
        // direction cannot flip a face's winding, and staying near-convex means
        // back-face culling alone gets hidden-line right without sorting faces
        // within the model.
        for (int i = 0; i < AST_VERTS; i++) M->v[i] = vmul(base[i], vg_frand(0.80f, 1.22f));
        for (int f = 0; f < fcount; f++) {
            M->f[f][0] = fa[f][0]; M->f[f][1] = fa[f][1]; M->f[f][2] = fa[f][2];
        }
        M->face_count = (uint8_t)fcount;
    }

    build_ship_model();
    Serial.printf("vg_models: %d models, %d faces\n", NUM_MODELS, fcount);
}

// ---------------------------------------------------------------------------
// Fields
// ---------------------------------------------------------------------------

// Motes live in the view frustum, so they spawn in a cone rather than a box --
// otherwise most would be off-screen and the few that were not would arrive in
// clumps.
Vec3 vg_mote_spawn(float zmin, float zmax) {
    float z = vg_frand(zmin, zmax);
    float a = vg_frand(0.0f, 6.2831853f);
    float r = sqrtf(vg_frand(0.02f, 1.0f)) * z * MOTE_CONE;
    return v3(cosf(a) * r, sinf(a) * r, z);
}

void vg_build_motes(void) {
    // Spread the initial z over the whole corridor so they do not all reach the
    // camera at the same moment and pulse.
    for (int i = 0; i < NUM_MOTES; i++) vg.mote[i] = vg_mote_spawn(0.0f, MOTE_Z_MAX);
}

void vg_build_starfield(void) {
    for (int i = 0; i < NUM_STARS; i++) {
        vg.star[i]   = vmul(vg_rand_unit(), STAR_DIST);
        vg.star_b[i] = (uint8_t)((uint32_t)(vg_frand01() * 3.0f) % 3u);
    }
}

// ===========================================================================
// PLAN OUTLINES, for the ship-select screen
//
// The four classes share one 3-D hull and will for a while yet. These are the
// other half of that problem: a flat TOP-DOWN silhouette per class, which is
// what a player actually recognises a fighter by, and which costs no projection,
// no culling and no per-class 3-D geometry to author.
//
// A turning 3-D model was tried first and was the wrong instrument. It spent most
// of its time edge-on, it was the same shape for all four, and a menu slot is
// wide and short -- which suits a plan view laid on its side and suits a
// spinning object badly.
//
// HALF THE SHAPE, MIRRORED. Each table is one side only, nose to tail, y >= 0.
// That halves the data, and it makes an accidentally asymmetric ship impossible
// rather than merely unlikely.
//
//   x   +1 at the nose, -1 at the tail
//   y   half-span, and the biggest across the roster is LANCE at 0.72
//
// The proportion is deliberate: 2.0 long against 1.44 across at the widest is
// about 1.4:1, which is what the drawings came back as. Scaling is uniform at
// draw time, so a class that is genuinely narrow LOOKS narrow next to the others
// rather than being stretched to fill the same box.
//
// WHAT EACH SILHOUETTE HAS TO SAY, since that is the whole job:
//   AEGIS     a balanced delta, wing root early, nothing exaggerated
//   LANCE     the widest, a double-delta kink, blocky square centre section
//   CHARIOT   a narrow dart, acute sweep, outrigger rails at the tail
//   BALLISTA  back-weighted: a long nose, wings set far aft, big stabilisers
// ===========================================================================

// AEGIS -- the reference. A clean delta: wing root early, wingtips at two thirds
// aft, a straight trailing edge back to the root and one modest tailplane.
static const float PLAN_AEGIS[] = {
     1.00f, 0.000f,
     0.74f, 0.055f,   0.26f, 0.120f,
    -0.30f, 0.640f,  -0.52f, 0.600f,     // wingtip, leading then trailing corner
    -0.60f, 0.170f,                       // trailing edge in to the wing root
    -0.78f, 0.150f,  -0.96f, 0.105f,     // tailplane
    -0.88f, 0.048f,  -1.00f, 0.042f,
    -1.00f, 0.000f,
};

// LANCE -- the widest, and the only leading edge with a KINK in it. The centre
// section is deliberately blocky: four hardpoints firing at once need span to
// leave from, which is why this hull is broad rather than the needle the fiction
// first suggested.
static const float PLAN_LANCE[] = {
     1.00f, 0.000f,
     0.72f, 0.070f,
     0.30f, 0.150f,   0.10f, 0.260f,     // the kink -- a double delta
    -0.26f, 0.720f,  -0.50f, 0.680f,
    -0.58f, 0.260f,                       // a wide, square-shouldered root
    -0.76f, 0.235f,  -0.98f, 0.175f,
    -0.88f, 0.080f,  -1.00f, 0.070f,
    -1.00f, 0.000f,
};

// CHARIOT -- a dart. The narrowest span in the roster and the most acute sweep.
//
// It had outrigger rails standing well proud of the tail, for the twelve rounds
// it carries. They had to go: an outline that swings wide and comes back reads as
// a CLAW at this size, not as a rail, and the shape stopped looking like an
// aircraft. Span and sweep carry this class instead.
static const float PLAN_CHARIOT[] = {
     1.00f, 0.000f,
     0.76f, 0.040f,   0.34f, 0.085f,
    -0.36f, 0.480f,  -0.56f, 0.440f,     // narrow tip, very swept
    -0.66f, 0.120f,                       // trailing edge in, hard
    -0.82f, 0.145f,  -0.98f, 0.100f,     // a small tailplane, barely proud
    -0.92f, 0.040f,  -1.00f, 0.035f,
    -1.00f, 0.000f,
};

// BALLISTA -- back-weighted. The wing root sits far aft, which leaves a long nose
// ahead of it: a hull built around its optics and given just enough airframe to
// carry them. The biggest tailplanes in the roster and the least wing forward.
static const float PLAN_BALLISTA[] = {
     1.00f, 0.000f,
     0.62f, 0.045f,   0.04f, 0.085f,     // a long, slender nose
    -0.34f, 0.560f,  -0.54f, 0.520f,
    -0.62f, 0.170f,
    -0.76f, 0.250f,  -0.98f, 0.205f,     // the biggest stabiliser in the roster
    -0.90f, 0.070f,  -1.00f, 0.060f,
    -1.00f, 0.000f,
};

// Indexed by ShipClass, and the order is checked at build time so a row cannot
// drift away from the class it draws.
const VgShipPlan vg_ship_plan[SHIP_CLASSES] = {
    { PLAN_AEGIS,    (uint8_t)(sizeof(PLAN_AEGIS)    / (2 * sizeof(float))) },
    { PLAN_LANCE,    (uint8_t)(sizeof(PLAN_LANCE)    / (2 * sizeof(float))) },
    { PLAN_CHARIOT,  (uint8_t)(sizeof(PLAN_CHARIOT)  / (2 * sizeof(float))) },
    { PLAN_BALLISTA, (uint8_t)(sizeof(PLAN_BALLISTA) / (2 * sizeof(float))) },
};

static_assert(sizeof(vg_ship_plan) / sizeof(vg_ship_plan[0]) == SHIP_CLASSES,
              "one plan outline per class, in ShipClass order");
