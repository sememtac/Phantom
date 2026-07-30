#include "vg_draw.h"
#include "vg_game.h"
#include "vg_ift.h"
#include "vg_tourney.h"
#include "vg_voice.h"
#include "vg_sky.h"
#include "vg_glitch.h"
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

#define glitch_hash vg_glitch_hash

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

// The crawl never changes. It is the legend as the hangar bays tell it, and the
// legend is always about someone else -- that is what a rumour is. "THEY CALL
// YOU" is earned exactly once, on the victory screen, and showing it here too
// would spend the line before the player has done anything to deserve it.
static const char* story_line(int i) { return STORY[i]; }

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
#define TITLE_IN      1.6f    // handoff, AFTER the word has landed

// Travel needed to carry the last line from its start position up to the title.
#define STORY_RUN   ((float)(STORY_START_Y + (STORY_LINES - 1) * STORY_LINE_H - TITLE_Y))
#define STORY_DUR   (STORY_RUN / STORY_SPEED)
#define STORY_CYCLE (STORY_DELAY + STORY_DUR + TITLE_IN + STORY_HOLD)

// Progress of the handoff: 0 for the whole crawl, ramping to 1 over TITLE_IN
// once the word has come to rest on the title's position. Everything about the
// swap hangs off this one value, so the word cannot start dissolving before it
// has finished arriving.
static float handoff(float s) {
    float h = (s - STORY_DUR) / TITLE_IN;
    if (h < 0.0f) h = 0.0f;
    if (h > 1.0f) h = 1.0f;
    return h;
}

// How present the title is, 0..1. It steps out of the way for the crawl and
// comes back exactly as the crawl's own PHANTOM arrives in its place -- the two
// are the same word at the same size in the same spot, so the handoff reads as
// the scrolling line settling into the title rather than as a cut.
static float title_alpha(void) {
    const float ct = fmodf(vg.state_t, STORY_CYCLE);
    if (ct < STORY_DELAY)              return 1.0f;

    const float s = ct - STORY_DELAY;
    if (s > STORY_DUR + TITLE_IN)      return 1.0f;

    float out = s / 1.8f;                          // dissolve away as it starts
    if (out > 1.0f) out = 1.0f;

    // Reassembles only once the crawl has come to rest. Overlapping this with
    // the approach meant the word was brightening and dissolving at the same
    // time and never actually reached full -- the arrival has to complete
    // before anything competes with it.
    const float a = (1.0f - out) + handoff(s);
    return (a > 1.0f) ? 1.0f : a;
}

static void draw_story(void) {
    const float ct = fmodf(vg.state_t, STORY_CYCLE);
    if (ct < STORY_DELAY || ct > STORY_DELAY + STORY_DUR + TITLE_IN) return;

    const float s = ct - STORY_DELAY;
    const float h = handoff(s);

    // Travel is pinned once the word lands, so it holds station on the title
    // while the handoff plays rather than sliding off the top of the screen.
    const float scroll = ((s < STORY_DUR) ? s : STORY_DUR) * STORY_SPEED;

    for (int i = 0; i < STORY_LINES; i++) {
        if (!story_line(i)[0]) continue;

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

        // The closing line is set at title size and hands over to the real title
        // as it arrives, so it is faded out by exactly the amount the title has
        // faded in. The window's own edge fade is bypassed entirely -- it must
        // not dim on approach, it has to become the title.
        //
        // On the way up it eases in from nothing instead of riding the screen
        // fully lit: the word materialises out of empty space over about seven
        // seconds, reaching full strength a hundred pixels short of its landing.
        // That leaves the last stretch clear for the crossfade, so the two
        // effects read as one continuous arrival rather than fighting.
        if (i == STORY_LINES - 1) {
            const float FADE_FROM = 460.0f;             // invisible here
            const float FADE_TO   = (float)TITLE_Y;     // full exactly on the title

            float r = (FADE_FROM - (float)y) / (FADE_FROM - FADE_TO);
            if (r < 0.0f) r = 0.0f;
            if (r > 1.0f) r = 1.0f;
            // Smoothstep, so it neither snaps on at the start nor arrives on a
            // hard edge -- a linear ramp is visible as a ramp.
            r = r * r * (3.0f - 2.0f * r);

            // Reaches full at the instant it comes to rest on the title's
            // position, then hands over. The two are never both climbing.
            const float la = r * (1.0f - h);
            if (la > 0.01f)
                centred(y, story_line(i), vg_dim(INK_BRIGHT, la), TITLE_SCALE);
            continue;
        }

        // The rest of the text clears out during the handoff, so the reveal is
        // not left sharing the screen with the tail of the paragraph.
        centred(y, story_line(i), vg_dim(INK_BRIGHT, f * (1.0f - h)), 2);
    }
}

// The feed failing, not just the ship. Tear bars, dropped frames and text that
// loses its place -- the display is the last thing still transmitting and it is
// not doing it well.
//
// All of it is hashed off a time bucket rather than random, so it runs at its
// own rate instead of strobing at whatever the frame rate happens to be, and it
// costs nothing to keep still between buckets.
#define DEATH_FLASH  0.11f

static int death_jitter(void) {
    const uint32_t h = vg_glitch_hash((uint32_t)(vg.state_t * 17.0f) * 977u);
    return ((h % 9u) < 3u) ? (int)((h >> 8) % 15u) - 7 : 0;
}

// Death is the same failure the panel shows when you take a hit, run all the
// way out. Severity 1.0 and every mechanism at once.
static void draw_death_glitch(void) {
    if (vg_glitch_dropout(vg.state_t, 1.0f)) {
        vg_fill_rect(0, 0, SCR_W, SCR_H, COL_BLACK);
        return;
    }
    vg_glitch_tears(vg.state_t, 1.0f);
    vg_glitch_patches(vg.state_t, 1.0f);
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

    // Launch cutscene. Each fighter is named while it is on screen, and the
    // gaps between passes are hard cuts -- a black frame with a tear across it,
    // which is what sells a change of shot on a display with no camera.
    case VG_INTRO: {
        const float t = vg.state_t;
        const bool  cut = (t > INTRO_YOU_END && t < INTRO_OPP_START) ||
                          (t > INTRO_OPP_END && t < INTRO_END);
        if (cut) {
            vg_fill_rect(0, 0, SCR_W, SCR_H, COL_BLACK);
            const int ty = 120 + (int)(fmodf(t * 900.0f, 240.0f));
            vg_fill_rect(0, ty, SCR_W, 3, INK_TRACE);
            break;
        }
        // Where this is happening, held over the opening drift and handed off
        // to the first fighter. A venue card, the way a broadcast would open.
        if (t < INTRO_DRIFT + 0.9f) {
            float a = t / 0.7f;
            if (a > 1.0f) a = 1.0f;
            if (t > INTRO_DRIFT) {
                const float out = 1.0f - (t - INTRO_DRIFT) / 0.9f;
                if (out < a) a = out;
            }
            if (a > 0.01f) {
                centred(196, vg_sky_place(), vg_dim(INK_MAX, a), 4);
                centred(244, vg_tourney_round_name(vt.round), vg_dim(INK_BRIGHT, a), 2);
            }
        }

        // The card waits for the broadcast to have SPOKEN AND FINISHED, tested on
        // the slot's own fired bit rather than on a time.
        //
        // Testing the clock is what put the card first: the card started at
        // INTRO_DRIFT and the line fired 0.4s later, so for those 0.4s the card
        // was alone on screen and then vanished when the IFT arrived. Matching the
        // two thresholds would have worked only as long as nothing reordered the
        // update and the draw. This cannot be got wrong by a timing change.
        //
        // An unwritten slot sets its bit and posts nothing, so the card appears at
        // once -- which is the right behaviour for a line that does not exist yet.
        const bool said_you = (vg.ift_fired & (1u << IFT_INTRO_YOU)) && vg.ift_t <= 0.0f;
        const bool said_opp = (vg.ift_fired & (1u << IFT_INTRO_OPP)) && vg.ift_t <= 0.0f;

        if (said_you && t > INTRO_DRIFT && t < INTRO_YOU_END) {
            centred(360, vg.callsign, INK_MAX, 5);
            centred(414, vg.spec->name, INK_BRIGHT, 2);
        } else if (said_opp && t > INTRO_OPP_START && t < INTRO_OPP_END) {
            const Entrant* o = vg_tourney_opponent();
            if (o) {
                centred(360, o->is_phantom ? "PHANTOM" : o->tag, INK_MAX, 5);
                snprintf(buf, sizeof(buf), "%s   %s",
                         vg_spec(o->cls)->name, vg_voice_archetype(o->voice));
                centred(414, buf, INK_BRIGHT, 2);
            }
        }
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

    // Winning closes the loop the intro opened. The crawl says a rumour goes
    // round the hangar bays about an elite pilot nobody has seen -- take the
    // tournament and the screen tells you, in the crawl's own words and the
    // title's own lettering, that the rumour is now about you.
    // Four beats, each finishing before the next begins. Overlapping them was
    // the mistake the intro crawl made first: two things fading at once means
    // neither is ever fully present, and the moment has nothing to land on.
    case VG_WON: {
        const float t = vg.state_t;

        // 1. The result, which then gets out of the way entirely.
        if (t < WON_RUMOR_IN) {
            float a = (t > 3.0f) ? (1.0f - (t - 3.0f)) : 1.0f;
            if (a < 0.0f) a = 0.0f;
            draw_glitch_title("CHAMPION", 128, 6, a);
            snprintf(buf, sizeof(buf), "%s   %s", vg.callsign, vg.spec->name);
            centred(212, buf, vg_dim(INK_BRIGHT, a), 3);
            snprintf(buf, sizeof(buf), "BANK %d CR", vg.credits);
            centred(254, buf, vg_dim(INK_FAINT, a), 2);
        }

        // 2. The rumour arrives, and is left alone long enough to be read.
        // 3. Then it withdraws as the name takes its place.
        if (t > WON_RUMOR_IN) {
            float a = (t - WON_RUMOR_IN) / 1.4f;
            if (a > 1.0f) a = 1.0f;
            if (t > WON_NAME_IN) {
                const float out = 1.0f - (t - WON_NAME_IN) / 1.5f;
                if (out < a) a = out;
            }
            if (a > 0.01f) {
                centred(214, "THE HANGAR BAYS HAVE A NEW RUMOR", vg_dim(INK_FAINT, a), 1);
                centred(238, "THEY CALL YOU...", vg_dim(INK_BRIGHT, a), 2);
            }
        }

        // 4. And it is the word that has been on the title card all along.
        // Same size, same position, same glitch treatment -- so the payoff is
        // recognition rather than information.
        if (t > WON_NAME_IN) {
            float a = (t - WON_NAME_IN) / 2.4f;
            if (a > 1.0f) a = 1.0f;
            a = a * a * (3.0f - 2.0f * a);
            draw_glitch_title("PHANTOM", TITLE_Y, TITLE_SCALE, a);
        }
        break;
    }

    case VG_HIT:
        if (fmodf(vg.state_t, 0.5f) < 0.3f)
            centred(200, "DAMAGE", COL_DANGER, 5);
        snprintf(buf, sizeof(buf), "HULL %d",
                 (int)(vg.health / vg.health_max * 100.0f + 0.5f));
        centred(258, buf, INK_MAX, 3);
        break;

    // No instruments -- there is no cockpit left to report from. What is on
    // screen is the wreck, the tumble, and the system telling you what happened.
    case VG_OVER: {
        draw_death_glitch();

        // The text drifts with the tearing rather than sitting steady over it.
        const int j = death_jitter();
        vg_text((SCR_W - vg_text_width("SIGNAL LOST", 5)) / 2 + j, 150,
                "SIGNAL LOST", COL_DANGER, 5);
        snprintf(buf, sizeof(buf), "%s   HULL BREACH", vg.callsign);
        vg_text((SCR_W - vg_text_width(buf, 2)) / 2 - j, 214, buf, COL_HUD, 2);
        snprintf(buf, sizeof(buf), "ELIMINATED IN THE %s",
                 vg_tourney_round_name(vt.round));
        centred(246, buf, COL_HUD_DIM, 2);
        snprintf(buf, sizeof(buf), "BANK %d CR", vg.credits);
        centred(278, buf, INK_FAINT, 2);
        if (vg.state_t > 2.2f && fmodf(vg.state_t, 1.0f) < 0.6f)
            centred(330, "TAP TO RETURN", COL_STAR_BRIGHT, 2);

        // The moment itself: one hard white frame, gone almost before it
        // registers. Drawn LAST so it covers everything, including the glitch --
        // the display is overwhelmed first and fails afterwards.
        if (vg.state_t < DEATH_FLASH) {
            float f = 1.0f - vg.state_t / DEATH_FLASH;
            vg_fill_rect(0, 0, SCR_W, SCR_H, vg_dim(COL_STAR_BRIGHT, f * f));
        }
        break;
    }

    default:
        break;
    }
}
