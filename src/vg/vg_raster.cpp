#include "vg_raster.h"
#include "vg_raster_int.h"
#include "vg_font.h"
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_memory_utils.h>
#include <math.h>
#include "vg_canopy_draw.h"
#include "vg_bezel.h"

// The submit half. Everything here runs ONCE per frame and costs frame time
// directly -- unlike the band raster in vg_band.cpp, which hides under DMA. Work
// added here is the expensive kind.
//
// Order of operations for every primitive: HUD warp (logical space) -> display
// rotation -> screen clip -> append. Doing the rotation here rather than by
// transposing band buffers keeps it to a couple of ops per primitive instead of
// a per-pixel copy.

// ---------------------------------------------------------------------------
// Primitive list
// ---------------------------------------------------------------------------

// Triangles are counted apart from everything else because their cost has
// nothing to do with how many there are -- one face of a close asteroid can
// cover more pixels than the entire rest of the frame.

static Prim* s_prims    = nullptr;
static int   s_count    = 0;          // the JOINED total, set by vg_prim_join

// ===========================================================================
// TWO SUBMITTERS, ONE ARRAY
//
// Submit is 3.8 ms of the 4.37 ms that bills frame time directly, and it splits
// cleanly in half where the draw order already breaks: the world (starfield, grid,
// objects, course gate) against the instruments (HUD, overlays, markers, mirror).
// Measured at 2.0 ms and 1.8 ms. Nothing in either half writes game state -- all
// six draw modules are read-only on `vg`, and vg_cockpit's writes live in
// vg_update_alerts and vg_hud_decay, which are update functions -- so the two can
// be built AT THE SAME TIME on the two cores.
//
// ONE ARRAY, NOT TWO, and that is what makes it free. Each submitter fills its own
// slice; vg_prim_join then memmoves the second block down to sit immediately after
// the first. What the band raster then sees is byte-for-byte the array a serial
// submit would have produced, in the same order, so the whole scanline active-list
// machinery below is untouched and no second 40KB list is needed. The peak count is
// 920 against MAX_PRIMS 2000, so the halves have room to spare.
//
// WHAT HAD TO BECOME PER-SUBMITTER, because all four are set by one half and read
// by the other's primitives if left global:
//   the AA flag      vg_render turns it OFF for the world and ON for the
//                    instruments -- precisely the two halves
//   fills            the mirror turns hidden-line fills off for its own content
//   the clip window  the mirror runs inside a viewport; the main view does not
//   the cursor       obviously
//
// The current context is selected PER CORE, so no draw module has to know any of
// this exists -- an emitter just asks which submitter is calling.
// ===========================================================================
struct Sub {
    int     at, end;                 // this submitter's slice of s_prims
    uint8_t aa;                      // vg_line_aa_mode
    bool    fills;                   // vg_rast_fills
    int     cx0, cy0, cx1, cy1;      // clip window, PANEL space, inclusive
    // The HUD warp and the panel shake. In the context for the same reason as the
    // rest: vg_line, vg_fill_rect and vg_text all consult it, group B opens the
    // bracket, and a shared flag would have core 0 bending core 1's WORLD geometry.
    bool    warp;
    float   warp_k;
    float   jx, jy;
    bool    overflow;
    int     tri;
};

// FOUR SLICES, AND THE REASON IS DRAW ORDER, not memory.
//
// Two slices could not be balanced. A core's slice decides WHERE its primitives land
// in the final list, so with the instruments in the second slice everything core 0
// draws lands last -- and the only work that may legitimately draw last is the work
// already there. Submit measured 2390 us on core 1 against 1180 on core 0, and the
// 1200 us difference could not move, because the arena grid has to draw BEHIND the
// ships and moving it to the other core would have drawn it in front of them.
//
// So the slices interleave the two cores instead:
//
//   0  starfield, hoops        core 1
//   1  rails                   core 0
//   2  world objects, gate     core 1
//   3  instruments             core 0
//
// Joined in index order, which is draw order, so the grid still lies under the hulls
// while half of it was built on the other core.
//
// Sized individually, see SUB_AT: an even split would have handed the busiest block a
// quarter of what it had. It IS four ceilings now instead of two, and an overflow shows
// on the telemetry line.
#define NSUB 4

// NOT EQUAL QUARTERS. Each slice is its own ceiling now, so an even split would have
// given the busiest block a quarter of what it had before -- and MAX_PRIMS was sized
// for the shard burst, which lands in the world.
//
// Sized from the geometry rather than guessed. nhoop is 10, so the grid is at most
// 21 hoops of ARENA_HOOP_SEGS (294 segments) and ARENA_RAILS of nhoop*2 (160), which
// the measured 846:426 us ratio agrees with. The instruments carry the rear-view
// patch, and that re-submits the whole grid at half density -- another ~234 -- on top
// of the HUD's own.
//
// Slice 0 ends where slice 1 begins, and so on; the last bound is MAX_PRIMS.
//
// SINCE THE RAILS WENT BACK to core 1 -- see the note at the submit call in
// vg_render.cpp -- the whole grid lands in slice 0 and slice 1 rides empty. The
// bounds are kept as they are for the per-frame rails ownership that note names.
// Worst-case hoops plus rails is 454 segments against slice 0's 450 plus the
// stars; a slice that overflows says so on the telemetry line.
static constexpr int SUB_AT[NSUB + 1] = {
    0,        // 0: starfield + hoops   -- 450: at most 294 grid segments, plus the stars
    450,      // 1: rails               -- 200: at most 160 segments
    650,      // 2: world objects, gate -- 800: the shard burst's 160 lives here
    1450,     // 3: instruments         -- 550: HUD, overlays, and the mirror's ~234
    2000,     // == MAX_PRIMS
};
static_assert(SUB_AT[NSUB] == MAX_PRIMS, "slice bounds must cover the list exactly");

static Sub  s_sub[NSUB];
static Sub* s_cur[2] = { &s_sub[0], &s_sub[0] };

static inline Sub* sub(void) { return s_cur[xPortGetCoreID()]; }

// Which half the calling core is building. Called by vg_render_frame around the
// two groups; the mapping is per core, so both cores can be inside submit at once.
void vg_prim_select(int group) {
    if (group < 0) group = 0;
    if (group >= NSUB) group = NSUB - 1;
    s_cur[xPortGetCoreID()] = &s_sub[group];
}

// THE LIVE TOTAL, straight off the cursors, and it has to be live rather than a
// value written by vg_prim_join.
//
// draw_fps prints the primitive count INTO THE FRAME -- deliberately, so a dip in
// the rate can be told apart from a rise in geometry. So the count is not only a
// diagnostic, it is PIXELS, and it is read during submit while the number is still
// growing. Parking it in s_count until join meant draw_fps read a stale value and
// the overlay printed the wrong number: 255 pixels in a 46x14 box, which the
// replay harness caught and nothing else would have.
//
// Equal to s_count once join has run, since join does not move the cursors.
static inline int live_count(void) {
    int n = 0;
    for (int i = 0; i < NSUB; i++) n += s_sub[i].at - SUB_AT[i];
    return n;
}

// Close the frame: bring the second block down against the first so the list is
// contiguous and in draw order.
// Timed apart from its caller's bracket because the two disagree: the bracket in
// vg_rast_flush reads 361-400 us in combat, which is far too much for a copy of a
// few hundred 20-byte structs in internal SRAM, and it does not track the prim
// count -- attract pays 141 us at P 364 while a light combat frame pays 13 at
// P 293. So the copy gets its own counter and the count comes out with it. If the
// copy is cheap, the time is going somewhere else in the flush prologue and the
// name `join` is a red herring.
static uint32_t s_join_mm_us = 0;
static int      s_join_n     = 0;
uint32_t vg_rast_join_mm_us(void) { return s_join_mm_us; }
int      vg_rast_join_n(void)     { return s_join_n; }

void vg_prim_join(void) {
    int at = s_sub[0].at;          // slice 0 is already in place
    int moved = 0;
    const uint32_t t0 = micros();
    // NOT memmove. This toolchain's memmove is a byte-at-a-time loop: measured at
    // ~1.05 us per 20-byte primitive, dead linear, which is about 19 MB/s and cost
    // 350-500 us of every combat frame to shift 8 KB around inside internal SRAM.
    // That was larger than the raster overrun the row split had just removed.
    //
    // A word copy is safe here for reasons that are worth stating rather than
    // rediscovering: sizeof(Prim) is a multiple of 4 and s_prims is word aligned,
    // so both ends are aligned and the length is a whole number of words; and the
    // destination is always at or below the source, because group A's cursor
    // cannot pass its own slice end. A FORWARD copy is therefore correct even if the two
    // ranges overlapped, since every word is read before anything writes over it.
    // The destination-below-source argument still holds with four blocks, and it is
    // worth restating because it is what makes a forward copy safe. Block i starts at
    // SUB_AT[i], and every block before it contributed at most its own capacity, so the
    // running cursor cannot have passed SUB_AT[i] by the time block i is
    // copied. Destination <= source for every block, so every word is read before
    // anything writes over it.
    static_assert(sizeof(Prim) % 4 == 0, "prim copy assumes whole words");
    for (int i = 1; i < NSUB; i++) {
        const int n = s_sub[i].at - SUB_AT[i];
        if (n <= 0) continue;
        uint32_t*       d = (uint32_t*)(void*)&s_prims[at];
        const uint32_t* s2 = (const uint32_t*)(const void*)&s_prims[SUB_AT[i]];
        const int       w = n * (int)(sizeof(Prim) / 4);
        for (int k = 0; k < w; k++) d[k] = s2[k];
        at    += n;
        moved += n;
    }
    s_join_n     = moved;
    s_join_mm_us = micros() - t0;
    s_count      = at;
}

bool vg_prim_init(void) {
    // Internal: the list is swept once per band, 15 times a frame.
    s_prims = (Prim*)heap_caps_malloc(sizeof(Prim) * MAX_PRIMS,
                                      MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_prims) {
        Serial.println("vg_prim_init: alloc failed");
        return false;
    }
    // WHERE IT ACTUALLY LANDED. The join's memmove measures ~1.05 us per 20-byte
    // primitive, which is about 19 MB/s -- ten times too slow for internal SRAM
    // and about right for PSRAM. The cap above asks for internal; this says
    // whether it got it. It matters far beyond the copy: this list is swept once
    // per band, fifteen times a frame.
    Serial.printf("vg_prim_init: %d prims, %u KB at %p, internal=%d\n",
                  (int)MAX_PRIMS,
                  (unsigned)(sizeof(Prim) * MAX_PRIMS / 1024),
                  (void*)s_prims,
                  (int)esp_ptr_internal(s_prims));
    return true;
}

const Prim* vg_prim_list(void) { return s_prims; }
int         vg_prim_live(void) { return s_count; }   // the JOINED list

// A MASTER SWITCH over every request to turn antialiasing ON, so it can be taken
// away wholesale and looked at. Off is the interesting direction: the world already
// runs unsmoothed by the author's decision, so clearing this is "no AA anywhere", and
// `aa` bills 1200-1580 us of the raster -- the largest single item that exists purely
// for how something looks rather than for what it does.
//
// Read by both cores while they submit. A plain bool, no synchronisation: the worst a
// torn read can do is smooth one half of one frame, and it moves only when somebody
// sends the command.
// OFF, BY THE AUTHOR'S DECISION AFTER LOOKING AT BOTH: "I really didn't notice any
// difference." Measured cost of the smoothing it removes: 1090-1430 us of every
// combat frame, which is more than the ~930 us that stood between combat and 60 fps,
// and more than the whole submit split bought.
//
// It is the second time this has gone the same way. Antialiasing was taken off the
// WORLD in 87f5ae3 and turned out to be 59 us of the 1200 it was blamed for; the 1200
// was here, on the instruments, and was kept on the reasoning that instruments have
// to be READ. That reasoning was sound and the conclusion was still wrong -- on a
// 480x480 panel at this pixel pitch the difference did not survive contact with
// somebody flying the game.
//
// Kept as a switch rather than deleted, because the AA rasteriser is still the better
// picture in principle and 'q' puts it back for a look. `aa` in the telemetry should
// now read 0 in flight.
static bool s_aa_master = false;
void vg_rast_aa_master(bool on) { s_aa_master = on; }
bool vg_rast_aa_master_on(void) { return s_aa_master; }

void vg_line_aa_mode(bool on) { sub()->aa = (on && s_aa_master) ? LINE_AA : LINE_OPAQUE; }

// Bracketed exactly like the antialiasing above, and for the same reason: the drawing
// functions should not have to know, and whatever turns it on turns it back off. NOT
// gated on the AA master -- adding light is not smoothing, and the author took the
// smoothing off precisely because it was not doing anything visible.
void vg_line_blend(int mode) {
    sub()->aa = (mode == LINE_ADD || mode == LINE_SUB) ? (uint8_t)mode : LINE_OPAQUE;
}

int vg_rast_tri_count(void) {
    int n = 0;
    for (int i = 0; i < NSUB; i++) n += s_sub[i].tri;
    return n;
}

bool vg_rast_init(void) {
    if (!vg_prim_init()) return false;
    if (!vg_band_init()) return false;

    // The buffer count comes from BAND_BUFS rather than being spelled "2x" as it was.
    // A diagnostic that states the wrong layout is worse than one that states none.
    Serial.printf("vg_rast_init: prims %uKB bands %dx%uKB (%d bands) internal-free %uKB\n",
                  (unsigned)(sizeof(Prim) * MAX_PRIMS / 1024),
                  vg_band_bufs(),
                  (unsigned)(SCR_W * BAND_H * 2 / 1024),
                  (int)NUM_BANDS,
                  (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
    // BOTH POOLS, AND THE FLASH, because "is there room" is a question about three
    // budgets that behave nothing alike and only one of them was ever reported.
    //
    // Internal SRAM is the scarce one and the only one the renderer can use: the band
    // buffers and the primitive list have to be here, because rasterising against PSRAM
    // thrashes the cache badly enough to dominate a frame -- which is the finding the
    // whole two-stage design is built on.
    //
    // PSRAM is large and nearly untouched, and it is fine for anything read once per
    // frame or less. Game DATA belongs here if it ever gets big enough to matter.
    //
    // Flash is where content actually lives: dialogue, ship tables, models, canopy
    // drawings are all const and never copied to RAM. It is the budget with the most
    // room and the one a writer or an artist is most likely to spend.
    // What mode the flash cache is ACTUALLY filling in, read from the controller
    // rather than believed from any header: the ROM banner says DIO about its own
    // conservative load of the bootloader, the image header says what esptool was
    // told, and neither is necessarily what the app runs with. Bits per the S3 TRM:
    // SPI_MEM_CTRL_REG(0) FREAD_QIO(24) FREAD_DIO(23) FREAD_QUAD(20) FREAD_DUAL(14).
    {
#if defined(__XTENSA__)
        const uint32_t c = *(volatile uint32_t*)0x60002008;   // SPI_MEM_CTRL_REG(0)
#else
        // No flash cache controller to ask, and no memory at that address --
        // dereferencing it on a desktop is an access violation. Reports SLOW,
        // which is the honest answer for a build that is not fetching from
        // flash at all.
        const uint32_t c = 0;
#endif
        const char* m = (c & (1u << 24)) ? "QIO" : (c & (1u << 23)) ? "DIO"
                      : (c & (1u << 20)) ? "QOUT" : (c & (1u << 14)) ? "DOUT" : "SLOW";
        Serial.printf("vg_rast_init: flash cache mode %s (ctrl %08x)\n", m, (unsigned)c);
    }
    Serial.printf("vg_rast_init: internal-free %uKB  psram-free %uKB  (largest block: "
                  "internal %uKB psram %uKB)\n",
                  (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                  (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
                  (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024),
                  (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024));
    return true;
}

// THE HIGH WATER MARK, since boot and never reset.
//
// MAX_PRIMS is 2000 and sizeof(Prim) is 20, so the list is 40KB of internal SRAM --
// and there are only ~36KB free, which is what blocks the one change that would
// clear both frame budgets at once: building the NEXT frame's list on core 0 while
// core 1 rasterises this one needs two lists.
//
// So the question is not what the list costs, it is what it USES. 3400 was sized
// for the shard burst -- 160 shards at one primitive each, plus everything else
// they are on top of -- and the per-frame count reads 470-652 in combat. If the
// true peak across a whole session is under half the ceiling, two lists fit inside
// the 40KB already spent and the pipeline becomes affordable without another byte.
//
// Peak rather than a mean, and never reset, because the number that decides a
// buffer size is the worst moment the game can produce, not the usual one. Sampled
// at the START of a frame, which is the only point where the previous frame's count
// is complete.
static int s_peak = 0;

// PER SLICE, because the whole-frame peak cannot size a slice.
//
// MAX_PRIMS is 2000 and the frame peak is 759, so the list looks over twice as large as
// it needs to be -- but each slice is its own ceiling now, and a slice that overflows
// drops primitives whatever the total was doing. Sizing them down needs to know which
// one gets closest to its own bound, and nothing measured that.
static int s_peak_slice[NSUB] = { 0 };

void vg_rast_begin_frame(void) {
    // From the cursors, before they are reset, so the peak does not depend on join
    // having run at all.
    if (live_count() > s_peak) s_peak = live_count();
    for (int i = 0; i < NSUB; i++) {
        const int n = s_sub[i].at - SUB_AT[i];
        if (n > s_peak_slice[i]) s_peak_slice[i] = n;
    }
    s_count = 0;
    // Both submitters, to the same defaults the globals used to hold: AA on, fills
    // on, clipping to the whole panel. vg_render overrides what it needs per half.
    for (int i = 0; i < NSUB; i++) {
        Sub* u = &s_sub[i];
        u->at    = SUB_AT[i];
        u->end   = SUB_AT[i + 1];
        u->aa    = 1;
        u->fills = true;
        u->cx0 = 0; u->cy0 = 0; u->cx1 = SCR_W - 1; u->cy1 = SCR_H - 1;
        u->warp   = false;
        u->warp_k = HUD_WARP_K;
        u->jx = 0.0f; u->jy = 0.0f;
        u->overflow = false;
        u->tri      = 0;
    }
    s_cur[0] = &s_sub[0];
    s_cur[1] = &s_sub[0];
}
int  vg_rast_prim_count(void)  { return live_count(); }
int  vg_rast_prim_peak(void)   { return s_peak; }
int  vg_rast_slice_peak(int i) { return (i >= 0 && i < NSUB) ? s_peak_slice[i] : 0; }
int  vg_rast_slice_cap(int i)  { return (i >= 0 && i < NSUB) ? SUB_AT[i + 1] - SUB_AT[i] : 0; }
int  vg_rast_slices(void)      { return NSUB; }
bool vg_rast_overflowed(void)  {
    for (int i = 0; i < NSUB; i++) if (s_sub[i].overflow) return true;
    return false;
}

static inline Prim* push(void) {
    Sub* u = sub();
    if (u->at >= u->end) { u->overflow = true; return nullptr; }
    return &s_prims[u->at++];
}

// ---------------------------------------------------------------------------
// Colour
//
// Values are stored byte-swapped for the panel (see VGC in cfg_palette.h), so
// anything that interprets a colour has to swap in and back out. Affordable
// precisely because these run per object, never per pixel.
// ---------------------------------------------------------------------------

uint16_t vg_dim(uint16_t c, float f) {
    if (f >= 1.0f) return c;
    if (f <= 0.0f) return 0;

    uint16_t n = (uint16_t)((c >> 8) | (c << 8));
    uint32_t r = (uint32_t)(((n >> 11) & 0x1F) * f);
    uint32_t g = (uint32_t)(((n >> 5)  & 0x3F) * f);
    uint32_t b = (uint32_t)(( n        & 0x1F) * f);

    uint16_t out = (uint16_t)((r << 11) | (g << 5) | b);
    return (uint16_t)((out >> 8) | (out << 8));
}

uint16_t vg_mix(uint16_t a, uint16_t b, float t) {
    if (t <= 0.0f) return a;
    if (t >= 1.0f) return b;

    uint16_t na = (uint16_t)((a >> 8) | (a << 8));
    uint16_t nb = (uint16_t)((b >> 8) | (b << 8));

    float ar = (float)((na >> 11) & 0x1F), br = (float)((nb >> 11) & 0x1F);
    float ag = (float)((na >> 5)  & 0x3F), bg = (float)((nb >> 5)  & 0x3F);
    float ab = (float)( na        & 0x1F), bb = (float)( nb        & 0x1F);

    uint32_t r  = (uint32_t)(ar + (br - ar) * t);
    uint32_t g  = (uint32_t)(ag + (bg - ag) * t);
    uint32_t bl = (uint32_t)(ab + (bb - ab) * t);

    uint16_t o = (uint16_t)((r << 11) | (g << 5) | bl);
    return (uint16_t)((o >> 8) | (o << 8));
}

// ---------------------------------------------------------------------------
// Spherical HUD warp
// ---------------------------------------------------------------------------

void vg_hud_warp(bool on, float scale) {
    Sub* u = sub();
    u->warp   = on;
    u->warp_k = HUD_WARP_K * scale;
}

void vg_hud_jitter(float dx, float dy) { Sub* u = sub(); u->jx = dx; u->jy = dy; }

static inline void warp_pt(float* x, float* y) {
    const Sub* u = sub();
    float dx = *x - SCR_CX, dy = *y - SCR_CY;
    float r2 = (dx * dx + dy * dy) * (1.0f / (SCR_CX * SCR_CX + SCR_CY * SCR_CY));
    float k  = 1.0f + u->warp_k * r2;
    // Displacement is added AFTER the bend, so the whole assembly translates as
    // one rather than the curvature being recomputed about a moved centre --
    // the panel vibrates, it does not flex.
    //
    // Every instrument routes through here when the warp bracket is open: lines
    // subdivide through it and fills go out as warped quads through it. So this
    // is the one point that moves the entire panel and nothing else, and the
    // world, which is drawn outside the bracket, is untouched.
    *x = SCR_CX + dx * k + u->jx;
    *y = SCR_CY + dy * k + u->jy;
}

// The bend of ONE point at an arbitrary warp scale, with no jitter and without
// consulting whether the bracket is open.
//
// For the rear-view patch, which cannot be warped -- it is a viewport, and
// bending it would bend the picture inside it -- but which has to ride the panel
// that is. The caller asks where the panel put this spot and moves the whole
// window there rigidly. Same formula as warp_pt so the two cannot drift apart.
void vg_hud_warp_at(float scale, float x, float y, float* ox, float* oy) {
    const float dx = x - SCR_CX, dy = y - SCR_CY;
    const float r2 = (dx * dx + dy * dy) * (1.0f / (SCR_CX * SCR_CX + SCR_CY * SCR_CY));
    const float k  = 1.0f + HUD_WARP_K * scale * r2;
    *ox = SCR_CX + dx * k;
    *oy = SCR_CY + dy * k;
}

// ---------------------------------------------------------------------------
// Orientation
//
// Applied after the warp, so everything upstream works in a single logical frame
// with the buttons at the top and never has to think about how the panel is
// actually scanned.
// ---------------------------------------------------------------------------

static inline void rot_pt(float* x, float* y) {
#if VG_ROTATE == 1
    float ox = *x, oy = *y;
    *x = oy;
    *y = (float)(SCR_H - 1) - ox;
#elif VG_ROTATE == 2
    *x = (float)(SCR_W - 1) - *x;
    *y = (float)(SCR_H - 1) - *y;
#elif VG_ROTATE == 3
    float ox = *x, oy = *y;
    *x = (float)(SCR_W - 1) - oy;
    *y = ox;
#else
    (void)x; (void)y;
#endif
}

// A quarter turn maps an axis-aligned rectangle to another axis-aligned
// rectangle with width and height exchanged, so fills stay cheap.
static inline void rot_rect(int* x, int* y, int* w, int* h) {
#if VG_ROTATE == 1
    int nx = *y, ny = SCR_H - *x - *w, nw = *h, nh = *w;
    *x = nx; *y = ny; *w = nw; *h = nh;
#elif VG_ROTATE == 2
    *x = SCR_W - *x - *w;
    *y = SCR_H - *y - *h;
#elif VG_ROTATE == 3
    int nx = SCR_W - *y - *h, ny = *x, nw = *h, nh = *w;
    *x = nx; *y = ny; *w = nw; *h = nh;
#else
    (void)x; (void)y; (void)w; (void)h;
#endif
}

// Logical rectangle -> panel rectangle. Exported because the backdrop needs the
// same mapping and duplicating a quarter turn is how two of them end up
// disagreeing.
void vg_rast_rot_rect(int* x, int* y, int* w, int* h) { rot_rect(x, y, w, h); }

// Submit the rear-view patch's backdrop. Carries only its row span: where it is
// and what it samples are the backdrop's business, and the band raster hands it
// the buffer to fill.
void vg_sky_patch_prim(int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return;
    rot_rect(&x, &y, &w, &h);
    Prim* p = push();
    if (!p) return;
    p->type  = PRIM_SKY;
    p->x0 = p->y0 = p->x1 = p->y1 = 0;
    p->color = 1;                       // non-zero: nothing skips it as blank
    p->ymin  = (int16_t)(y < 0 ? 0 : y);
    p->ymax  = (int16_t)((y + h - 1) > SCR_H - 1 ? SCR_H - 1 : (y + h - 1));
}

// ---------------------------------------------------------------------------
// Cohen-Sutherland clip against the full screen, done once here so band
// rasterisation never walks an off-screen span.
// ---------------------------------------------------------------------------

// The clip window, in PANEL space, inclusive. The whole screen unless somebody
// has asked for a viewport -- the rear-view patch is the only caller so far.
//
// Panel space and not logical, because rot_pt has already run by the time
// anything is clipped. vg_rast_viewport takes the rectangle the way the game
// thinks about it and turns it once, here, rather than making every caller
// know which way the panel is scanned.
void vg_rast_viewport(int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return;
    rot_rect(&x, &y, &w, &h);
    int x1 = x + w - 1, y1 = y + h - 1;
    if (x  < 0) x = 0;
    if (y  < 0) y = 0;
    if (x1 > SCR_W - 1) x1 = SCR_W - 1;
    if (y1 > SCR_H - 1) y1 = SCR_H - 1;
    Sub* u = sub();
    u->cx0 = x; u->cy0 = y; u->cx1 = x1; u->cy1 = y1;
}

void vg_rast_viewport_full(void) {
    Sub* u = sub();
    u->cx0 = 0; u->cy0 = 0; u->cx1 = SCR_W - 1; u->cy1 = SCR_H - 1;
}

static inline int outcode(float x, float y) {
    const Sub* u = sub();
    int c = 0;
    if      (x < u->cx0) c |= 1;
    else if (x > u->cx1) c |= 2;
    if      (y < u->cy0) c |= 4;
    else if (y > u->cy1) c |= 8;
    return c;
}

static bool clip_screen(float* px0, float* py0, float* px1, float* py1) {
    const Sub* u = sub();
    float ax = *px0, ay = *py0, bx = *px1, by = *py1;
    int c0 = outcode(ax, ay), c1 = outcode(bx, by);

    for (int guard = 0; guard < 8; guard++) {
        if (!(c0 | c1)) { *px0 = ax; *py0 = ay; *px1 = bx; *py1 = by; return true; }
        if (c0 & c1)    return false;

        int   c = c0 ? c0 : c1;
        float x = 0, y = 0;
        if (c & 8)      { y = (float)u->cy1; x = ax + (bx - ax) * (y - ay) / (by - ay); }
        else if (c & 4) { y = (float)u->cy0; x = ax + (bx - ax) * (y - ay) / (by - ay); }
        else if (c & 2) { x = (float)u->cx1; y = ay + (by - ay) * (x - ax) / (bx - ax); }
        else            { x = (float)u->cx0; y = ay + (by - ay) * (x - ax) / (bx - ax); }

        if (!isfinite(x) || !isfinite(y)) return false;

        if (c == c0) { ax = x; ay = y; c0 = outcode(ax, ay); }
        else         { bx = x; by = y; c1 = outcode(bx, by); }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Submit
// ---------------------------------------------------------------------------

static void line_raw(float x0, float y0, float x1, float y1, uint16_t color) {
    if (!color) return;
    rot_pt(&x0, &y0);
    rot_pt(&x1, &y1);
    if (!isfinite(x0) || !isfinite(y0) || !isfinite(x1) || !isfinite(y1)) return;
    if (!clip_screen(&x0, &y0, &x1, &y1)) return;

    Prim* p = push();
    if (!p) return;
    p->type  = PRIM_LINE;
    p->aa    = sub()->aa;
    p->x0    = (int16_t)lrintf(x0);
    p->y0    = (int16_t)lrintf(y0);
    p->x1    = (int16_t)lrintf(x1);
    p->y1    = (int16_t)lrintf(y1);
    p->color = color;
    p->ymin  = p->y0 < p->y1 ? p->y0 : p->y1;
    p->ymax  = p->y0 < p->y1 ? p->y1 : p->y0;
}

void vg_line(float x0, float y0, float x1, float y1, uint16_t color) {
    if (!sub()->warp) { line_raw(x0, y0, x1, y1, color); return; }

    // Subdivide, or the warp would just displace the endpoints and leave the
    // line straight between them.
    float dx = x1 - x0, dy = y1 - y0;
    float len = sqrtf(dx * dx + dy * dy);
    int   n   = (int)(len / HUD_WARP_SEG) + 1;
    if (n > 10) n = 10;

    float px = x0, py = y0;
    warp_pt(&px, &py);
    for (int i = 1; i <= n; i++) {
        float t  = (float)i / (float)n;
        float cx = x0 + dx * t, cy = y0 + dy * t;
        warp_pt(&cx, &cy);
        line_raw(px, py, cx, cy, color);
        px = cx; py = cy;
    }
}

void vg_line_w(float x0, float y0, float x1, float y1, uint16_t color, int w) {
    if (w <= 1) { vg_line(x0, y0, x1, y1, color); return; }

    float dx = x1 - x0, dy = y1 - y0;
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 1e-3f) { vg_line(x0, y0, x1, y1, color); return; }

    // Screen-space normal; offset copies straddle the true line so the stroke
    // stays centred on the geometry.
    float px = -dy / len, py = dx / len;
    float start = -(float)(w - 1) * 0.5f;
    for (int i = 0; i < w; i++) {
        float o = start + (float)i;
        vg_line(x0 + px * o, y0 + py * o, x1 + px * o, y1 + py * o, color);
    }
}

void vg_point(int x, int y, uint16_t color) {
    const Sub* u = sub();
    if (!color) return;
    {
        float fx = (float)x, fy = (float)y;
        rot_pt(&fx, &fy);
        x = (int)lrintf(fx);
        y = (int)lrintf(fy);
    }
    if (x < u->cx0 || x > u->cx1 || y < u->cy0 || y > u->cy1) return;

    Prim* p = push();
    if (!p) return;
    p->type  = PRIM_POINT;
    p->x0    = (int16_t)x;
    p->y0    = (int16_t)y;
    p->x1    = 0;
    p->y1    = 0;
    p->color = color;
    p->ymin  = p->ymax = (int16_t)y;
}

static void fill_rect_raw(int x, int y, int w, int h, uint16_t color) {
    const Sub* u = sub();
    if (!color || w <= 0 || h <= 0) return;
    rot_rect(&x, &y, &w, &h);
    if (x < u->cx0) { w += x - u->cx0; x = u->cx0; }
    if (y < u->cy0) { h += y - u->cy0; y = u->cy0; }
    if (x + w > u->cx1 + 1) w = u->cx1 + 1 - x;
    if (y + h > u->cy1 + 1) h = u->cy1 + 1 - y;
    if (w <= 0 || h <= 0) return;

    Prim* p = push();
    if (!p) return;
    p->type  = PRIM_FILL;
    p->x0    = (int16_t)x;
    p->y0    = (int16_t)y;
    p->x1    = (int16_t)w;
    p->y1    = (int16_t)h;
    p->color = color;
    p->ymin  = (int16_t)y;
    p->ymax  = (int16_t)(y + h - 1);
}

void vg_fill_rect(int x, int y, int w, int h, uint16_t color) {
    if (!sub()->warp) { fill_rect_raw(x, y, w, h, color); return; }
    if (!color || w <= 0 || h <= 0) return;

    // A warped rectangle is no longer axis-aligned, so it goes out as a strip of
    // quads (two triangles each), subdivided along its long axis so the bend is
    // visible rather than just a displaced box.
    const bool horiz = (w >= h);
    const int  span  = horiz ? w : h;
    int n = (int)((float)span / HUD_WARP_SEG) + 1;
    if (n > 8) n = 8;

    for (int i = 0; i < n; i++) {
        float a0 = (float)(horiz ? x : y) + (float)span * (float)i       / (float)n;
        float a1 = (float)(horiz ? x : y) + (float)span * (float)(i + 1) / (float)n;

        float qx[4], qy[4];
        if (horiz) {
            qx[0] = a0; qy[0] = (float)y;
            qx[1] = a1; qy[1] = (float)y;
            qx[2] = a1; qy[2] = (float)(y + h);
            qx[3] = a0; qy[3] = (float)(y + h);
        } else {
            qx[0] = (float)x;       qy[0] = a0;
            qx[1] = (float)(x + w); qy[1] = a0;
            qx[2] = (float)(x + w); qy[2] = a1;
            qx[3] = (float)x;       qy[3] = a1;
        }
        for (int k = 0; k < 4; k++) warp_pt(&qx[k], &qy[k]);

        // vg_tri applies the rotation, so these stay in logical space.
        vg_tri(qx[0], qy[0], qx[1], qy[1], qx[2], qy[2], color);
        vg_tri(qx[0], qy[0], qx[2], qy[2], qx[3], qy[3], color);
    }
}

void vg_rect(int x, int y, int w, int h, uint16_t color) {
    if (w <= 0 || h <= 0) return;
    vg_fill_rect(x,         y,         w, 1, color);
    vg_fill_rect(x,         y + h - 1, w, 1, color);
    vg_fill_rect(x,         y,         1, h, color);
    vg_fill_rect(x + w - 1, y,         1, h, color);
}

// Hidden-line fills, on by default.
//
// Exists for the viewport. A triangle is clipped by its bounding box at submit
// and by the FULL screen width when its spans are walked in vg_band.cpp, which
// does not know a viewport exists -- so a face crossing the edge of a patch
// would bleed across the screen. Widening the primitive to carry an x range
// would cost eight bytes on every primitive in the frame to fix a case that
// only the patch has.
//
// So the patch draws wireframe. It is cheaper, and at that size a solid hull is
// a blob: the thing that reads is the outline.
void vg_rast_fills(bool on) { sub()->fills = on; }

// The baked canopy: one primitive covering the whole panel, so the band raster runs
// the table at the right point in the order. No coordinates -- the table is in panel
// space already, which is also why nothing here goes through rot_pt or the warp.
void vg_canopy_prim(void) {
    Prim* p = push();
    if (!p) return;
    p->type  = PRIM_CANOPY;
    p->aa    = LINE_OPAQUE;
    p->color = 0;
    p->x0 = p->y0 = p->x1 = p->y1 = p->x2 = p->y2 = 0;
    p->ymin = 0;
    p->ymax = (int16_t)(SCR_H - 1);
}

// The console chassis: one primitive covering the whole panel, submitted before
// the menu so the menu draws on top of it. Beside vg_canopy_prim because push()
// is private to this file and both want it for the same reason.
void vg_bezel_prim(void) {
    if (!vg_bezel_current()) return;
    Prim* p = push();
    if (!p) return;
    p->type  = PRIM_BEZEL;
    p->aa    = LINE_OPAQUE;
    p->color = 0;
    p->x0 = p->y0 = p->x1 = p->y1 = p->x2 = p->y2 = 0;
    p->ymin = 0;
    p->ymax = (int16_t)(SCR_H - 1);
}

void vg_tri(float x0, float y0, float x1, float y1, float x2, float y2, uint16_t color) {
    const Sub* u = sub();
    if (!sub()->fills) return;
    if (!isfinite(x0) || !isfinite(y0) || !isfinite(x1) ||
        !isfinite(y1) || !isfinite(x2) || !isfinite(y2)) return;
    rot_pt(&x0, &y0);
    rot_pt(&x1, &y1);
    rot_pt(&x2, &y2);

    // Trivial reject against the screen.
    float minx = x0 < x1 ? (x0 < x2 ? x0 : x2) : (x1 < x2 ? x1 : x2);
    float maxx = x0 > x1 ? (x0 > x2 ? x0 : x2) : (x1 > x2 ? x1 : x2);
    float miny = y0 < y1 ? (y0 < y2 ? y0 : y2) : (y1 < y2 ? y1 : y2);
    float maxy = y0 > y1 ? (y0 > y2 ? y0 : y2) : (y1 > y2 ? y1 : y2);
    if (maxx < u->cx0 || minx > u->cx1 || maxy < u->cy0 || miny > u->cy1) return;

    // Vertices are stored unclipped so the scanline interpolation stays exact;
    // the per-band fill clamps spans instead. Clamping to +-16000 only bites for
    // geometry within half a unit of the near plane, which is already inside a
    // collision.
    #define TCLAMP(v) ((int16_t)((v) < -16000.0f ? -16000 : ((v) > 16000.0f ? 16000 : (int)lrintf(v))))

    Prim* p = push();
    if (!p) return;
    p->type  = PRIM_TRI;
    sub()->tri++;
    p->x0 = TCLAMP(x0); p->y0 = TCLAMP(y0);
    p->x1 = TCLAMP(x1); p->y1 = TCLAMP(y1);
    p->x2 = TCLAMP(x2); p->y2 = TCLAMP(y2);
    p->color = color;
    // Carries the blend mode, so vg_line_blend brackets a FILL as well as a line --
    // which is what lets a broad canopy member be one fill instead of twenty lines.
    // Everywhere else this is LINE_OPAQUE, and the band fill only branches on
    // ADD/SUB, so an antialiasing request on a triangle stays a no-op as before.
    p->aa   = u->aa;
    p->ymin = (int16_t)(miny < u->cy0 ? u->cy0 : (int)miny);
    p->ymax = (int16_t)(maxy > u->cy1 ? u->cy1 : (int)maxy);
    #undef TCLAMP
}

int vg_text_width(const char* s, int scale) {
    int n = 0;
    while (s[n]) n++;
    return n > 0 ? (n * 6 - 1) * scale : 0;
}

// NOTE: colour 0 means INVISIBLE here, not black -- passing COL_BLACK draws
// nothing at all. Inverse video (dark glyphs on a lit fill) must use INK_ONFILL,
// which is the palette entry that exists for exactly that.
// TEXT NOW OBEYS THE VIEWPORT, and it did not before.
//
// vg_rast_viewport clipped fills and lines and had no effect on a glyph at all --
// vg_text tested a character against the SCREEN and against nothing else. A caller
// that set a viewport and drew text got no clip and no complaint, which is the
// worst of the three outcomes: the ship-select ticker was "clipped" to its window
// for two builds and was never clipped to anything.
//
// The clip costs no memory. A glyph's panel extents are already computed here for
// ymin/ymax, and x2/y2 are unused by PRIM_GLYPH -- the struct comment says they
// belong to PRIM_TRI. Clamping the four to the viewport turns the band raster's
// existing bounds tests into the clip, so the cut lands in the loops that were
// already testing bounds and adds nothing to the inner one.
void vg_text(int x, int y, const char* s, uint16_t color, int scale) {
    if (!color || scale <= 0) return;
    const int gh = 7 * scale;
    const Sub* u = sub();

    for (; *s; s++, x += 6 * scale) {
        char ch = *s;
        if (ch >= 'a' && ch <= 'z') ch -= 32;
        if (ch < VG_FONT_FIRST || ch > VG_FONT_LAST || ch == ' ') continue;
        if (x + 5 * scale < 0 || x >= SCR_W) continue;

        // Glyphs stay upright and unbent; only their origin follows the curve.
        // Warping the bitmaps themselves would cost far more and read worse at
        // this size than letting the baseline arc.
        float gx = (float)x, gy = (float)y;
        if (sub()->warp) warp_pt(&gx, &gy);
        rot_pt(&gx, &gy);

        const int px = (int)lrintf(gx), py = (int)lrintf(gy);

        // The origin is the rotated LOGICAL top-left, so the glyph's panel-space
        // extent runs a different way per quadrant.
        int ylo, yhi, xlo, xhi;
#if VG_ROTATE == 1
        ylo = py - (5 * scale - 1); yhi = py;
        xlo = px;                   xhi = px + gh - 1;
#elif VG_ROTATE == 2
        ylo = py - (gh - 1);        yhi = py;
        xlo = px - (5 * scale - 1); xhi = px;
#elif VG_ROTATE == 3
        ylo = py;                   yhi = py + (5 * scale - 1);
        xlo = px - (gh - 1);        xhi = px;
#else
        ylo = py;                   yhi = py + gh - 1;
        xlo = px;                   xhi = px + (5 * scale - 1);
#endif
        // The viewport is already in panel space -- see fill_rect_raw, which
        // rotates first and clips against these same bounds afterwards. Clamped
        // BEFORE the primitive is taken, so a character outside the viewport
        // costs nothing and occupies no slot.
        if (ylo < u->cy0) ylo = u->cy0;
        if (yhi > u->cy1) yhi = u->cy1;
        if (xlo < u->cx0) xlo = u->cx0;
        if (xhi > u->cx1) xhi = u->cx1;
        if (ylo > yhi || xlo > xhi) continue;

        Prim* p = push();
        if (!p) return;
        p->type  = PRIM_GLYPH;
        p->x0    = (int16_t)px;
        p->y0    = (int16_t)py;
        p->x1    = (int16_t)scale;
        p->y1    = (int16_t)ch;
        p->color = color;
        p->ymin  = (int16_t)ylo;
        p->ymax  = (int16_t)yhi;
        p->x2    = (int16_t)xlo;
        p->y2    = (int16_t)xhi;
    }
}
