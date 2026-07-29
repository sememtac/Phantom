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

// How a patch fails.
//
// Five console failures rather than five flat colours. This interface states
// everything through brightness, inverse video and the ink ramp -- a solid red
// block says "error" in a language the display does not otherwise speak, and
// reads as a graphic pasted over the panel rather than as the panel breaking.
// Every mode below is something the console already does, pushed past working.
static void patch_draw(int x, int y, int w, int h, uint32_t g) {
    switch ((g >> 27) % 5u) {

    case 0:
        // Inverse video with its content knocked out. The panel already inverts
        // to mark a live control; this is that, stuck and holding nothing.
        vg_fill_rect(x, y, w, h, INK);
        for (int i = 2; i < h - 1; i += 4)
            vg_fill_rect(x + 1, y + i, w - 2, 2, INK_ONFILL);
        break;

    case 1:
        // Lost sync: alternate rows lit, the rest gone. The same interleave the
        // scanline pass uses, run to the point where half the region is missing.
        for (int i = 0; i < h; i += 2)
            vg_fill_rect(x, y + i, w, 1, INK_TRACE);
        break;

    case 2:
        // Dropped to the screen ground. Dark, but NOT the black of nothing
        // being drawn -- a dead region of a lit panel still glows faintly, and
        // that difference is most of what makes it read as hardware.
        vg_fill_rect(x, y, w, h, INK_WELL);
        break;

    case 3:
        // Corrupted content: short bars at varying widths and offsets, which is
        // the shape text makes when its addressing survives but its data does
        // not. The most legible failure of the five, because the eye recognises
        // it as writing that has stopped meaning anything.
        for (int i = 0; i < h - 1; i += 3) {
            const uint32_t r  = vg_glitch_hash(g + (uint32_t)i * 2654435761u);
            int            rw = 4 + (int)(r % 24u);
            if (rw > w) rw = w;
            const int      rx = x + (int)((r >> 9) % (uint32_t)(w - rw + 1));
            vg_fill_rect(rx, y + i, rw, 2, ((r >> 20) & 1u) ? INK_BRIGHT : INK_FAINT);
        }
        break;

    default:
        // Stuck full on -- the one that hurts to look at, and the reason the
        // others are needed: on its own it is just a bright rectangle.
        vg_fill_rect(x, y, w, h, INK_MAX);
        break;
    }
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

        patch_draw(bx + dx, by + dy, bw, bh, g);
    }
}
