#include "vg_draw.h"
#include "vg_game.h"
#include <stdio.h>
#include <math.h>

// Full-screen state overlays: the title card, the damage vignette, and the
// between-states text. Deliberately separate from the HUD -- these appear and
// vanish with game state rather than being instruments that are always present.

static void centred(int y, const char* s, uint16_t col, int scale) {
    vg_text((SCR_W - vg_text_width(s, scale)) / 2, y, s, col, scale);
}

static void draw_damage_vignette(void) {
    if (vg.hit_flash <= 0) return;
    float f = vg.hit_flash / 0.6f;
    if (f > 1.0f) f = 1.0f;
    uint16_t c = vg_dim(COL_DANGER, f);
    const int t = 9;
    vg_fill_rect(0, 0, SCR_W, t, c);
    vg_fill_rect(0, SCR_H - t, SCR_W, t, c);
    vg_fill_rect(0, 0, t, SCR_H, c);
    vg_fill_rect(SCR_W - t, 0, t, SCR_H, c);
}

static uint32_t glitch_hash(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352dU;
    x ^= x >> 15; x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

// The title card glitches in short bursts: characters jump out of line, a dim
// ghost separates sideways like a mistimed signal, occasional letters corrupt,
// and tear bars cut across. Driven entirely by hashing a time bucket, so it needs
// no per-frame state and repeats deterministically.
static void draw_glitch_title(const char* s, int y, int scale) {
    const uint32_t bucket = (uint32_t)(vg.state_t * 9.0f);
    const bool     glitch = (glitch_hash(bucket) % 100u) < 22u;

    int n = 0;
    while (s[n]) n++;
    const int adv = 6 * scale;
    const int x0  = (SCR_W - vg_text_width(s, scale)) / 2;

    if (!glitch) {
        vg_text(x0, y, s, INK_BRIGHT, scale);
        return;
    }

    int gx = (int)(glitch_hash(bucket * 7u) % 13u) - 6;
    vg_text(x0 + gx, y, s, INK_TRACE, scale);

    for (int i = 0; i < n; i++) {
        uint32_t h  = glitch_hash(bucket * 31u + (uint32_t)i);
        int      dx = (int)(h % 9u) - 4;
        int      dy = (int)((h >> 8) % 5u) - 2;
        char     c  = s[i];
        if (((h >> 16) % 11u) == 0u) c = (char)('A' + ((h >> 20) % 26u));
        char one[2] = { c, 0 };
        vg_text(x0 + i * adv + dx, y + dy, one, INK_MAX, scale);
    }

    int bars = (int)(glitch_hash(bucket * 13u) % 3u);
    for (int k = 0; k < bars; k++) {
        uint32_t h  = glitch_hash(bucket * 101u + (uint32_t)k);
        int      ty = y + (int)(h % (uint32_t)(7 * scale));
        int      tw = 70 + (int)((h >> 8) % 190u);
        int      tx = (int)((h >> 16) % (uint32_t)(SCR_W - tw));
        vg_fill_rect(tx, ty, tw, 2, INK);
    }
}

void vg_draw_overlays(void) {
    char buf[40];

    draw_damage_vignette();

    switch (vg.state) {
    case VG_ATTRACT:
        draw_glitch_title("PHANTOM", 150, 7);
        if (fmodf(vg.state_t, 1.2f) < 0.8f)
            centred(285, "TOUCH TO START", INK_MAX, 3);
        break;

    case VG_HIT:
        if (fmodf(vg.state_t, 0.5f) < 0.3f)
            centred(200, "DAMAGE", COL_DANGER, 5);
        snprintf(buf, sizeof(buf), "HULL %d", (int)(vg.health * 100.0f + 0.5f));
        centred(258, buf, INK_MAX, 3);
        break;

    case VG_OVER:
        centred(170, "GAME OVER", COL_DANGER, 5);
        snprintf(buf, sizeof(buf), "SCORE %d", vg.score);
        centred(236, buf, COL_HUD, 3);
        snprintf(buf, sizeof(buf), "KILLS %d", vg.kills);
        centred(272, buf, COL_HUD_DIM, 2);
        if (vg.state_t > 1.0f && fmodf(vg.state_t, 1.0f) < 0.6f)
            centred(316, "TAP TO RESTART", COL_STAR_BRIGHT, 2);
        break;

    default:
        break;
    }
}
