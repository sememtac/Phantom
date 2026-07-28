#include "vg_draw.h"
#include "vg_game.h"
#include "vg_tourney.h"
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

// What the last player missile did. A proximity fuse is otherwise ambiguous --
// a detonation off the target's wing looks identical whether it took a third of
// their hull or nothing at all, and with damage now scaling by how close it went
// off, "nothing at all" is a real outcome the player needs to see.
static void draw_missile_banner(void) {
    if (vg.msl_event == MSL_NONE || vg.msl_event_t <= 0.0f) return;

    const char* s;
    uint16_t    col;
    switch (vg.msl_event) {
    case MSL_DESTROYED: s = "DESTROYED"; col = INK_MAX;    break;
    case MSL_HIT:       s = "HIT";       col = INK_BRIGHT; break;
    default:            s = "MISSED";    col = INK_FAINT;  break;
    }

    // Blinks for the first moment, then holds -- inversion and blink are how
    // this interface shouts, so a kill announces itself without a second hue.
    if (vg.msl_event > MSL_MISSED && vg.msl_event_t > 0.75f &&
        fmodf(vg.msl_event_t, 0.16f) < 0.08f)
        return;

    const int scale = (vg.msl_event == MSL_DESTROYED) ? 4 : 3;
    centred(186, s, col, scale);
}

void vg_draw_overlays(void) {
    char buf[40];

    draw_damage_vignette();
    if (vg.state == VG_PLAYING || vg.state == VG_HIT) draw_missile_banner();

    switch (vg.state) {
    case VG_ATTRACT:
        draw_glitch_title("PHANTOM", 170, 7);
        if (fmodf(vg.state_t, 1.2f) < 0.8f)
            centred(290, "TOUCH TO START", INK_MAX, 3);
        break;

    case VG_ROUND_WON:
        centred(160, "ROUND WON", INK_MAX, 5);
        snprintf(buf, sizeof(buf), "HULL %d/%d",
                 (int)(vg.health + 0.5f), (int)(vg.health_max + 0.5f));
        centred(232, buf, INK_BRIGHT, 3);
        snprintf(buf, sizeof(buf), "+%d CR", vg_last_purse());
        centred(276, buf, INK_MAX, 4);
        snprintf(buf, sizeof(buf), "BANK %d", vg.credits);
        centred(320, buf, INK_FAINT, 2);
        break;

    case VG_WON:
        draw_glitch_title("CHAMPION", 150, 6);
        snprintf(buf, sizeof(buf), "%s  HULL %d/%d", vg.spec->name,
                 (int)(vg.health + 0.5f), (int)(vg.health_max + 0.5f));
        centred(240, buf, INK_BRIGHT, 2);
        if (vg.state_t > 1.5f && fmodf(vg.state_t, 1.0f) < 0.6f)
            centred(300, "TAP TO CONTINUE", INK_MAX, 2);
        break;

    case VG_HIT:
        if (fmodf(vg.state_t, 0.5f) < 0.3f)
            centred(200, "DAMAGE", COL_DANGER, 5);
        snprintf(buf, sizeof(buf), "HULL %d",
                 (int)(vg.health / vg.health_max * 100.0f + 0.5f));
        centred(258, buf, INK_MAX, 3);
        break;

    case VG_OVER:
        centred(160, "ELIMINATED", COL_DANGER, 5);
        snprintf(buf, sizeof(buf), "OUT IN THE %s", vg_tourney_round_name(vt.round));
        centred(226, buf, COL_HUD, 2);
        snprintf(buf, sizeof(buf), "SCORE %d", vg.score);
        centred(262, buf, COL_HUD_DIM, 2);
        if (vg.state_t > 1.2f && fmodf(vg.state_t, 1.0f) < 0.6f)
            centred(316, "TAP TO RETURN", COL_STAR_BRIGHT, 2);
        break;

    default:
        break;
    }
}
