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
static void draw_glitch_title(const char* s, int y, int scale, float a) {
    if (a <= 0.01f) return;

    const uint32_t bucket = (uint32_t)(vg.state_t * 9.0f);
    const bool     glitch = (glitch_hash(bucket) % 100u) < 22u;

    int n = 0;
    while (s[n]) n++;
    const int adv = 6 * scale;
    const int x0  = (SCR_W - vg_text_width(s, scale)) / 2;

    if (!glitch) {
        vg_text(x0, y, s, vg_dim(INK_BRIGHT, a), scale);
        return;
    }

    int gx = (int)(glitch_hash(bucket * 7u) % 13u) - 6;
    vg_text(x0 + gx, y, s, vg_dim(INK_TRACE, a), scale);

    for (int i = 0; i < n; i++) {
        uint32_t h  = glitch_hash(bucket * 31u + (uint32_t)i);
        int      dx = (int)(h % 9u) - 4;
        int      dy = (int)((h >> 8) % 5u) - 2;
        char     c  = s[i];
        if (((h >> 16) % 11u) == 0u) c = (char)('A' + ((h >> 20) % 26u));
        char one[2] = { c, 0 };
        vg_text(x0 + i * adv + dx, y + dy, one, vg_dim(INK_MAX, a), scale);
    }

    int bars = (int)(glitch_hash(bucket * 13u) % 3u);
    for (int k = 0; k < bars; k++) {
        uint32_t h  = glitch_hash(bucket * 101u + (uint32_t)k);
        int      ty = y + (int)(h % (uint32_t)(7 * scale));
        int      tw = 70 + (int)((h >> 8) % 190u);
        int      tx = (int)((h >> 16) % (uint32_t)(SCR_W - tw));
        vg_fill_rect(tx, ty, tw, 2, vg_dim(INK, a));
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
    default:            s = "MISSED";    col = INK;        break;
    }

    // Blinks for the first moment, then holds. Inversion and blink are how this
    // interface shouts, so a kill announces itself without spending a hue.
    if (vg.msl_event > MSL_MISSED && vg.msl_event_t > 0.75f &&
        fmodf(vg.msl_event_t, 0.16f) < 0.08f)
        return;

    // Solid block with the label knocked out of it, the way an aircraft caution
    // annunciator reads. It is the strongest thing a single-hue interface can
    // do, which suits a message the player must not miss mid-turn.
    const int scale = 3;
    const int tw    = vg_text_width(s, scale);
    const int bw    = tw + 40;
    const int bh    = 7 * scale + 20;
    const int bx    = (SCR_W - bw) / 2;
    const int by    = 178;

    vg_fill_rect(bx, by, bw, bh, col);
    // INK_ONFILL rather than COL_BLACK: vg_text discards colour 0 outright, so
    // a black label would leave nothing but the block.
    vg_text(bx + 20, by + 10, s, INK_ONFILL, scale);

    // Outer rule, standing off the block -- the bracketed-caution motif.
    vg_rect(bx - 7, by - 7, bw + 14, bh + 14, col);
}

// ---------------------------------------------------------------------------
// Backstory
//
// Wrapped by hand rather than at runtime: the font is fixed width, the screen
// is a known 480, and a word-wrapper would be more code than the text it wraps.
// Uppercase throughout because the 5x7 face has no lower case.
// ---------------------------------------------------------------------------

static const char* const STORY[] = {
    "IN THE DISTANT FUTURE, THE",
    "INTERGALACTIC FEDERATION OF",
    "TERRA SANCTIONED A BRUTAL",
    "NEW PROGRAM: A FIGHT-TO-THE-",
    "DEATH SPACE COMBAT TOURNAMENT",
    "DESIGNED TO BOOST VIEWERSHIP",
    "AND TAX REVENUES.",
    "",
    "YOU ARE ONE OF THE FEW",
    "CONTESTANTS WHO HAS SURVIVED",
    "TO REACH THE FINAL STAGES.",
    "",
    "BUT A DARK RUMOR CIRCULATES",
    "THROUGH THE HANGAR BAYS.",
    "WHISPERS OF AN ELITE PILOT",
    "WHO MERCILESSLY HUNTS DOWN",
    "THE INEXPERIENCED.",
    "",
    "THEY CALL HIM...",
    "",
    "PHANTOM",
};
#define STORY_LINES ((int)(sizeof(STORY) / sizeof(STORY[0])))

#define TITLE_Y       150     // where the game title lives, and where the crawl ends
#define TITLE_SCALE   7

#define STORY_DELAY   10.0f   // title held before the crawl begins
#define STORY_SPEED   30.0f   // px per second
#define STORY_LINE_H  27
#define STORY_START_Y 500     // first line begins just off the bottom
#define STORY_TOP     28      // the crawl owns the whole screen
#define STORY_BOT     468
#define STORY_FADE    52      // px of fade at each end of the window
#define STORY_HOLD    6.0f    // title held again after the crawl lands

// Travel needed to carry the last line from its start position up to the title.
#define STORY_RUN   ((float)(STORY_START_Y + (STORY_LINES - 1) * STORY_LINE_H - TITLE_Y))
#define STORY_DUR   (STORY_RUN / STORY_SPEED)
#define STORY_CYCLE (STORY_DELAY + STORY_DUR + STORY_HOLD)

// How present the title is, 0..1. It steps out of the way for the crawl and
// comes back exactly as the crawl's own PHANTOM arrives in its place -- the two
// are the same word at the same size in the same spot, so the handoff reads as
// the scrolling line settling into the title rather than as a cut.
static float title_alpha(void) {
    const float ct = fmodf(vg.state_t, STORY_CYCLE);
    if (ct < STORY_DELAY)              return 1.0f;

    const float s = ct - STORY_DELAY;
    if (s > STORY_DUR)                 return 1.0f;

    // The landing is the whole point of the sequence, so it gets a long, slow
    // resolve -- the word is legible in both forms throughout the overlap and
    // simply firms up into the title rather than swapping over.
    float out = s / 2.6f;                          // dissolve away as it starts
    if (out > 1.0f) out = 1.0f;
    float in = (s - (STORY_DUR - 6.0f)) / 6.0f;    // reassemble as it lands
    if (in < 0.0f) in = 0.0f;
    if (in > 1.0f) in = 1.0f;

    float a = (1.0f - out) + in;
    return (a > 1.0f) ? 1.0f : a;
}

static void draw_story(void) {
    const float ct = fmodf(vg.state_t, STORY_CYCLE);
    if (ct < STORY_DELAY || ct > STORY_DELAY + STORY_DUR) return;

    const float scroll = (ct - STORY_DELAY) * STORY_SPEED;
    const float ta     = title_alpha();

    for (int i = 0; i < STORY_LINES; i++) {
        if (!STORY[i][0]) continue;

        const int y = STORY_START_Y + i * STORY_LINE_H - (int)scroll;
        if (y < STORY_TOP || y > STORY_BOT) continue;

        // Fade in and out at the window edges. Without it lines appear and
        // vanish mid-air, which reads as a glitch rather than as motion.
        float f = 1.0f;
        if (y < STORY_TOP + STORY_FADE)
            f = (float)(y - STORY_TOP) / (float)STORY_FADE;
        else if (y > STORY_BOT - STORY_FADE)
            f = (float)(STORY_BOT - y) / (float)STORY_FADE;
        if (f < 0.0f) f = 0.0f;

        // The closing line is set at title size and hands over to the real
        // title as it arrives, so it is faded out by exactly the amount the
        // title has faded in. Its top-edge fade is suppressed for the same
        // reason -- it must not dim on approach, it must become the title.
        if (i == STORY_LINES - 1) {
            float la = (1.0f - ta);
            if (la > f) la = f;
            if (y <= TITLE_Y + STORY_FADE) la = 1.0f - ta;
            if (la > 0.01f)
                centred(y, STORY[i], vg_dim(INK_BRIGHT, la), TITLE_SCALE);
            continue;
        }

        centred(y, STORY[i], vg_dim(INK_BRIGHT, f), 2);
    }
}

void vg_draw_overlays(void) {
    char buf[40];

    draw_damage_vignette();
    if (vg.state == VG_PLAYING || vg.state == VG_HIT) draw_missile_banner();

    switch (vg.state) {
    case VG_ATTRACT: {
        // Title and prompt fade out together and leave the whole screen to the
        // crawl, then fade back in as the crawl's own PHANTOM arrives.
        const float ta = title_alpha();
        draw_glitch_title("PHANTOM", TITLE_Y, TITLE_SCALE, ta);
        if (ta > 0.01f && fmodf(vg.state_t, 1.2f) < 0.8f)
            centred(250, "TOUCH TO START", vg_dim(INK_MAX, ta), 3);
        draw_story();
        break;
    }

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
        draw_glitch_title("CHAMPION", 150, 6, 1.0f);
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
