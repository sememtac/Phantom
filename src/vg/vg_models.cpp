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
//   y   half-span, and the biggest across the roster is LANCE at 0.566
//
// TRACED, NOT DRAWN BY HAND. The outlines are the per-column silhouette envelope
// of the reference art, simplified to the points that carry the shape -- so the
// proportions are the artist's rather than my recollection of them. The interior
// segments are authored, because structure is a judgement about what matters at
// 115 pixels and an envelope cannot make it.
//
// Scaling is uniform at draw time, so a class that is genuinely narrow LOOKS
// narrow beside the others instead of being stretched to fill the same box.
// BALLISTA is 3.2 times as long as it is wide and AEGIS is 2.0, and BALLISTA's
// reverse wing is the widest thing in the roster; that spread is most of what
// tells the four apart at a glance.
//
// WHAT EACH SILHOUETTE HAS TO SAY, since that is the whole job:
//   AEGIS     a balanced delta, wing root early, nothing exaggerated
//   LANCE     the widest, a double-delta kink, blocky square centre section
//   CHARIOT   a narrow dart, acute sweep, outrigger rails at the tail
//   BALLISTA  back-weighted: a long nose, wings set far aft, big stabilisers
// ===========================================================================

// AEGIS -- the reference. A clean delta, and the radar dome at the tail is the
// lifeline: the bay only rearms while that antenna holds the target.
static const float PLAN_AEGIS[] = {
     1.000f,  0.000f,  0.281f,  0.158f, -0.276f,  0.488f,
    -0.482f,  0.456f, -0.580f,  0.235f, -0.982f,  0.158f,
    -1.000f,  0.080f,
};

static const float DTL_AEGIS[] = {
    -0.520f,  0.000f, -0.580f,  0.150f, -0.580f,  0.150f, -0.730f,  0.200f,
    -0.730f,  0.200f, -0.880f,  0.150f, -0.880f,  0.150f, -0.930f,  0.000f,
     0.520f,  0.000f,  0.450f,  0.080f,  0.450f,  0.080f,  0.280f,  0.074f,
     0.280f,  0.074f,  0.220f,  0.000f,
};

static const float BAY_AEGIS[] = {
    -0.020f,  0.235f, -0.280f,  0.300f,
};

// LANCE -- the widest, and the four bays are the whole read on it -- two a side,
// side by side, because the salvo leaves from four places at once.
static const float PLAN_LANCE[] = {
     0.885f,  0.050f,  0.400f,  0.112f,  0.235f,  0.178f,
     0.168f,  0.179f, -0.219f,  0.566f, -0.400f,  0.552f,
    -0.431f,  0.413f, -0.690f,  0.432f, -0.718f,  0.306f,
    -0.756f,  0.269f, -0.924f,  0.270f, -1.000f,  0.203f,
};

static const float DTL_LANCE[] = {
     0.460f,  0.000f,  0.390f,  0.068f,  0.390f,  0.068f,  0.170f,  0.064f,
     0.170f,  0.064f,  0.110f,  0.000f,
};

static const float BAY_LANCE[] = {
    -0.330f,  0.175f, -0.520f,  0.265f, -0.330f,  0.340f, -0.520f,  0.430f,
};

// CHARIOT -- a dart, and mostly frame. Its bay is the panel FILLED, with the lattice
// cut back out of it -- twelve lit cells rather than a scratched-on grid.
static const float PLAN_CHARIOT[] = {
     1.000f,  0.000f,  0.574f,  0.062f,  0.352f,  0.120f,
     0.343f,  0.151f,  0.130f,  0.184f, -0.362f,  0.379f,
    -0.437f,  0.464f, -0.623f,  0.460f, -0.810f,  0.482f,
    -0.819f,  0.144f, -0.881f,  0.131f, -0.898f,  0.038f,
    -1.000f,  0.004f,
};

static const float DTL_CHARIOT[] = {
    -0.420f,  0.030f, -0.760f,  0.030f, -0.760f,  0.030f, -0.760f,  0.260f,
    -0.760f,  0.260f, -0.420f,  0.260f, -0.420f,  0.260f, -0.420f,  0.030f,
     0.330f,  0.000f,  0.260f,  0.100f,  0.260f,  0.100f,  0.110f,  0.094f,
     0.110f,  0.094f,  0.050f,  0.000f,
};

static const float BAY_CHARIOT[] = {
    -0.420f,  0.030f, -0.760f,  0.260f,
};

static const float CUT_CHARIOT[] = {
    -0.760f,  0.160f, -0.630f,  0.030f, -0.750f,  0.260f, -0.520f,  0.030f,
    -0.640f,  0.260f, -0.420f,  0.040f, -0.530f,  0.260f, -0.420f,  0.150f,
    -0.760f,  0.160f, -0.660f,  0.260f, -0.760f,  0.050f, -0.550f,  0.260f,
    -0.670f,  0.030f, -0.440f,  0.260f, -0.560f,  0.030f, -0.420f,  0.170f,
};

// BALLISTA -- back-weighted, built around the barrel, and forward-swept at BOTH ends:
// one reverse wing reads as an oddity, two say the airframe is that way.
static const float PLAN_BALLISTA[] = {
     0.955f,  0.030f,  0.898f,  0.039f,  0.702f,  0.033f,
     0.637f,  0.057f,  0.560f,  0.062f,  0.645f,  0.245f,
     0.470f,  0.238f,  0.395f,  0.076f,  0.355f,  0.075f,
    -0.132f,  0.215f, -0.198f,  0.242f, -0.234f,  0.303f,
    -0.333f,  0.218f, -0.673f,  0.148f, -0.752f,  0.123f,
    -0.470f,  0.575f, -0.665f,  0.550f, -0.955f,  0.090f,
    -1.000f,  0.053f,
};

static const float DTL_BALLISTA[] = {
     0.960f,  0.000f,  0.960f,  0.030f,  0.960f,  0.030f,  0.300f,  0.050f,
     0.300f,  0.050f,  0.300f,  0.000f,  0.620f,  0.030f,  0.620f,  0.000f,
     0.100f,  0.000f,  0.020f,  0.106f,  0.020f,  0.106f, -0.120f,  0.100f,
    -0.120f,  0.100f, -0.180f,  0.000f,  0.260f,  0.000f,  0.150f,  0.100f,
};

static const float BAY_BALLISTA[] = {
    -0.220f,  0.150f, -0.400f,  0.250f, -0.220f,  0.000f, -0.400f,  0.055f,
};

const VgShipPlan vg_ship_plan[SHIP_CLASSES] = {
    { PLAN_AEGIS,    (uint8_t)(sizeof(PLAN_AEGIS) / (2 * sizeof(float))),
      DTL_AEGIS,     (uint8_t)(sizeof(DTL_AEGIS)  / (4 * sizeof(float))),
      BAY_AEGIS,     (uint8_t)(sizeof(BAY_AEGIS)  / (4 * sizeof(float))),
      nullptr,        0 },
    { PLAN_LANCE,    (uint8_t)(sizeof(PLAN_LANCE) / (2 * sizeof(float))),
      DTL_LANCE,     (uint8_t)(sizeof(DTL_LANCE)  / (4 * sizeof(float))),
      BAY_LANCE,     (uint8_t)(sizeof(BAY_LANCE)  / (4 * sizeof(float))),
      nullptr,        0 },
    { PLAN_CHARIOT,  (uint8_t)(sizeof(PLAN_CHARIOT) / (2 * sizeof(float))),
      DTL_CHARIOT,   (uint8_t)(sizeof(DTL_CHARIOT)  / (4 * sizeof(float))),
      BAY_CHARIOT,   (uint8_t)(sizeof(BAY_CHARIOT)  / (4 * sizeof(float))),
      CUT_CHARIOT,    (uint8_t)(sizeof(CUT_CHARIOT) / (4 * sizeof(float))) },
    { PLAN_BALLISTA, (uint8_t)(sizeof(PLAN_BALLISTA) / (2 * sizeof(float))),
      DTL_BALLISTA,  (uint8_t)(sizeof(DTL_BALLISTA)  / (4 * sizeof(float))),
      BAY_BALLISTA,  (uint8_t)(sizeof(BAY_BALLISTA)  / (4 * sizeof(float))),
      nullptr,        0 },
};

static_assert(sizeof(vg_ship_plan) / sizeof(vg_ship_plan[0]) == SHIP_CLASSES,
              "one plan outline per class, in ShipClass order");
