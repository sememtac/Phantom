#pragma once
#include <stdlib.h>
#include <stdint.h>
// One heap, no capabilities. The flags are accepted and ignored: a PC has no
// DMA-reachable or PSRAM distinction, and every allocation the game makes is
// small enough that malloc is a fair stand-in.
//
// The free-size answers are invented, and generously. They only ever reach
// diagnostics -- vg_rast_init prints them at boot -- so a plausible number keeps
// that line readable rather than alarming.
#define MALLOC_CAP_DMA       (1 << 0)
#define MALLOC_CAP_INTERNAL  (1 << 1)
#define MALLOC_CAP_SPIRAM    (1 << 2)
#define MALLOC_CAP_8BIT      (1 << 3)
#define MALLOC_CAP_32BIT     (1 << 4)
static inline void* heap_caps_malloc(size_t n, uint32_t)          { return malloc(n); }
static inline void* heap_caps_calloc(size_t c, size_t n, uint32_t){ return calloc(c, n); }
static inline void  heap_caps_free(void* p)                       { free(p); }
// PLAIN malloc, NOT _aligned_malloc, and the reason matters. The one caller
// pairs this with heap_caps_free, and a pointer from _aligned_malloc must go
// back through _aligned_free or the heap is corrupted. malloc on x64 already
// returns 16-byte aligned memory, which is the only alignment ever asked for
// here, so this satisfies the request AND stays free()-able.
static inline void* heap_caps_aligned_alloc(size_t a, size_t n, uint32_t) {
    (void)a;
    return malloc(n);
}
static inline size_t heap_caps_get_free_size(uint32_t)            { return 8u * 1024u * 1024u; }
static inline size_t heap_caps_get_largest_free_block(uint32_t)   { return 8u * 1024u * 1024u; }
