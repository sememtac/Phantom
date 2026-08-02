#include "vg_sky.h"
#include "vg_raster.h"
#include "vg_config.h"
#include "vg_capture.h"
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


// Ceiling on backdrop brightness. It must sit well below the vector art or it
// competes with the thing the player is actually looking at.
// Backdrop ceiling. At 0.80 galaxy cores peaked around 76/125 and thin HUD
// strokes lost contrast against them -- a backdrop that competes with the
// foreground is just noise.
#define SKY_MAX_LEVEL   0.52f

// ...and nothing may be perfectly black. See the lift in vg_sky_generate.
//
// These are the values an empty texel lifts TO, and they are cool rather than
// grey because empty space is not neutral. Chosen to survive the 5/6/5
// quantisation: below about 0.033 the red channel rounds back to zero and the
// floor does nothing at all.
#define SKY_FLOOR_R     0.040f
#define SKY_FLOOR_G     0.045f
#define SKY_FLOOR_B     0.075f

// Sampling scale and pan rate. One backdrop kind is left -- a cloud on a
// sphere -- so both are set to the identity below at every generate, and these
// initialisers only cover the window before the first sky exists.
static float s_scale = 0.05f;   // set per backdrop at generate
static float s_pan   = 20.4f;

// --- the sphere identity ------------------------------------------------------
//
// The tile IS the sky. One revolution of yaw is exactly one tile width, so the
// pan rate follows from the tile and not from the focal length:
//
//     texels per radian = SKY_TEX_SIZE / 2pi = 20.37
//
// That is what guarantees each feature appears ONCE. An earlier match sky
// panned at 30 texels a radian, which walks through 1.5 tiles in a full turn
// and showed the same landmark twice at two bearings.
//
// The scale follows from the pan, and ONLY from the pan:
//
//     texels per pixel = pan / FOCAL
//
// because a yaw of theta must move the backdrop theta*FOCAL pixels -- exactly
// what it moves the starfield. Anything else and the two slide against each
// other, which is the one cue that says an object is nearby: at 21.7/480 the
// sky ran 13% fast and the backdrop read as painted on the canopy rather than
// sitting at infinity.
//
// 21.7 texels was the width of the view measured ACROSS THE SPHERE, 61/360 of a
// tile. That is the right number for an equirectangular strip and the wrong one
// for a projection, which is linear in the tangent and not in the angle. The
// two agree only at the centre of the frame, and disagreeing by 13% everywhere
// else is exactly the error.
// Two constraints, neither negotiable:
//
//   pan   = TILE / 2pi        one revolution is exactly one tile, so the
//                             pole's branch switch (+pi of longitude) is
//                             exactly half a tile -- the SAME PICTURE, which
//                             is what makes crossing the zenith seamless
//   scale = pan / FOCAL       locked to the starfield, so translation and
//                             rotation compose rigidly at any attitude
//
// The old match constants satisfied neither, then only the second, and each
// gap was its own sky bug. The tile is the whole sky: no repeat is ever
// visible, and each venue's backdrop is a PLACE with its features at fixed
// bearings.
#define SKY_SPHERE_PAN   ((float)SKY_TEX_SIZE / 6.28318531f)
#define SKY_SPHERE_SCALE (SKY_SPHERE_PAN / FOCAL)

static uint16_t* s_tex   = nullptr;
static bool      s_ready = false;

static float s_u = 0.0f, s_v = 0.0f, s_bank = 0.0f;

// WHERE THE SHIP IS POINTING, as an orientation rather than as two integrated
// angles. Accumulated from the same Mat3 the stars and the arena ride, so the
// backdrop cannot drift against them however the ship is flown.
//
// s_u and s_v above are derived from this now, once a frame. They are kept
// because the sampler wants a plane position and because a cloud still tiles;
// what has changed is that they are read OFF an orientation instead of being
// the only record of one.
static Mat3 s_ori = {{ 1,0,0, 0,1,0, 0,0,1 }};

// ---------------------------------------------------------------------------
// The two-chart atlas.
//
// This replaced, in order: a raw sampler that flipped at the zenith, a
// longitude lock that let the roll spin instead, a paired lock that snapped on
// exit, and a rate-limited sweep that turned the snap into a visible rotation.
// Four attempts at suppressing the pole fold, each moving it somewhere else --
// because the fold is not an artefact, it is the chart. An equirectangular
// chart genuinely has two branches at the pole, and the OTHER branch is the
// continuation: longitude plus half a turn, latitude reflected through the
// pole, roll plus half a turn, all at once. Cross the pole, switch branch, and
// every sampled quantity is continuous by construction.
//
// So each view carries a PARITY -- which branch it is on -- flipped when the
// horizontal direction reverses inside a small cap around the pole, and the
// branch-adjusted angles are accumulated as unfolded totals. Pure pitch
// through vertical: longitude constant, roll constant, latitude climbing
// straight through -- the sky streams past with ZERO rotation, which is what
// a canopy shows in a loop. Tested in prototype over pitch loops, offset
// loops, rolled loops and five thousand frames of random tumble: worst
// per-frame step 0.098 rad, no discontinuity anywhere, and a pitch loop
// accumulates exactly one tile of latitude and lands home.
//
// The accumulators are per view because the mirror sits at the opposite pole.
// ---------------------------------------------------------------------------
static float s_eff_prev[2][3];       // last branch-adjusted lon/lat/roll
static float s_eff_acc[2][3];        // unfolded totals the fills sample from
static bool  s_par[2]   = { false, false };
static bool  s_incap[2] = { false, false };
static float s_capx[2], s_capz[2];   // horizontal direction at cap entry
static bool  s_snap     = true;      // first frame after generate: no history

static inline float ang_wrap(float d) {
    while (d >  3.14159265f) d -= 6.28318531f;
    while (d < -3.14159265f) d += 6.28318531f;
    return d;
}

static void sky_step_view(int li, float sign) {
    float dx = s_ori.m[6] * sign, dy = s_ori.m[7] * sign, dz = s_ori.m[8] * sign;
    if (dy >  1.0f) dy =  1.0f;
    if (dy < -1.0f) dy = -1.0f;

    const float lon = atan2f(dx, dz);
    const float lat = asinf(dy);
    // Roll sign convention: searched over tumbled attitudes when it was first
    // wrong -- see the history in git. Negated forward, plain astern.
    const float ux = s_ori.m[1], uy = s_ori.m[4];
    const float roll = atan2f(sign < 0.0f ? ux : -ux, uy);

    // The cap: within ~5 degrees of the pole. Entering stores the horizontal
    // direction; the pole has been CROSSED -- not merely approached -- when
    // that direction reverses, and that is the branch switch.
    const bool incap = (dx * dx + dz * dz) < 0.008f;
    if (incap) {
        if (!s_incap[li]) { s_capx[li] = dx; s_capz[li] = dz; }
        else if (dx * s_capx[li] + dz * s_capz[li] < 0.0f) {
            s_par[li]  = !s_par[li];
            s_capx[li] = dx; s_capz[li] = dz;
        }
    }
    s_incap[li] = incap;

    const float eff[3] = {
        lon  + (s_par[li] ? 3.14159265f : 0.0f),
        s_par[li] ? (3.14159265f - lat) : lat,
        roll + (s_par[li] ? 3.14159265f : 0.0f),
    };

    if (s_snap) {
        for (int k = 0; k < 3; k++) {
            s_eff_prev[li][k] = eff[k];
            s_eff_acc[li][k]  = eff[k];
        }
        return;
    }
    for (int k = 0; k < 3; k++) {
        float d = ang_wrap(eff[k] - s_eff_prev[li][k]);
        // A single pathological frame exactly on the pole can make the raw
        // longitude arbitrary. One clamped frame is invisible; a spike is not.
        if (d >  0.35f) d =  0.35f;
        if (d < -0.35f) d = -0.35f;
        s_eff_acc[li][k] += d;
        s_eff_prev[li][k] = eff[k];
    }
}

void vg_sky_orient(const Mat3& R, float bank) {
    s_ori  = mat3_mul(R, s_ori);
    s_bank = bank;
    sky_step_view(0,  1.0f);
    sky_step_view(1, -1.0f);
    s_snap = false;

    // WRAPPED, or the session gets slower by the lap. The accumulators are
    // unfolded totals and every circuit of the course adds a full turn; the
    // chart samples lift toward them through a while-loop that spins once per
    // turn of difference, so after half an hour of flying every one of two
    // hundred chart samples was paying dozens of iterations. A full turn of
    // longitude is exactly one tile under the sphere identity -- the same
    // sample -- so wrapping changes nothing but the arithmetic's size.
    for (int i = 0; i < 2; i++)
        for (int k = 0; k < 3; k++)
            s_eff_acc[i][k] = ang_wrap(s_eff_acc[i][k]);
}

// What the fills use: the unfolded totals. Latitude included -- v runs on past
// the fold and the tile wraps, which is exactly what lets a loop scroll one
// full tile and land home.
static void sky_sample(float sign, float* u, float* v, float* roll) {
    const int li = (sign < 0.0f) ? 1 : 0;
    *u    = s_u + s_eff_acc[li][0] * s_pan;
    *v    = s_v - s_eff_acc[li][1] * s_pan;
    *roll = s_eff_acc[li][2];
}
// Whether the next fill is for a camera looking aft. Set per frame by the
// renderer, because the backdrop has no camera of its own to ask.
static bool  s_rear = false;
void vg_sky_set_rear(bool on) { s_rear = on; }


// The rear-view patch, as a PANEL rectangle. Zero width means no patch this
// frame, which is the case in every menu and whenever the main window is
// already looking aft.
static int s_px0 = 0, s_py0 = 0, s_px1 = -1, s_py1 = -1;

void vg_sky_set_patch(int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) { s_px1 = -1; return; }
    vg_rast_rot_rect(&x, &y, &w, &h);     // the raster's own turn, not a copy
    s_px0 = x; s_py0 = y;
    s_px1 = x + w - 1; s_py1 = y + h - 1;
    if (s_px0 < 0) s_px0 = 0;
    if (s_py0 < 0) s_py0 = 0;
    if (s_px1 > SCR_W - 1) s_px1 = SCR_W - 1;
    if (s_py1 > SCR_H - 1) s_py1 = SCR_H - 1;
}

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

    // Reuse the texture if it already exists. This used to allocate every time
    // and leaked 32KB per call, which nobody noticed while vg_game_init() ran
    // once at boot. Recording and rendering a session both call it again, so
    // internal RAM fell 87KB -> 54KB -> 21KB across three sessions and then the
    // allocation failed and the backdrop disappeared.
    if (s_tex) {
        s_ready = false;        // the contents belong to an older sky
        return true;
    }

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

// The texture is kept, not freed. A menu is a place the player passes through
// on the way back to a fight, and reallocating 32KB of internal SRAM on every
// transit is a way to eventually not get it.

// The menu backdrop. A nebula rather than nothing, and always THIS one.
//
// Seed 256 is not arbitrary: name_place picks the proper noun with
// (seed >> 7) % 24, and 2 is ORION -- so the title screen is ORION NEBULA every
// time, and it is a place with a name rather than a random wash.
//
// Fixed rather than drawn from vg_replay_rand, which matters beyond taste: a
// menu that consumed a random number would put every recording's simulation on a
// different footing depending on how many menus were passed through.
#define SKY_MENU_SEED 256u
static bool s_is_menu = false;

void vg_sky_menu(void) {
    // Already up. Menu to menu is free; only a venue displaces it.
    //
    // But the VIEW still has to be put back, because skipping the generate skips
    // the reset inside it -- and vg_game_init calls through here at the start of
    // every replay. Without this the backdrop carries the idle tumble into the
    // run, which is exactly the drift the reset in vg_sky_generate exists to
    // stop.
    if (s_is_menu && s_ready) {
        s_ori  = Mat3{{ 1,0,0, 0,1,0, 0,0,1 }};
        s_snap = true;
        s_par[0]   = s_par[1]   = false;
        s_incap[0] = s_incap[1] = false;
        return;
    }
    vg_sky_generate(SKY_NEBULA, SKY_MENU_SEED);
    s_is_menu = true;
}

static inline uint16_t pack565_swapped(float r, float g, float b) {
    if (r < 0) r = 0; if (r > 1) r = 1;
    if (g < 0) g = 0; if (g > 1) g = 1;
    if (b < 0) b = 0; if (b > 1) b = 1;
    uint16_t v = (uint16_t)(((uint16_t)(r * 31.0f) << 11) |
                            ((uint16_t)(g * 63.0f) << 5)  |
                             (uint16_t)(b * 31.0f));
    return (uint16_t)((v >> 8) | (v << 8));
}

// ...and back. Needed because a backdrop can now be built in LAYERS: a cloud
// laid down first, then a landmark composited over what is already in the
// texture. Reading a texel back is cheaper than carrying a second full-size
// buffer to accumulate in, and this runs once per level, not per frame.
static inline void unpack565_swapped(uint16_t c, float* r, float* g, float* b) {
    const uint16_t v = (uint16_t)((c >> 8) | (c << 8));
    *r = (float)((v >> 11) & 0x1F) * (1.0f / 31.0f);
    *g = (float)((v >>  5) & 0x3F) * (1.0f / 63.0f);
    *b = (float)( v        & 0x1F) * (1.0f / 31.0f);
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
    // Same object in both, so the same word. The course is where the hole is
    // and the menu is a picture of it.
    const char* k = (s_kind == SKY_GALAXY)  ? "GALAXY"
                  : (s_kind == SKY_CLUSTER) ? "CLUSTER"
                                            : "NEBULA";
    snprintf(s_place, sizeof(s_place), "%s %s", p, k);
}

const char* vg_sky_name(void) {
    switch (s_kind) {
    case SKY_GALAXY:  return "GALAXY";
    case SKY_CLUSTER: return "CLUSTER";
    default:          return "NEBULA";
    }
}

void vg_sky_generate(SkyKind kind, uint32_t seed) {
    if (!s_tex) return;
    // Whatever this builds, it is not the menu's any more -- vg_sky_menu sets
    // the flag back after it calls through here.
    s_is_menu = false;

    uint32_t t0 = millis();

    // SKY_KINDS sits inside the enum as a count, so it is a reachable value and
    // not a valid kind -- the switch has to select the generator and the label
    // together rather than sanitising the input first.
    switch (kind) {
    case SKY_GALAXY:  s_kind = SKY_GALAXY;  gen_galaxy(seed);  break;
    case SKY_CLUSTER: s_kind = SKY_CLUSTER; gen_cluster(seed); break;
    default:          s_kind = SKY_NEBULA;  gen_nebula(seed);  break;
    }

    // THE FLOOR. No direction may come out perfectly black.
    //
    // The view is 21.7 texels wide under the sphere identity and the generators
    // mask out large empty regions on purpose, so an empty patch wider than the
    // view means the WHOLE SCREEN is black whichever way the ship points.
    // Measured on a nebula: a band about 14 texels across read 0 lit out of 144
    // sampled, and looking into it gave a dead panel. The rear-view patch shows
    // aft permanently, so it sat in one of these far more often than the main
    // window did -- which is what made a texture problem look like a broken
    // instrument.
    //
    // A LIFT, NOT A CLAMP. max(v, floor) leaves a hard edge exactly where the
    // mask cut off, which reads as a shape with a border; scaling the range up
    // from the floor keeps every gradient the generator drew and simply stops it
    // reaching zero. Before the pole and fold passes below, so both covers
    // inherit it and the two branches of a direction still agree.
    if (s_tex) {
        for (int i = 0; i < SKY_TEX_SIZE * SKY_TEX_SIZE; i++) {
            float r, g, b;
            unpack565_swapped(s_tex[i], &r, &g, &b);
            s_tex[i] = pack565_swapped(SKY_FLOOR_R + r * (1.0f - SKY_FLOOR_R),
                                       SKY_FLOOR_G + g * (1.0f - SKY_FLOOR_G),
                                       SKY_FLOOR_B + b * (1.0f - SKY_FLOOR_B));
        }
    }

    // AND THE VIEW ITSELF, not just the parity that rides on it.
    //
    // s_ori accumulates every world rotation and was only ever set at boot. A
    // render therefore began with whatever orientation the attract loop had
    // tumbled to while the host was getting organised -- so the backdrop started
    // the replay pointing somewhere that depended on how long the device had been
    // idling, and the same recording rendered a slightly different sky each time.
    //
    // It showed up as sixty to ninety pixels of a 230,400 pixel frame differing
    // by exactly one step of a 5-bit channel: one texel of sampling drift, in a
    // different place each run. Enough to change every frame hash, and enough to
    // look like a simulation that was not reproducible.
    s_ori = Mat3{{ 1,0,0, 0,1,0, 0,0,1 }};
    s_snap = true;    // a new backdrop is arrived at, not swept to
    // ...and on branch zero. Parity is flight history, and the attract loop
    // tumbles through the poles constantly -- the course was inheriting an odd
    // parity from the menu and sampling the far half of the tile, where there
    // is no hole and hardly any cloud. Arriving somewhere new has no history.
    s_par[0] = s_par[1] = false;
    s_incap[0] = s_incap[1] = false;
    name_place(seed);
    s_reveal = 1.0f;      // callers that want a dissolve ask for it afterwards

    // The sphere identity. Every backdrop is a sphere now -- the flat one went
    // with the menu that needed it. The clouds ran their own pan and scale for
    // as long as they only ever translated; the rear view, the poles and the
    // roll each exposed a fresh inconsistency, in that order, and this is the
    // last of the private constants gone.
    s_scale = SKY_SPHERE_SCALE;
    s_pan   = SKY_SPHERE_PAN;
    // THE ORIGIN MUST PUT THE EQUATOR THROUGH THE TILE CENTRE. "Any origin
    // will do" was true while clouds panned freely; the sphere ended it.
    // The generators author their features around (64,64) -- the galaxy's
    // bar, the nebula's mass -- and the fold pass builds the far cover by
    // overwriting everything outside the origin's own band. At (0,0) the
    // authored half WAS the far cover: the galaxy's centre was erased and
    // replaced with mirrored edge noise, the equator ran along the tile
    // seam, and pitching crawled the view across mirror copies -- the
    // backdrop that "would not orient".
    s_u = s_v = (float)(SKY_TEX_SIZE / 2);

    // THE POLE ROWS ARE ONE POINT EACH, and the texture has to converge to
    // say so. On the sphere every longitude meets at the zenith; on the tile
    // the zenith is a full row. Raw noise varies along that row, so pointing
    // at the pole showed structure sliding where there should be a single
    // colour -- the "rotation" seen at the zenith in matches. The course only
    // looked immune because its base cloud is dimmed and its pole is nearly
    // black. Standard equirect pinch: the last eight rows of latitude blend
    // toward their own row average, fully constant at the pole itself. Runs on
    // the canonical band before the fold copies it, so both covers inherit it.
    if (s_tex) {
        const int vc = (int)s_v & SKY_TEX_MASK;
        for (int v = 0; v < SKY_TEX_SIZE; v++) {
            int d = (v - vc) & 127;
            if (d >= 64) d -= 128;
            const int ad = (d < 0) ? -d : d;
            if (ad < 24 || ad > 32) continue;         // canonical high latitudes
            const float t = (float)(ad - 24) / 8.0f;  // 0 at 67 deg, 1 at pole
            // Row average, in linear channel space.
            uint32_t sr = 0, sg = 0, sb = 0;
            for (int u = 0; u < SKY_TEX_SIZE; u++) {
                const uint16_t c = s_tex[(v << SKY_TEX_BITS) | u];
                const uint16_t n = (uint16_t)((c >> 8) | (c << 8));
                sr += (n >> 11) & 0x1F; sg += (n >> 5) & 0x3F; sb += n & 0x1F;
            }
            const float ar = sr / 128.0f, ag = sg / 128.0f, ab = sb / 128.0f;
            for (int u = 0; u < SKY_TEX_SIZE; u++) {
                const uint16_t c = s_tex[(v << SKY_TEX_BITS) | u];
                const uint16_t n = (uint16_t)((c >> 8) | (c << 8));
                float r = (n >> 11) & 0x1F, g = (n >> 5) & 0x3F, b2 = n & 0x1F;
                r += (ar - r) * t; g += (ag - g) * t; b2 += (ab - b2) * t;
                const uint16_t o = (uint16_t)(((uint16_t)(r + 0.5f) << 11) |
                                              ((uint16_t)(g + 0.5f) << 5)  |
                                               (uint16_t)(b2 + 0.5f));
                s_tex[(v << SKY_TEX_BITS) | u] = (uint16_t)((o >> 8) | (o << 8));
            }
        }
    }


    // THE TILE IS A DOUBLE COVER, and the texture has to say so. The unfolded
    // atlas maps a full turn of pitch to the full tile height, so every
    // direction owns TWO texels: (u,v) and its fold (u+64, 64-v). A real
    // equirectangular sphere map carries that symmetry; a noise texture does
    // not, so the same bearing showed different sky depending on how many
    // poles the flight had crossed -- and an Immelmann made a landmark vanish.
    // Half the tile is canonical; the other half is overwritten with its fold,
    // so both branches of every direction read the same texel. The dual row
    // formula assumes s_v is 0 or 64, which the origin is.
    if (s_tex) {
        const int vc = (int)s_v & SKY_TEX_MASK;
        for (int v = 0; v < SKY_TEX_SIZE; v++) {
            int d = (v - vc) & 127;                  // wrapped offset from s_v
            if (d >= 64) d -= 128;                   // -> [-64, 63]
            const bool owned = (d > -32 && d <= 32); // the canonical half
            if (owned) continue;
            const int vd = (64 - v) & SKY_TEX_MASK;
            for (int u = 0; u < SKY_TEX_SIZE; u++)
                s_tex[(v << SKY_TEX_BITS) | u] =
                    s_tex[(vd << SKY_TEX_BITS) | ((u + 64) & SKY_TEX_MASK)];
        }
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
    // after the setup above it -- so it quietly undid the centring and left the
    // fill sampling texel (0,0), the corner of the tile. The texture was
    // perfect the whole time; it was simply being read from the empty quarter.
    s_bank = 0.0f;
    s_ready = true;

    // Origin and scale are in the report for the same reason lit and peak are:
    // a backdrop pointed at the wrong part of its own texture looks exactly like
    // one that was never generated.
    // Silent while the link carries frames. A venue is generated mid-session
    // -- at the start of every match -- so this line lands between two bands
    // and the host reads it as pixel data.
    if (vg_link_busy()) return;
    Serial.printf("vg_sky_generate: %s seed %u in %ums  lit %d%%  peak %u/125"
                  "  uv %.0f,%.0f  scale %.3f\n",
                  vg_sky_name(), (unsigned)seed, (unsigned)(millis() - t0),
                  lit * 100 / (SKY_TEX_SIZE * SKY_TEX_SIZE), (unsigned)peak,
                  (double)s_u, (double)s_v, (double)s_scale);
}

void vg_sky_step(float d_pitch, float d_yaw, float bank) {
    // Kept for the cutscene, which tumbles the backdrop by hand and has no
    // world rotation to hand over. Same convention as vg_game builds its own R
    // with, so the two cannot disagree about which way is which.
    vg_sky_orient(mat3_euler(-d_pitch, -d_yaw, 0.0f), bank);
}

// Fast inverse trig for the per-band chart samples. Accurate to ~0.0004 rad,
// which is a sixth of a pixel; the libm versions cost several times more and
// this runs 270 times a frame inside the CPU-bound flush.
static inline float fatan2(float y, float x) {
    const float ax = fabsf(x), ay = fabsf(y);
    const float mx = ax > ay ? ax : ay;
    const float mn = ax > ay ? ay : ax;
    if (mx <= 0.0f) return 0.0f;
    const float a = mn / mx;
    const float ss = a * a;
    float r = ((-0.0464964749f * ss + 0.15931422f) * ss - 0.327622764f) * ss * a + a;
    if (ay > ax) r = 1.57079633f - r;
    if (x < 0.0f) r = 3.14159265f - r;
    return (y < 0.0f) ? -r : r;
}
static inline float fasin(float y) {
    if (y >  1.0f) y =  1.0f;
    if (y < -1.0f) y = -1.0f;
    return fatan2(y, sqrtf(1.0f - y * y));
}

// One exact chart sample: LOGICAL screen point -> (u, v) texels, lifted onto
// the atlas accumulators so it agrees with the branch the frame is on.
static float s_cb = 1.0f, s_sb = 0.0f;   // bank trig, hoisted per fill call

static void sky_chart(float lx, float ly, int li, float sign,
                      const float* ref_lon, const float* ref_lat,
                      float* out_u, float* out_v) {
    // Screen -> projection plane, undoing the cosmetic bank the projection
    // applies, then screen-down y becomes view-up y.
    const float sx = lx - (float)SCR_CX, sy = ly - (float)SCR_CY;
    const float cb = s_cb, sb = s_sb;
    const float px = sx * cb + sy * sb;
    const float py = -sx * sb + sy * cb;
    float vx = px / FOCAL, vy = -py / FOCAL, vz = 1.0f;
    const float inv = 1.0f / sqrtf(vx * vx + vy * vy + vz * vz);
    vx *= inv; vy *= inv; vz *= inv;

    // View ray -> sky direction, through the accumulated orientation; the rear
    // view looks along the negated ray.
    float dx = (s_ori.m[0] * vx + s_ori.m[3] * vy + s_ori.m[6] * vz) * sign;
    float dy = (s_ori.m[1] * vx + s_ori.m[4] * vy + s_ori.m[7] * vz) * sign;
    float dz = (s_ori.m[2] * vx + s_ori.m[5] * vy + s_ori.m[8] * vz) * sign;

    float lon = fatan2(dx, dz);
    float lat = fasin(dy);
    if (s_par[li]) { lon += 3.14159265f; lat = 3.14159265f - lat; }
    // Lifted toward the CALLER'S reference, not the frame accumulator. Every
    // sample lifting independently to the accumulator meant two samples
    // straddling the pi wrap could land a full turn apart, and the derivative
    // between them -- 0.8 texels per PIXEL of garbage -- extrapolated a whole
    // band into dashes. The caller chains the reference sample to sample, so
    // neighbours are always lifted onto the same turn.
    *out_u = *ref_lon + ang_wrap(lon - *ref_lon);
    *out_v = *ref_lat + ang_wrap(lat - *ref_lat);
}

void vg_sky_fill_band(uint16_t* band, int band_y0) {
    if (!s_ready) return;

    // THE SPHERE, PIECEWISE. The old fill mapped the whole frame as one rigid
    // sheet -- centre angles plus a rotation -- which is exact at the centre
    // and up to 115 PIXELS wrong at the edges with the nose 30 degrees up in a
    // yaw, where the sphere's meridians shear across the view. The stars are
    // projected point by point, so they carry the true curvature; a sheet
    // cannot, and the mismatch read as the backdrop rotating on the view axis.
    //
    // So the chart is now evaluated exactly at nine points down this band's
    // centre column and nine more a step across, and the fill interpolates
    // between them: fifteen bands by eight segments of bilinear patch. Against
    // per-pixel evaluation that is at most 8px wrong anywhere the texture is
    // not the pinched pole wash -- star-grade, at eighteen cheap samples a
    // band.
    //
    // Bands are PANEL rows, which under the quarter turn are logical COLUMNS:
    // this band is 32 columns wide and the segments run down the 480-pixel
    // logical height, walked by the inner loop along panel x.
    const int   li   = s_rear ? 1 : 0;
    const float sign = s_rear ? -1.0f : 1.0f;
#if VG_ROTATE == 1
    const float lx_c = (float)(SCR_H - 1 - (band_y0 + BAND_H / 2));
#else
    const float lx_c = (float)(SCR_W / 2);   // other rotations: centre column
#endif

    // Ten, not eight: the segment length must divide by the 8-pixel splash or
    // the walk leaves a gap of unwritten pixels at every segment boundary --
    // 480/8 segments is 60px, 60/8 rounds to 7 chunks, and the missing 4px
    // showed the previous band's leftovers as amber dashes every 60 pixels.
    // 480/10 is 48, which is six exact chunks.
    enum { SEGS = 10 };
    float Au[SEGS + 1], Av[SEGS + 1], Cu[SEGS + 1], Cv[SEGS + 1];
    // Chained lift: the first sample references the frame accumulator, each
    // later one references its predecessor, and each cross sample references
    // its own column sample. In ANGLE space, then scaled to texels once.
    // Bank trig once per band, not once per sample: cosf twice against
    // twenty-two times, and it was two milliseconds of the flight profile.
    s_cb = cosf(s_bank); s_sb = sinf(s_bank);

    float rl = s_eff_acc[li][0], rt = s_eff_acc[li][1];
    for (int k = 0; k <= SEGS; k++) {
        const float ly = (float)(k * (SCR_H / SEGS));
        sky_chart(lx_c, ly, li, sign, &rl, &rt, &Au[k], &Av[k]);
        rl = Au[k]; rt = Av[k];
    }
    // Cross-derivatives sampled at three stations and interpolated: the
    // across-band slope varies slowly, and eight chart calls a band were a
    // measurable slice of the two milliseconds this fill cost in flight.
    {
        float cu3[3], cv3[3];
        for (int j = 0; j < 3; j++) {
            const int   k  = j * (SEGS / 2);
            const float ly = (float)(k * (SCR_H / SEGS));
            float rl2 = Au[k], rt2 = Av[k];
            sky_chart(lx_c - 8.0f, ly, li, sign, &rl2, &rt2, &cu3[j], &cv3[j]);
        }
        // The stations give the cross OFFSET (C minus A) at three latitudes;
        // each segment endpoint gets its A plus the interpolated offset.
        const float du[3] = { cu3[0] - Au[0], cu3[1] - Au[SEGS / 2], cu3[2] - Au[SEGS] };
        const float dv[3] = { cv3[0] - Av[0], cv3[1] - Av[SEGS / 2], cv3[2] - Av[SEGS] };
        for (int k = 0; k <= SEGS; k++) {
            const float t = (float)k / (float)SEGS * 2.0f;   // 0..2 over 3 stations
            const int   j = (t < 1.0f) ? 0 : 1;
            const float f = t - (float)j;
            Cu[k] = Au[k] + du[j] + (du[j + 1] - du[j]) * f;
            Cv[k] = Av[k] + dv[j] + (dv[j + 1] - dv[j]) * f;
        }
    }
    for (int k = 0; k <= SEGS; k++) {
        Au[k] = s_u + Au[k] * s_pan;  Av[k] = s_v - Av[k] * s_pan;
        Cu[k] = s_u + Cu[k] * s_pan;  Cv[k] = s_v - Cv[k] * s_pan;
    }

    // The cross OFFSET per endpoint, banked once for the whole band. The row
    // loop used to rebuild (A - C) for four endpoints of every segment, which
    // is the same eleven subtractions repeated thirty-two times.
    float Du[SEGS + 1], Dv[SEGS + 1];
    for (int k = 0; k <= SEGS; k++) {
        Du[k] = Au[k] - Cu[k];
        Dv[k] = Av[k] - Cv[k];
    }

    static const uint8_t ORDER[8] = { 0, 4, 2, 6, 1, 5, 3, 7 };
    const int seg_px = SCR_H / SEGS;
    // Pixels painted from one texture sample. The chart is evaluated per
    // segment either way, so this trades backdrop detail against the per-chunk
    // overhead -- the stores are the same 460KB a frame at any width. seg_px is
    // 48, so 8 and 16 both divide exactly and neither leaves the unwritten gap
    // the SEGS comment above warns about.
    enum { SPLASH = 16 };
    // One constant for the step: the 8-pixel splash over the segment length,
    // into 16.16. Folding the two multiplies into one is bit-exact rather than
    // merely close, because 65536 is a power of two -- scaling by it moves the
    // exponent and leaves the mantissa alone, so it cannot change a rounding.
    // That matters: a replay must render frame for frame.
    const float step_k = ((float)SPLASH / (float)seg_px) * 65536.0f;

    // ROWS IN PAIRS. A splash of 16 across and 1 down is a ribbon, and the eye
    // reads the anisotropy before it reads the coarseness -- 16x2 costs the same
    // as 16x1 did against 8x1 and looks less like a smear. The second row of
    // each pair is a copy, so a pair costs one walk plus a 960-byte memcpy
    // instead of two walks.
    //
    // Per-row while the backdrop is dissolving in: the reveal pattern is keyed
    // on ORDER[sy & 7], so the two rows of a pair are not always on the same
    // side of the threshold and copying one onto the other would coarsen the
    // dissolve to 2px steps. It runs for under a second at a screen change.
    const int row_step = (s_reveal < 1.0f) ? 1 : 2;

    for (int row = 0; row < BAND_H; row += row_step) {
        const int sy = band_y0 + row;
        if (s_reveal < 1.0f &&
            (float)(ORDER[sy & 7] + 1) * (1.0f / 8.0f) > s_reveal) {
            memset(&band[row * SCR_W], 0, SCR_W * 2);
            continue;
        }

#if VG_ROTATE == 1
        const float dxl = (float)(SCR_H - 1 - sy) - lx_c;
#else
        const float dxl = 0.0f;
#endif
        // THE DITHER ROW, and it must advance per DRAWN row, not per screen row.
        // With rows paired, sy steps by two, so (sy & 3) only ever produced two
        // of the four Bayer phases and the pattern lost half its vertical
        // resolution -- which is half of why the backdrop started banding.
        const int32_t* dth =
            &s_dither[(((row_step == 2) ? (sy >> 1) : sy) & 3) * 4];
        uint16_t* dst = &band[row * SCR_W];
        const uint16_t* tex = s_tex;

        // Boundary tint, applied to the chunk colour as it is written -- one
        // word op per chunk against the dead pass's two hundred and forty per
        // row. Ring boundaries are half-widths from the row's own geometry;
        // walking x, the ring index just steps at each crossing.
        int lim[VG_TINT_RINGS + 1];
        const bool tint = vg_tint_active();
        int ring = -1;                    // carried across the row's chunks
        if (tint) vg_tint_row_limits(sy, lim);

        // dxl/8 is exact -- 8 is a power of two -- so the reciprocal form is
        // bit-identical, and it leaves the division out of the segment loop.
        const float w = dxl * 0.125f;
        // A segment's END endpoint is the next segment's START endpoint, the
        // same expression to the bit, so the walk carries it instead of
        // computing it twice. Worth 170us of the fill, measured: the compiler
        // had already found most of what looked like a much larger saving.
        float su = Au[0] + Du[0] * w;
        float sv = Av[0] + Dv[0] * w;

        for (int k = 0; k < SEGS; k++) {
            // Endpoint values for THIS row: centre-column sample plus the
            // cross-derivative carried dxl columns over. The step between the
            // endpoints is the segment's own slope, so the walk is bilinear.
            const float eu = Au[k + 1] + Du[k + 1] * w;
            const float ev = Av[k + 1] + Dv[k + 1] * w;

            int32_t u   = (int32_t)(su * 65536.0f);
            int32_t v   = (int32_t)(sv * 65536.0f);
            const int32_t du8 = (int32_t)((eu - su) * step_k);
            const int32_t dv8 = (int32_t)((ev - sv) * step_k);

            uint32_t* d32 = (uint32_t*)(dst + k * seg_px);
            // The dither column counts chunks ALONG THE WHOLE ROW, not within the
            // segment. At a splash of 8 there were six chunks a segment and
            // (i & 3) happened to walk the pattern; at 16 there are three, so it
            // used phases 0,1,2 and never 3, and reset every 48 pixels. A
            // three-phase pattern repeating on a 48-pixel pitch is not a dither,
            // it is a stripe -- the other half of why the backdrop started
            // banding after the fill was widened.
            const int chunk0 = k * (seg_px / SPLASH);
            for (int i = 0; i < seg_px / SPLASH; i++) {
                const int32_t  o   = dth[(chunk0 + i) & 3];
                const uint32_t idx = (uint32_t)(((((v + o) >> 16) & SKY_TEX_MASK) << SKY_TEX_BITS) |
                                                 (((u + o) >> 16) & SKY_TEX_MASK));
                const uint32_t c  = tex[idx];
                uint32_t cc = (c << 16) | c;
                if (tint) {
                    // The ring index walks WITH x instead of restarting at
                    // every chunk -- restarting was thirteen compares a chunk,
                    // three milliseconds a frame parked against a wall.
                    // |dx| falls to the screen centre then rises, so the index
                    // steps at ring crossings and is amortised constant.
                    const int adx = abs(k * seg_px + i * SPLASH + SPLASH / 2 - SCR_W / 2);
                    while (ring >= 0 && adx < lim[ring]) ring--;
                    while (ring + 1 < VG_TINT_RINGS && adx >= lim[ring + 1]) ring++;
                    if (ring >= 0)
                        cc = vg_tint_word(cc, ring >= VG_TINT_RINGS ? VG_TINT_RINGS - 1 : ring);
                }
                for (int q = 0; q < SPLASH / 2; q += 4) {
                    d32[q] = cc; d32[q + 1] = cc; d32[q + 2] = cc; d32[q + 3] = cc;
                }
                d32 += SPLASH / 2;
                u += du8;
                v += dv8;
            }
            su = eu; sv = ev;
        }

        // The pair's second row. BAND_H is 32 and band_y0 is a multiple of it,
        // so pairs never straddle a band boundary and the phase is the same in
        // every band.
        if (row_step == 2) memcpy(&band[(row + 1) * SCR_W], dst, SCR_W * 2);
    }
}

// The patch's backdrop, called from the band raster when it meets a PRIM_SKY.
    // --- the rear-view patch ------------------------------------------------
    //
    // A second pass over its rows, because the backdrop is not geometry and the
    // submit-time viewport cannot reach it: this fill REPLACES the band clear,
    // which is exactly why it costs nothing and exactly why the patch had a
    // hole in it.
    //
    // Three things differ from the pass above and nothing else does.
    //
    //   scale   the patch shows the same field of view as the main window in
    //           REAR_W pixels instead of SCR_W, so a pixel covers 480/145 as
    //           much sky. Isotropic, so the quarter turn does not enter into it.
    //   origin  the PATCH centre maps to the sample point, not the screen's.
    //   heading half a turn, always -- the patch is the aft view by definition,
    //           and it is not drawn at all when the main window is already aft.
    //
    // Per pixel rather than one sample per eight. The patch is 44 panel columns
    // wide, so the whole pass is a few thousand pixels against the 230,400 above
    // it, and at this scale a texel covers three pixels instead of ten -- eight
    // would be sampling coarser than the texture.
void vg_sky_fill_patch(uint16_t* band, int band_y0) {
    if (!s_ready || s_px1 < s_px0) return;

    float su, sv, sky_roll;
    sky_sample(-1.0f, &su, &sv, &sky_roll);

// The real roll is in the orientation; s_bank is the cosmetic lean the
// projection adds on top. Both, or the backdrop slides against the starfield.
#if VG_ROTATE == 1
    const float bank_eff = sky_roll + s_bank + 1.57079633f;
#elif VG_ROTATE == 2
    const float bank_eff = sky_roll + s_bank + 3.14159265f;
#elif VG_ROTATE == 3
    const float bank_eff = sky_roll + s_bank - 1.57079633f;
#else
    const float bank_eff = sky_roll + s_bank;
#endif
    const float cb = cosf(bank_eff), sb = sinf(bank_eff);

    {
        int r0 = s_py0 - band_y0, r1 = s_py1 - band_y0;
        if (r0 < 0) r0 = 0;
        if (r1 > BAND_H - 1) r1 = BAND_H - 1;

        if (r1 >= r0) {
            const float ps = s_scale / REAR_FOCAL_K;
            const int32_t pdux = (int32_t)( cb * ps * 65536.0f);
            const int32_t pdvx = (int32_t)( sb * ps * 65536.0f);
            const int32_t pduy = (int32_t)(-sb * ps * 65536.0f);
            const int32_t pdvy = (int32_t)( cb * ps * 65536.0f);

            const int32_t pu = (int32_t)(su * 65536.0f);
            const int32_t pv = (int32_t)(sv * 65536.0f);

            const int pcx = (s_px0 + s_px1) / 2;
            const int pcy = (s_py0 + s_py1) / 2;

            for (int row = r0; row <= r1; row++) {
                const int sy = band_y0 + row;
                int32_t u = pu + pdux * (int32_t)(s_px0 - pcx)
                               + pduy * (int32_t)(sy - pcy);
                int32_t v = pv + pdvx * (int32_t)(s_px0 - pcx)
                               + pdvy * (int32_t)(sy - pcy);
                uint16_t* dst = &band[row * SCR_W + s_px0];
                for (int x = s_px0; x <= s_px1; x++) {
                    *dst++ = s_tex[(((v >> 16) & SKY_TEX_MASK) << SKY_TEX_BITS) |
                                    ((u >> 16) & SKY_TEX_MASK)];
                    u += pdux;
                    v += pdvx;
                }
            }
        }
    }
}
