#include "vg_glitch.h"
#include "vg_raster.h"
#include "vg_config.h"

uint32_t vg_glitch_hash(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352dU;
    x ^= x >> 15; x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

void vg_glitch_offset(float t, float rate, float amp, float* dx, float* dy) {
    const uint32_t h = vg_glitch_hash((uint32_t)(t * rate) * 0x9E3779B9u);
    *dx = ((float)(h        & 63u) / 63.0f - 0.5f) * 2.0f * amp;
    *dy = ((float)((h >> 7) & 63u) / 63.0f - 0.5f) * 2.0f * amp;
}

bool vg_glitch_dropout(float t, float sev) {
    if (sev <= 0.0f) return false;
    const uint32_t h = vg_glitch_hash((uint32_t)(t * 17.0f));
    // Rarer as severity falls: a wounded ship stutters occasionally, a dead one
    // stutters constantly.
    const uint32_t odds = 23u + (uint32_t)((1.0f - sev) * 90.0f);
    return (h % odds) == 0u;
}

void vg_glitch_tears(float t, float sev) {
    if (sev <= 0.0f) return;

    const uint32_t bucket = (uint32_t)(t * 17.0f);
    const uint32_t h      = vg_glitch_hash(bucket);

    const int bars = 1 + (int)((h >> 3) % (uint32_t)(1 + (int)(sev * 3.0f)));
    for (int i = 0; i < bars; i++) {
        const uint32_t g  = vg_glitch_hash(bucket * 71u + (uint32_t)i);
        const int      y  = (int)(g % (uint32_t)SCR_H);
        const int      hh = 2 + (int)((g >> 8) % (uint32_t)(2 + (int)(sev * 9.0f)));
        const int      xo = (int)(((g >> 13) % 70u) - 35u) * (int)(sev * 1.0f + 0.35f);
        vg_fill_rect(xo, y, SCR_W, hh, ((g >> 21) & 1u) ? INK_TRACE : INK_ONFILL);
    }

    if (sev > 0.6f && ((h >> 11) % 5u) == 0u)
        vg_fill_rect(0, (int)((h >> 15) % (uint32_t)SCR_H), SCR_W, 2, INK_BRIGHT);
}

void vg_glitch_patches(float t, float sev) {
    if (sev <= 0.0f) return;

    const uint32_t slow = (uint32_t)(t * 4.5f);
    const uint32_t fast = (uint32_t)(t * 22.0f);

    const int nblk = 1 + (int)(vg_glitch_hash(slow * 31u) % (uint32_t)(1 + (int)(sev * 6.0f)));
    for (int i = 0; i < nblk; i++) {
        const uint32_t g = vg_glitch_hash(slow * 613u + (uint32_t)i * 71u);

        // Not lit every moment. A stuck block that never interrupts looks
        // painted on rather than broken.
        if (((vg_glitch_hash(fast * 17u + (uint32_t)i) >> 3) % 5u) == 0u) continue;

        const int bw = 10 + (int)((g >> 2) % (uint32_t)(14 + (int)(sev * 64.0f)));
        const int bh = 4  + (int)((g >> 9) % (uint32_t)(6  + (int)(sev * 32.0f)));
        if (bw >= SCR_W || bh >= SCR_H) continue;

        const int bx = (int)((g >> 15) % (uint32_t)(SCR_W - bw));
        const int by = (int)((g >> 21) % (uint32_t)(SCR_H - bh));

        // Edges crawl a few pixels on the fast clock, so a patch never holds a
        // clean rectangle.
        const uint32_t m  = vg_glitch_hash(fast * 53u + (uint32_t)i);
        const int      dx = (int)(m % 7u) - 3;
        const int      dy = (int)((m >> 4) % 5u) - 2;

        uint16_t c;
        switch ((g >> 27) % 4u) {
        case 0:  c = COL_BLACK;  break;   // dead
        case 1:  c = INK_MAX;    break;   // stuck full on
        case 2:  c = INK_TRACE;  break;
        default: c = COL_DANGER; break;   // one channel jammed
        }
        vg_fill_rect(bx + dx, by + dy, bw, bh, c);

        // Some fail by ROW rather than as a solid area, which is what losing
        // part of the addressing actually looks like.
        if (((g >> 25) & 1u) && bh > 12) {
            for (int y = 1; y < bh; y += 3)
                vg_fill_rect(bx + dx, by + dy + y, bw, 1, COL_BLACK);
        }
    }
}
