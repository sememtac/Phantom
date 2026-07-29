#include "vg_sky.h"
#include "vg_config.h"
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

// 128x128 RGB565, byte-swapped to match the panel storage convention. 32 KB.
//
// An earlier version kept a second pre-darkened copy and selected it on scanline
// rows, so the backdrop got scanlines for free. That was a mistake: it gave the
// NEBULA scanlines but left every vector element drawn on top of it without any,
// which quietly deleted the effect from the whole HUD. The scanline pass now
// runs over the finished band instead, so background and instruments are treated
// alike -- and it turns out to still hide under DMA.
#define SKY_TEX_BITS 7
#define SKY_TEX_SIZE (1 << SKY_TEX_BITS)
#define SKY_TEX_MASK (SKY_TEX_SIZE - 1)

// Texels per screen pixel. This governs how BIG the cloud reads, and getting it
// wrong is what made the first version feel like close-up noise: at 0.30 the
// screen showed 144 texels of a 128-texel texture, i.e. the whole texture was
// smaller than the screen, so several copies were visible at once and every
// feature was tiny. At 0.10 the texture spans ~2.7 screens, features fill a good
// fraction of the view, and a repeat only comes round every ~165 degrees of yaw.
//
// The cost is a ~10x nearest-neighbour upscale, which would band badly on smooth
// gradients -- hence the dither in the fill.
#define SKY_SCALE    0.10f

// On screen a radian subtends roughly FOCAL pixels, so SKY_SCALE * FOCAL is the
// pan rate of a genuinely infinite backdrop. We deliberately run a little under
// it: a touch of lag reads as enormous distance, and costs nothing since a
// tiling cloud has no "correct" absolute position to betray the cheat.
#define SKY_PAN_FACTOR  0.75f
#define SKY_PAN_PER_RAD (SKY_SCALE * FOCAL * SKY_PAN_FACTOR)

// Ceiling on backdrop brightness. It must sit well below the vector art or it
// competes with the thing the player is actually looking at.
// Backdrop ceiling. At 0.80 galaxy cores peaked around 76/125 and thin HUD
// strokes lost contrast against them -- a backdrop that competes with the
// foreground is just noise.
#define SKY_MAX_LEVEL   0.52f

// The menu gets its own, far higher ceiling. That cap above exists to protect
// thin HUD strokes during a fight -- and the menu has no HUD, only the title
// and the crawl, both of which are large text sitting mostly over the shadow.
// Held to the combat ceiling the black hole came out at peak 61/125, which is a
// dim smudge rather than the thing you are meant to sit and look at.
#define SKY_MENU_LEVEL  0.92f

// Sampling scale and pan rate, set per backdrop at generation time.
//
// The clouds want the defaults: they are unbounded fields, so panning through
// them is the whole point and a tile boundary is nothing to look at.
//
// The black hole is the opposite. It is a single object, and flying a lap of
// the torus means yawing continuously through 360 degrees, which pans the sky
// through several tile widths and produces a procession of black holes sliding
// past. So it does not pan at all: an object at that distance would not shift
// no matter how far you flew, and holding the sample centred on it means the
// repeats can never be reached.
static float s_scale = SKY_SCALE;
static float s_pan   = SKY_PAN_PER_RAD;

// Bigger than the combat skies, and this is close to the ceiling. The lensed
// arcs sit at ~20 texels; much past this and they cross the top and bottom
// edges, which crops the one feature that makes it read as a black hole.
// Raised so the whole structure sits inside the frame with room around it. It
// is a backdrop -- the thing behind the title and the crawl -- and a black hole
// cropped by the screen edges reads as a dark shape rather than as an object.
#define SKY_MENU_SCALE  0.118f

static uint16_t* s_tex   = nullptr;
static bool      s_ready = false;

static float s_u = 0.0f, s_v = 0.0f, s_bank = 0.0f;

// 4x4 Bayer, recentred and scaled to +-0.5 texel in 16.16 fixed point.
static const int32_t s_dither[16] = {
    ( 0 - 8) * 4096, ( 8 - 8) * 4096, ( 2 - 8) * 4096, (10 - 8) * 4096,
    (12 - 8) * 4096, ( 4 - 8) * 4096, (14 - 8) * 4096, ( 6 - 8) * 4096,
    ( 3 - 8) * 4096, (11 - 8) * 4096, ( 1 - 8) * 4096, ( 9 - 8) * 4096,
    (15 - 8) * 4096, ( 7 - 8) * 4096, (13 - 8) * 4096, ( 5 - 8) * 4096,
};

// ---------------------------------------------------------------------------
// Tileable value noise
// ---------------------------------------------------------------------------

static inline uint32_t hash2(int x, int y, uint32_t seed) {
    uint32_t n = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u + seed * 2246822519u;
    n = (n ^ (n >> 13)) * 1274126177u;
    return n ^ (n >> 16);
}

static inline float lattice(int x, int y, uint32_t seed) {
    return (float)(hash2(x, y, seed) & 0xFFFFu) * (1.0f / 65535.0f);
}

// Wrapping the lattice at `period` is what makes the result tile seamlessly --
// necessary because the backdrop pans indefinitely as the ship turns.
static float vnoise(float x, float y, int period, uint32_t seed) {
    int   xi = (int)floorf(x), yi = (int)floorf(y);
    float xf = x - (float)xi,  yf = y - (float)yi;
    float sx = xf * xf * (3.0f - 2.0f * xf);
    float sy = yf * yf * (3.0f - 2.0f * yf);

    int x0 = ((xi % period) + period) % period, x1 = (x0 + 1) % period;
    int y0 = ((yi % period) + period) % period, y1 = (y0 + 1) % period;

    float a = lattice(x0, y0, seed), b = lattice(x1, y0, seed);
    float c = lattice(x0, y1, seed), d = lattice(x1, y1, seed);
    float ab = a + (b - a) * sx;
    float cd = c + (d - c) * sx;
    return ab + (cd - ab) * sy;
}

// fBm sampled directly in TEXEL space.
//
// This takes texel indices rather than free-floating coordinates on purpose. A
// field only tiles if its coordinate range across the texture is exactly a whole
// number of lattice periods, so the scale and the period have to advance in
// lockstep. The previous version scaled coordinates independently
// (`fbm(x * 0.45f, ...)`, spanning 0..1.8 against a period of 4) for the mask and
// tone fields, which is precisely why the texture had a visible seam. Feature
// size is now chosen with `base_period` instead, which cannot break tiling.
static float fbm_tex(int tx, int ty, uint32_t seed, int octaves, int base_period) {
    float sum = 0.0f, amp = 0.5f, norm = 0.0f;
    float scale  = (float)base_period / (float)SKY_TEX_SIZE;
    int   period = base_period;

    for (int o = 0; o < octaves; o++) {
        sum  += amp * vnoise((float)tx * scale, (float)ty * scale, period,
                             seed + (uint32_t)o * 131u);
        norm += amp;
        amp   *= 0.52f;
        scale *= 2.0f;
        period <<= 1;
    }
    return sum / norm;
}

// ---------------------------------------------------------------------------

bool vg_sky_init(void) {
    const size_t bytes = (size_t)SKY_TEX_SIZE * SKY_TEX_SIZE * 2;

    // Internal only. The fill reads this every frame in a scattered pattern;
    // from PSRAM it would thrash the cache exactly as a full framebuffer would.
    s_tex = (uint16_t*)heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    if (!s_tex) {
        Serial.println("vg_sky_init: out of internal SRAM, backdrop disabled");
        return false;
    }
    memset(s_tex, 0, bytes);
    Serial.printf("vg_sky_init: %uKB, internal-free %uKB\n",
                  (unsigned)(bytes / 1024),
                  (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
    return true;
}

bool vg_sky_ready(void) { return s_ready; }

static inline uint16_t pack565_swapped(float r, float g, float b) {
    if (r < 0) r = 0; if (r > 1) r = 1;
    if (g < 0) g = 0; if (g > 1) g = 1;
    if (b < 0) b = 0; if (b > 1) b = 1;
    uint16_t v = (uint16_t)(((uint16_t)(r * 31.0f) << 11) |
                            ((uint16_t)(g * 63.0f) << 5)  |
                             (uint16_t)(b * 31.0f));
    return (uint16_t)((v >> 8) | (v << 8));
}

// Shortest distance across the wrap. Everything positional in here uses it, so
// a feature placed anywhere still tiles seamlessly instead of being clipped at
// the texture edge.
static inline float wrap_delta(int a, int b) {
    int d = a - b;
    if (d >  SKY_TEX_SIZE / 2) d -= SKY_TEX_SIZE;
    if (d < -SKY_TEX_SIZE / 2) d += SKY_TEX_SIZE;
    return (float)d;
}

// --- galaxy ----------------------------------------------------------------

static void gen_galaxy(uint32_t seed) {
    const int   gx     = (int)(hash2(1, 1, seed) % SKY_TEX_SIZE);
    const int   gy     = (int)(hash2(2, 2, seed) % SKY_TEX_SIZE);
    const float arms   = (hash2(3, 3, seed) & 1u) ? 2.0f : 3.0f;
    const float twist  = 2.0f + (float)(hash2(4, 4, seed) % 100u) * 0.014f;
    const float radius = (float)SKY_TEX_SIZE * 0.34f;

    for (int ty = 0; ty < SKY_TEX_SIZE; ty++) {
        for (int tx = 0; tx < SKY_TEX_SIZE; tx++) {
            const int i = (ty << SKY_TEX_BITS) + tx;

            float dx = wrap_delta(tx, gx), dy = wrap_delta(ty, gy);
            float r  = sqrtf(dx * dx + dy * dy);
            float rn = r / radius;
            if (rn >= 1.0f) { s_tex[i] = 0; continue; }

            // Logarithmic spiral: the arm phase advances with ln(r), which is
            // what gives real galaxies their constant pitch angle.
            float phase = atan2f(dy, dx) * arms - logf(r * 0.10f + 1.0f) * twist * arms;
            float arm   = 0.5f + 0.5f * cosf(phase);
            arm = arm * arm * arm;                       // tighten the arms

            float disk = expf(-rn * rn * 2.6f);
            float core = expf(-rn * rn * 30.0f);
            float mott = fbm_tex(tx, ty, seed + 991u, 3, 4);   // break up the arms

            // Fade to exactly zero at the rim, or the disc would leave a visible
            // circular edge where it is cut off.
            float edge = 1.0f - rn * rn;

            float dens = (disk * (0.20f + 0.80f * arm) * (0.55f + 0.75f * mott)
                          + core * 1.6f) * edge * SKY_MAX_LEVEL;
            if (dens > SKY_MAX_LEVEL) dens = SKY_MAX_LEVEL;

            float warm = core * 2.2f;
            if (warm > 1.0f) warm = 1.0f;

            s_tex[i] = pack565_swapped(dens * (0.42f + 0.58f * warm),
                                       dens * (0.30f + 0.50f * warm),
                                       dens * (0.80f - 0.30f * warm));
        }
    }
}

// --- star cluster ----------------------------------------------------------

static void gen_cluster(uint32_t seed) {
    const int KNOTS = 5 + (int)(hash2(9, 9, seed) % 4u);
    struct Knot { int x, y; float rad, amp, tone; } k[8];

    for (int n = 0; n < KNOTS; n++) {
        uint32_t h = hash2(n + 40, n * 7 + 3, seed);
        k[n].x    = (int)(h % SKY_TEX_SIZE);
        k[n].y    = (int)((h >> 9) % SKY_TEX_SIZE);
        k[n].rad  = 7.0f + (float)((h >> 18) % 20u);
        k[n].amp  = 0.35f + (float)((h >> 24) % 100u) * 0.0065f;
        k[n].tone = (float)((h >> 5) % 100u) * 0.01f;
    }

    for (int ty = 0; ty < SKY_TEX_SIZE; ty++) {
        for (int tx = 0; tx < SKY_TEX_SIZE; tx++) {
            const int i = (ty << SKY_TEX_BITS) + tx;

            float sum = 0.0f, tsum = 0.0f;
            for (int n = 0; n < KNOTS; n++) {
                float dx = wrap_delta(tx, k[n].x), dy = wrap_delta(ty, k[n].y);
                float r2 = dx * dx + dy * dy;
                float rr = k[n].rad;
                if (r2 > 9.0f * rr * rr) continue;        // beyond 3 sigma
                float g = expf(-r2 / (2.0f * rr * rr)) * k[n].amp;
                sum  += g;
                tsum += g * k[n].tone;
            }

            float haze = fbm_tex(tx, ty, seed + 555u, 3, 3);
            haze = (haze - 0.42f) * 1.9f;
            if (haze < 0.0f) haze = 0.0f;

            float dens = (sum + haze * 0.22f) * SKY_MAX_LEVEL;
            if (dens > SKY_MAX_LEVEL) dens = SKY_MAX_LEVEL;

            float tone = (sum > 0.001f) ? (tsum / sum) : 0.5f;

            s_tex[i] = pack565_swapped(dens * (0.55f + 0.45f * tone),
                                       dens * (0.52f + 0.30f * tone),
                                       dens * (0.85f - 0.20f * tone));
        }
    }
}

// --- menu backdrop: the black hole -----------------------------------------
//
// Gargantua. Four features, and it is unmistakable only if all four are there:
//
//   1. the shadow      a hard black disc, larger than the horizon itself
//   2. the photon ring a thin, near-white line hugging that edge
//   3. the disc        seen almost edge-on, running out to either side
//   4. the halo        the FAR side of the disc, lensed up over the top and
//                      down under the bottom, closing into a ring
//
// (4) is the one that sells it. Without it this is a dot with a stripe; with
// it, it reads as light being bent around a hole. It is painted as an annulus
// weighted toward the vertical rather than ray-traced, which is nowhere near
// the real null geodesics but lands in the same place visually at this size.
//
// Everything is measured with wrap_delta, so the whole construction tiles even
// though it is a single localised object.
//
// This suits the panel better than the noise backdrops do, incidentally: it is
// almost entirely LOW frequency -- a big dark disc and broad smooth arcs -- and
// low frequency is exactly what survives a 10x nearest-neighbour upscale.
static void gen_blackhole(uint32_t seed) {
    const int   cx = SKY_TEX_SIZE / 2;
    const int   cy = SKY_TEX_SIZE / 2;

    const float R_SHADOW = (float)SKY_TEX_SIZE * 0.115f;   // ~15 texels
    const float R_PHOTON = R_SHADOW * 1.08f;
    const float R_HALO   = R_SHADOW * 1.36f;
    const float HALO_W   = R_SHADOW * 0.20f;
    // Must not go below about a texel and a half. At 0.055 this worked out to
    // 0.81 texels -- narrower than the grid it is drawn on, so the sampler
    // landed off the peak and the ring came out dim and broken instead of the
    // hard white rim that defines the edge of the shadow. The texture reported
    // it too: peak 61/125 where a saturated ring should read near 95.
    const float PHOTON_W = R_SHADOW * 0.125f;
    const float DISC_H   = (float)SKY_TEX_SIZE * 0.017f;   // half-thickness
    const float DISC_R   = R_SHADOW * 2.3f;                // radial falloff

    for (int ty = 0; ty < SKY_TEX_SIZE; ty++) {
        for (int tx = 0; tx < SKY_TEX_SIZE; tx++) {
            const int   i  = (ty << SKY_TEX_BITS) + tx;
            const float dx = wrap_delta(tx, cx);
            const float dy = wrap_delta(ty, cy);
            const float r  = sqrtf(dx * dx + dy * dy);

            // Faint dust so the surrounding sky is not dead black.
            float haze = fbm_tex(tx, ty, seed + 313u, 4, 3);
            haze = (haze - 0.54f) * 1.3f;
            if (haze < 0.0f) haze = 0.0f;
            float lum = haze * 0.10f;

            // The disc, edge-on: a thin band running out to both sides, thrown
            // brighter on one side than the other. Real relativistic beaming is
            // far more violent than this; a hint of it stops the image reading
            // as symmetrical clip art.
            const float band  = expf(-(dy * dy) / (2.0f * DISC_H * DISC_H));
            const float reach = 1.0f / (1.0f + (r / DISC_R) * (r / DISC_R));
            lum += band * reach * ((dx < 0.0f) ? 1.15f : 0.62f);

            // The halo: the far side of the disc bent over and under. Weighted
            // by |dy|/r so it piles up above and below and thins at the sides,
            // which is where the near disc already is.
            const float hd   = (r - R_HALO) / HALO_W;
            const float vert = (r > 0.001f) ? (dy < 0.0f ? -dy : dy) / r : 0.0f;
            lum += expf(-hd * hd) * (0.30f + 0.70f * vert) * 0.85f;

            // Photon ring.
            const float pd = (r - R_PHOTON) / PHOTON_W;
            lum += expf(-pd * pd) * 1.15f;

            // The shadow. Nothing comes out, and the edge is softened over a
            // texel and a half so a ~300px black disc does not upscale into a
            // visible polygon.
            if (r < R_SHADOW) {
                const float e = R_SHADOW - r;
                lum *= (e > 1.5f) ? 0.0f : (1.0f - e / 1.5f);
            }

            if (lum > 1.0f) lum = 1.0f;

            // Gargantua is gold, whitening only at the very hottest parts. The
            // squared term keeps blue out of everything but the photon ring,
            // which is what stops it looking like a generic glow.
            const float w = lum * lum;
            float R = lum * SKY_MENU_LEVEL;
            float G = (lum * 0.60f + w * 0.38f) * SKY_MENU_LEVEL;
            float B = (lum * 0.13f + w * w * 0.72f) * SKY_MENU_LEVEL;

            if (R > SKY_MENU_LEVEL) R = SKY_MENU_LEVEL;
            if (G > SKY_MENU_LEVEL) G = SKY_MENU_LEVEL;
            if (B > SKY_MENU_LEVEL) B = SKY_MENU_LEVEL;

            s_tex[i] = pack565_swapped(R, G, B);
        }
    }
}

// --- nebula ----------------------------------------------------------------

static void gen_nebula(uint32_t seed) {
    for (int ty = 0; ty < SKY_TEX_SIZE; ty++) {
        for (int tx = 0; tx < SKY_TEX_SIZE; tx++) {
            // Density, then a large-scale mask so the cloud has empty sky
            // around it instead of covering everything uniformly.
            //
            // Both fields MUST be stretched before shaping. fBm of value noise
            // clusters hard around 0.5 and barely reaches 0 or 1, so raising the
            // raw product to a power annihilates it -- the first version of this
            // left 99% of texels at pure black and a peak blue of 4/31, i.e. an
            // invisible nebula.
            // base_period 3 puts the largest structures at ~1/3 of the texture,
            // which at this scale is most of a screen -- a cloud you fly past
            // rather than a field of specks.
            float d = fbm_tex(tx, ty, seed, 5, 3);
            d = (d - 0.32f) * 2.6f;
            if (d < 0.0f) d = 0.0f; else if (d > 1.0f) d = 1.0f;
            d = powf(d, 1.6f);                // mostly faint, with bright wisps

            float mask = fbm_tex(tx, ty, seed + 7919u, 2, 2);
            mask = (mask - 0.30f) * 2.2f;
            if (mask < 0.0f) mask = 0.0f; else if (mask > 1.0f) mask = 1.0f;

            float dens = d * mask * SKY_MAX_LEVEL;

            // A second field shifts the tone across the cloud so it is not one
            // flat colour: cool indigo through violet to a warm core.
            float tone = fbm_tex(tx, ty, seed + 4211u, 2, 2);

            float r = dens * (0.22f + 0.90f * tone);
            float g = dens * (0.10f + 0.28f * tone);
            float b = dens * (0.60f + 0.40f * (1.0f - tone));

            s_tex[(ty << SKY_TEX_BITS) + tx] = pack565_swapped(r, g, b);
        }
    }
}

// ---------------------------------------------------------------------------

static SkyKind s_kind = SKY_NEBULA;

static float s_reveal = 1.0f;
void  vg_sky_set_reveal(float r) { s_reveal = (r < 0.0f) ? 0.0f : (r > 1.0f ? 1.0f : r); }
float vg_sky_reveal(void)        { return s_reveal; }

// Somewhere to have a fight. Proper noun plus the kind of thing the backdrop
// actually is, so the name is never at odds with what is on screen.
static char s_place[24];

const char* vg_sky_place(void) { return s_place; }

static void name_place(uint32_t seed) {
    static const char* const PROPER[] = {
        "VELA", "CARINA", "ORION", "LYRA", "DRACO", "HYDRA", "CYGNUS", "AURIGA",
        "TUCANA", "NORMA", "PYXIS", "SERPENS", "VOLANS", "MENSA", "INDUS", "ARA",
        "CORVUS", "GRUS", "LEPUS", "MUSCA", "OCTANS", "PICTOR", "RETICULUM", "SEXTANS",
    };
    const int n = (int)(sizeof(PROPER) / sizeof(PROPER[0]));
    const char* p = PROPER[(seed >> 7) % (uint32_t)n];
    const char* k = (s_kind == SKY_GALAXY)  ? "GALAXY"
                  : (s_kind == SKY_CLUSTER) ? "CLUSTER"
                  : (s_kind == SKY_MENU)    ? "SINGULARITY"
                                            : "NEBULA";
    snprintf(s_place, sizeof(s_place), "%s %s", p, k);
}

const char* vg_sky_name(void) {
    switch (s_kind) {
    case SKY_GALAXY:  return "GALAXY";
    case SKY_CLUSTER: return "CLUSTER";
    case SKY_MENU:    return "GARGANTUA";
    default:          return "NEBULA";
    }
}

void vg_sky_generate(SkyKind kind, uint32_t seed) {
    if (!s_tex) return;

    uint32_t t0 = millis();

    // SKY_KINDS sits inside the enum as a count, so it is a reachable value and
    // not a valid kind -- the switch has to select the generator and the label
    // together rather than sanitising the input first.
    switch (kind) {
    case SKY_GALAXY:  s_kind = SKY_GALAXY;  gen_galaxy(seed);  break;
    case SKY_CLUSTER: s_kind = SKY_CLUSTER; gen_cluster(seed); break;
    case SKY_MENU:    s_kind = SKY_MENU;    gen_blackhole(seed); break;
    default:          s_kind = SKY_NEBULA;  gen_nebula(seed);  break;
    }

    name_place(seed);
    s_reveal = 1.0f;      // callers that want a dissolve ask for it afterwards

    if (s_kind == SKY_MENU) {
        s_scale = SKY_MENU_SCALE;
        s_pan   = 0.0f;                       // fixed at infinity; see above
        // Park the sample on the hole itself. The fill maps screen centre to
        // (s_u, s_v), so this is what puts it in the middle of the view -- and
        // with no pan it stays there for good.
        s_u = s_v = (float)(SKY_TEX_SIZE / 2);
    } else {
        s_scale = SKY_SCALE;
        s_pan   = SKY_PAN_PER_RAD;
        // Clouds have no centre worth finding, so any origin will do.
        s_u = s_v = 0.0f;
    }

    // Report what actually landed in the texture. A backdrop that is silently
    // all-black looks identical to one that is not being drawn at all, and
    // telling those apart by eye cost a flash cycle once already.
    int      lit  = 0;
    uint32_t peak = 0;
    for (int i = 0; i < SKY_TEX_SIZE * SKY_TEX_SIZE; i++) {
        uint16_t n = (uint16_t)((s_tex[i] >> 8) | (s_tex[i] << 8));
        if (n) lit++;
        uint32_t bright = ((n >> 11) & 0x1F) + ((n >> 5) & 0x3F) + (n & 0x1F);
        if (bright > peak) peak = bright;
    }

    // Bank only. This line used to reset the sampling ORIGIN too, and it runs
    // after the per-kind setup below it -- so it quietly undid the centring that
    // puts the black hole in front of the camera and left the menu sampling
    // texel (0,0), the corner of the tile. The texture was perfect the whole
    // time; it was simply being read from the empty quarter.
    s_bank = 0.0f;
    s_ready = true;

    // Origin and scale are in the report for the same reason lit and peak are:
    // a backdrop pointed at the wrong part of its own texture looks exactly like
    // one that was never generated.
    Serial.printf("vg_sky_generate: %s seed %u in %ums  lit %d%%  peak %u/125"
                  "  uv %.0f,%.0f  scale %.3f\n",
                  vg_sky_name(), (unsigned)seed, (unsigned)(millis() - t0),
                  lit * 100 / (SKY_TEX_SIZE * SKY_TEX_SIZE), (unsigned)peak,
                  (double)s_u, (double)s_v, (double)s_scale);
}

void vg_sky_step(float d_pitch, float d_yaw, float bank) {
    // The backdrop must sweep exactly the way the starfield does, or it reads as
    // attached to the ship rather than to the universe.
    //
    // A feature at texel U lands at screen x = cx + (U - s_u)/S, so screen
    // position moves OPPOSITE to s_u. Turning right (d_yaw > 0) has to push the
    // world left, i.e. shrink x, i.e. grow s_u -- hence '+='. This was '-=',
    // which slid the sky along with the turn instead of against it.
    // The pan is expressed in LOGICAL screen units first, then carried into
    // texture space through the bank rotation. Accumulating straight into u/v
    // was only correct while bank was zero -- it quietly ignored roll, and would
    // now be 90 degrees out as well.
    const float pan_x = d_yaw   * s_pan;   // logical +x
    const float pan_y = d_pitch * s_pan;   // logical +y (screen down)

    const float cb = cosf(bank), sb = sinf(bank);
    s_u += pan_x * cb - pan_y * sb;
    s_v += pan_x * sb + pan_y * cb;

    s_bank = bank;
}

void vg_sky_fill_band(uint16_t* band, int band_y0) {
    if (!s_ready) return;

    // This fill writes the band directly, so unlike every other drawing path it
    // never passes through the rasteriser's rotation. It has to account for the
    // display quarter-turn itself.
    //
    // Sampling maps a screen offset into texture space by rotating it by `bank`.
    // Feeding it PANEL offsets instead of logical ones simply composes a second
    // rotation, so the whole correction is a constant added to the bank angle --
    // no per-pixel work at all.
#if VG_ROTATE == 1
    const float bank_eff = s_bank + 1.57079633f;
#elif VG_ROTATE == 2
    const float bank_eff = s_bank + 3.14159265f;
#elif VG_ROTATE == 3
    const float bank_eff = s_bank - 1.57079633f;
#else
    const float bank_eff = s_bank;
#endif

    const float cb = cosf(bank_eff), sb = sinf(bank_eff);

    // 16.16 fixed point: the inner loop is two adds, two shift-masks and a
    // 32-bit store. Floats here would not survive the per-pixel budget.
    const int32_t dux = (int32_t)( cb * s_scale * 65536.0f);
    const int32_t dvx = (int32_t)( sb * s_scale * 65536.0f);
    const int32_t duy = (int32_t)(-sb * s_scale * 65536.0f);
    const int32_t dvy = (int32_t)( cb * s_scale * 65536.0f);

    const int32_t u_org = (int32_t)(s_u * 65536.0f);
    const int32_t v_org = (int32_t)(s_v * 65536.0f);

    // One sample per EIGHT pixels. At SKY_SCALE 0.10 a texel covers ten screen
    // pixels, so even an 8px span rarely straddles two texels.
    //
    // This is the single most expensive loop in the firmware. Measurement, not
    // guesswork: with the panel DMA wait reported separately it turns out to sit
    // at 5-8us per frame -- i.e. the transfer is NEVER what we are waiting on.
    // The band stage is purely CPU-bound, and ~11.5ms of it is this fill plus
    // the scanline pass, before a single line of geometry is drawn. Halving the
    // sampling rate here buys more frame time than any change to the vector
    // pipeline can.
    // That matters because the band stage is CPU-bound, not DMA-bound: at ~1.1ms
    // per band against a 0.768ms DMA window, every cycle here costs frame time.
    const int32_t du8 = dux * 8, dv8 = dvx * 8;

    // Ordered row dissolve. A bit-reversed sequence rather than a straight
    // threshold, so the backdrop materialises evenly across the screen instead
    // of wiping in from the top -- which would read as a loading bar.
    static const uint8_t ORDER[8] = { 0, 4, 2, 6, 1, 5, 3, 7 };

    for (int row = 0; row < BAND_H; row++) {
        const int sy = band_y0 + row;

        if (s_reveal < 1.0f &&
            (float)(ORDER[sy & 7] + 1) * (1.0f / 8.0f) > s_reveal) {
            memset(&band[row * SCR_W], 0, SCR_W * 2);
            continue;
        }

        int32_t u = u_org + dux * (int32_t)(-(int)SCR_CX) + duy * (int32_t)(sy - (int)SCR_CY);
        int32_t v = v_org + dvx * (int32_t)(-(int)SCR_CX) + dvy * (int32_t)(sy - (int)SCR_CY);

        const uint16_t* tex = s_tex;

        // Ordered dither on the sample coordinate. At a ~10x nearest upscale a
        // smooth gradient would otherwise band into visible 10px blocks; jogging
        // the lookup by up to half a texel turns those hard steps into a fine
        // stipple, which on a nebula just reads as grain.
        const int32_t* dth = &s_dither[(sy & 3) * 4];

        // One sample splashed across eight pixels as four 32-bit stores. The
        // stores are unavoidable -- every pixel of the band has to be written --
        // but the index arithmetic and the texture fetch are not, and those are
        // what this loop is actually paying for.
        uint32_t* dst = (uint32_t*)&band[row * SCR_W];
        for (int i = 0; i < SCR_W / 8; i++) {
            int32_t  o   = dth[i & 3];
            uint32_t idx = (uint32_t)(((((v + o) >> 16) & SKY_TEX_MASK) << SKY_TEX_BITS) |
                                       (((u + o) >> 16) & SKY_TEX_MASK));
            uint32_t c  = tex[idx];
            uint32_t cc = (c << 16) | c;
            dst[0] = cc;
            dst[1] = cc;
            dst[2] = cc;
            dst[3] = cc;
            dst += 4;
            u += du8;
            v += dv8;
        }
    }
}
