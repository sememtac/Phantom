#include "vg_bezel.h"
#include "vg_raster.h"
#include "vg_raster_int.h"
#include "vg_port.h"
#include "vg_config.h"
#include <Arduino.h>   // IRAM_ATTR

// The selected bezel, or none. Menus that have no chassis leave this null and
// the primitive returns immediately.
static const VgBezel* s_cur = nullptr;

void vg_bezel_use(const VgBezel* b) { s_cur = b; }

// The slots are emitted sorted by role, so the drawing ones are the leading run
// and the Nth of them is simply the Nth entry. The role test is what keeps that
// true if a drawing ever has no drawing slots at all.
const VgBezelSlot* vg_bezel_slot(int n) {
    if (!s_cur || n < 0 || n >= (int)s_cur->slots) return nullptr;
    return (s_cur->slot[n].role == VG_SLOT_DRAW) ? &s_cur->slot[n] : nullptr;
}

const VgBezelSlot* vg_bezel_headline(void) {
    if (!s_cur) return nullptr;
    for (int i = 0; i < (int)s_cur->slots; i++)
        if (s_cur->slot[i].role == VG_SLOT_HEADLINE) return &s_cur->slot[i];
    return nullptr;
}
const VgBezel* vg_bezel_current(void) { return s_cur; }

// vg_bezel_prim lives in vg_raster.cpp, beside vg_canopy_prim: the primitive
// list's push() is private to that file.

// PAINT, DO NOT BLEND.
//
// One byte in, one palette lookup, one 16-bit store. The palette is 512 bytes and
// stays in cache across the whole frame, so the inner loop is a load, an indexed
// load and a store -- which is why a chassis covering 42% of the panel costs less
// than a cockpit frame covering 10% of it. The canopy has to read the destination
// back, add a signed delta to three channels and clamp each one.
//
// r0 and r1 are the CALLER'S rows and not the band's. Under the row split each
// core owns part of a band, and a store outside its share lands in the other
// core's rows.
void IRAM_ATTR vg_bezel_rows(uint16_t* band, int by0, int r0, int r1) {
    const VgBezel* b = s_cur;
    if (!b) return;

    // Hoisted out of the loops. The palette is 512 bytes and stays in cache all
    // frame, but reloading the base pointer through two levels of struct on every
    // pixel is work the compiler cannot always see through.
    const uint16_t* pal  = b->pal;
    const uint8_t*  data = b->data;

    for (int r = r0; r < r1; r++) {
        const int y = by0 + r;
        if (y < 0 || y >= SCR_H) continue;

        const uint16_t s0 = b->row[y], s1 = b->row[y + 1];
        uint16_t* dst_row = &band[r * SCR_W];

        for (uint16_t si = s0; si < s1; si++) {
            const VgBezelSpan* sp  = &b->span[si];
            const uint8_t*     src = &data[sp->off];
            uint16_t*          dst = &dst_row[sp->x0];
            int                n   = sp->len;

            // TWO PIXELS TO A STORE. Measured on the board at 3.58 ms for 94,235
            // pixels, which is the biggest single item in a menu frame -- nine
            // cycles a pixel for a byte load, an indexed load and a 16-bit store.
            //
            // The stores are the half worth attacking. The panel is 16-bit and the
            // bus is 32, so a pair of pixels is one word, and the scanline pass
            // next door already found that most of its cost was loop overhead
            // rather than arithmetic.
            //
            // A span starts at an arbitrary x, so the first pixel may be at an odd
            // halfword. It goes out on its own to reach alignment, and an
            // unaligned 32-bit store on Xtensa is a fault rather than a slowdown,
            // so this is correctness and not tuning.
            if ((((uintptr_t)dst) & 2u) && n > 0) {
                *dst++ = pal[*src++];
                n--;
            }

            uint32_t* d32 = (uint32_t*)dst;

            // Four at a time. The pairs are built as words: the low halfword is
            // the lower address, which is what a little-endian store wants, and
            // the palette is already in the panel's byte order.
            while (n >= 4) {
                const uint32_t p0 = (uint32_t)pal[src[0]] | ((uint32_t)pal[src[1]] << 16);
                const uint32_t p1 = (uint32_t)pal[src[2]] | ((uint32_t)pal[src[3]] << 16);
                d32[0] = p0;
                d32[1] = p1;
                d32 += 2;
                src  += 4;
                n    -= 4;
            }
            while (n >= 2) {
                *d32++ = (uint32_t)pal[src[0]] | ((uint32_t)pal[src[1]] << 16);
                src += 2;
                n   -= 2;
            }

            dst = (uint16_t*)d32;
            while (n-- > 0) *dst++ = pal[*src++];
        }
    }
}

int vg_bezel_gaps(int y, int16_t* out) {
    const VgBezel* b = s_cur;
    if (!b || y < 0 || y >= SCR_H) {
        out[0] = 0; out[1] = SCR_W - 1;
        return 1;
    }

    int n = 0, x = 0;
    for (uint16_t si = b->row[y]; si < b->row[y + 1]; si++) {
        const VgBezelSpan* sp = &b->span[si];
        if (sp->x0 > x && n < VG_BEZEL_MAX_GAPS) {
            out[n * 2]     = (int16_t)x;
            out[n * 2 + 1] = (int16_t)(sp->x0 - 1);
            n++;
        }
        const int end = sp->x0 + sp->len;
        if (end > x) x = end;
    }
    if (x < SCR_W && n < VG_BEZEL_MAX_GAPS) {
        out[n * 2]     = (int16_t)x;
        out[n * 2 + 1] = (int16_t)(SCR_W - 1);
        n++;
    }
    return n;
}
