#include "vg_replay.h"
#include "esp_task_wdt.h"
#include "vg_game.h"
#include "vg_raster.h"   // NUM_BANDS via cfg_display.h, and the band window
#include "vg_ship.h"
#include "vg_capture.h"
#include <Arduino.h>
#include "esp_attr.h"   // RTC_NOINIT_ATTR, for the arm that outlives a reset
#include "soc/extmem_reg.h"   // the cache miss counters
#include <esp_random.h>
#include "vg_prof.h"     // the submit halves and the mixer, for the slow-frame list
#include <string.h>

// Seeds drawn in a single frame. Two is the real maximum today (a sky
// regenerate takes a kind and a seed); the headroom costs 64 bytes and means a
// future draw cannot silently truncate the log and desynchronise a replay.
#define RP_MAX_RAND 16

// The persisted progress a session starts from. Mirrors what vg_save_load
// applies, rather than the SaveRecord itself -- this only has to survive the
// round trip to the host and back, so it does not need the on-flash layout and
// does not break when that layout changes.
struct RpSave {
    uint16_t credits;
    char     callsign[4];
    uint8_t  ship;
    uint8_t  champion;
    float    hue;
};

static int      s_mode   = VG_RP_OFF;
static uint32_t s_index  = 0;
static uint32_t s_rand[RP_MAX_RAND];
static int      s_rand_n = 0;    // logged this frame (RECORD) / supplied (PLAY)
static int      s_rand_i = 0;    // next to hand out (PLAY)

int  vg_replay_mode(void)          { return s_mode; }
bool vg_replay_suppress_save(void) { return s_mode == VG_RP_PLAY; }

// TIMED REPLAY STATE. Sums rather than a per-frame log: 5,444 frames of five counters is
// 109 KB and there is nowhere to put it, while the mean over an identical session is
// exactly the comparison this is for. The worst frame comes along because a mean hides the
// case that actually drops a frame.
static bool     s_timed = false;
// How many times the record stream had to be realigned. Non-zero means the link dropped
// bytes and the session is one record short for each -- the numbers are still sound, but
// the run is not the same length as the file.
static uint32_t s_resync = 0;

// WHY THE LAST REPLAY ENDED. Six ways out of vg_replay_next and they were indistinguishable
// from the host, which sees only that the acknowledgements stopped.
//
// A run of ordnance.phr stops between frames 1000 and 1500, at a different frame each time,
// while regress.phr completes all 5,444 -- so it is neither the firmware nor the protocol,
// and a guess at which of the six it is would be a guess. This says.
static const char* s_why = "not started";
static uint32_t s_t_n   = 0;
static uint32_t s_t_sum[5], s_t_max[5];
static uint32_t s_c_sum[3];   // the canopy on core 0, on core 1, and the split row
// The `world` split for the same frames, kept the same way. Reported on its own line
// because the two answer different questions: the costs above are "what does a frame come
// to", and these are "which part of it grows when the fight gets busy".
static uint32_t s_w_sum[7], s_w_max[7];
// And the blit split -- see vg_replay_note_blit for what each one means.
static uint32_t s_b_sum[8], s_b_max[8];
// Per band, and NUM_BANDS is fixed by the panel and BAND_H.
static uint32_t s_bd_sum[NUM_BANDS];
static uint32_t s_s_sum[6], s_s_max[6];
static uint32_t s_i0_sum = 0, s_i0_max = 0;
static uint32_t s_ty_sum[5], s_ty_max[5];

void vg_replay_note_world(uint32_t motes, uint32_t rocks, uint32_t trails,
                          uint32_t ships, uint32_t msl, uint32_t fire,
                          uint32_t total) {
    if (!s_timed) return;
    // `total` is g_sub_world, the whole these six are a split OF. Carried so the host can
    // SUBTRACT rather than trust that they tile -- they are supposed to, the brackets cover
    // the function end to end, and the live telemetry still showed 35-40% unaccounted. One
    // of those two things is wrong and this is what will say which.
    const uint32_t v[7] = { motes, rocks, trails, ships, msl, fire, total };
    for (int i = 0; i < 7; i++) {
        s_w_sum[i] += v[i];
        if (v[i] > s_w_max[i]) s_w_max[i] = v[i];
    }
}

void vg_replay_note_blit(uint32_t join, uint32_t wait, uint32_t push,
                         uint32_t res, uint32_t over_n, uint32_t over_us,
                         uint32_t sky, uint32_t scan) {
    if (!s_timed) return;
    // `rast` is sky + prim + scan. Two of the three are reported elsewhere, so carrying
    // these means the third needs no subtraction to find.
    const uint32_t v[8] = { join, wait, push, res, over_n, over_us, sky, scan };
    for (int i = 0; i < 8; i++) {
        s_b_sum[i] += v[i];
        if (v[i] > s_b_max[i]) s_b_max[i] = v[i];
    }
}

void vg_replay_note_bands(const uint32_t* band_us, int n) {
    if (!s_timed || !band_us) return;
    if (n > NUM_BANDS) n = NUM_BANDS;
    for (int i = 0; i < n; i++) s_bd_sum[i] += band_us[i];
}

// THE WORST WAIT, WITH ITS OWN FRAME'S A AND B BESIDE IT. Three independent maxima
// cannot say whether the worst wait happened because B spiked -- they are three
// different frames. This captures the pair AT the frame that set the record, and the
// count of waits over 100 us, which says whether the tail is one event or a habit.
static uint32_t s_cw_worst = 0, s_cw_frame = 0;
static uint32_t s_cw_can = 0, s_cw_rast = 0, s_cw_prim = 0, s_cw_sub = 0, s_cw_upd = 0;
// AND THE OTHER TWO THIRDS OF THE RASTER. `rast` is sky + prim + scan and only prim was
// captured here, so a worst frame whose raster was scanlines said nothing about why --
// which is exactly the frame 423 case, 15.9 ms of raster with the canopy at zero. `tv`
// is the part of scan that was the transition, and is a subset of it.
static uint32_t s_cw_scan = 0, s_cw_tv = 0;
static uint32_t s_cw_over16 = 0, s_cw_over20 = 0;
static uint32_t s_cw_upd_stage[11] = {0};
// THE TAIL, NOT ITS MAXIMUM. One worst frame cannot describe a hundred slow ones,
// and a hundred slow ones is what the pilot reports as "sometimes". Six worst
// frames with their parts, and a histogram of where every frame lands.
#define SLOW_TOP 6
static uint32_t s_top_ft[SLOW_TOP]  = {0};
static uint32_t s_top_fr[SLOW_TOP]  = {0};
static uint32_t s_top_upd[SLOW_TOP] = {0};
static uint32_t s_top_sub[SLOW_TOP] = {0};
static uint32_t s_top_rast[SLOW_TOP]= {0};
static uint32_t s_top_can[SLOW_TOP] = {0};
static uint32_t s_top_prim[SLOW_TOP]= {0};
static uint32_t s_top_scan[SLOW_TOP]= {0};
static uint32_t s_top_tv[SLOW_TOP]  = {0};
// WHAT THE SUBMIT WAS MADE OF on a slow frame. `sub` alone said 11 ms on three
// fight frames and could not say whether that was the world, the instruments, the
// rendezvous or the mixer -- which renders inline under a replay and, on a frame
// the recording itself took slowly, renders a long one. Read straight off the
// globals: submit has finished by the time the cost is noted.
static uint32_t s_top_a[SLOW_TOP]   = {0};
static uint32_t s_top_b[SLOW_TOP]   = {0};
static uint32_t s_top_wait[SLOW_TOP]= {0};
static uint32_t s_top_sxr[SLOW_TOP] = {0};
static uint32_t s_top_wrld[SLOW_TOP]= {0};
// The five raster types for the worst frame. note_types runs AFTER note_cost in the
// same frame, so note_cost raises this flag and note_types answers it.
static bool     s_top_want = false;
static uint32_t s_top_ty[5] = {0};
// 60+, 57-60, 54-57, 50-54, under 50. The pilot said 54, so the buckets straddle it.
static uint32_t s_hist[5] = {0};
static uint32_t s_sw_a = 0, s_sw_b = 0, s_sw_frame = 0, s_sw_over = 0;
static uint32_t s_wp_sum = 0, s_wp_max = 0, s_wp_n = 0, s_sw_warp = 0;

void vg_replay_note_sub(uint32_t a, uint32_t b, uint32_t wait,
                        uint32_t arena, uint32_t star, uint32_t hud) {
    if (!s_timed) return;
    const uint32_t v[6] = { a, b, wait, arena, star, hud };
    for (int i = 0; i < 6; i++) {
        s_s_sum[i] += v[i];
        if (v[i] > s_s_max[i]) s_s_max[i] = v[i];
    }
    extern uint32_t g_sub_warp;
    s_wp_sum += g_sub_warp; if (g_sub_warp > s_wp_max) s_wp_max = g_sub_warp;
    if (g_sub_warp > 50u) s_wp_n++;              // a frame that actually rebuilt
    if (wait > 100u) s_sw_over++;
    if (wait >= s_s_max[2]) { s_sw_a = a; s_sw_b = b; s_sw_frame = s_t_n; s_sw_warp = g_sub_warp; }
}

void vg_replay_note_idle0(uint32_t us) {
    if (!s_timed) return;
    s_i0_sum += us;
    if (us > s_i0_max) s_i0_max = us;
}

void vg_replay_note_types(uint32_t aa, uint32_t ln, uint32_t tri,
                          uint32_t gl, uint32_t fl) {
    if (!s_timed) return;
    if (s_top_want) {
        s_top_want = false;
        const uint32_t t[5] = { aa, ln, tri, gl, fl };
        for (int i = 0; i < 5; i++) s_top_ty[i] = t[i];
    }
    const uint32_t v[5] = { aa, ln, tri, gl, fl };
    for (int i = 0; i < 5; i++) {
        s_ty_sum[i] += v[i];
        if (v[i] > s_ty_max[i]) s_ty_max[i] = v[i];
    }
}

// ibus_acs, ibus_miss, dbus_acs, dbus_flash_miss, dbus_psram_miss
static uint32_t s_ch_prev[5];
static uint64_t s_ch_sum[5];
static uint32_t s_ch_max[5];
static bool     s_ch_primed = false;

// One frame in this many is folded. 5,444 frames gives about twenty samples, which
// is more sample points than the host-side regression ever rendered, at a total cost
// under a tenth of a second.
#define BAND_HASH_EVERY 256

// IN RTC MEMORY, and that is the whole trick. replay_cost.py resets the board
// before every run, so a flag armed over the serial line was gone by the time the
// replay started -- armed twice, printed nothing, twice. RTC slow memory survives a
// software reset, so `w` now arms the NEXT run the way it reads as doing. The magic
// guards a cold boot, where this word is whatever was in the die.
#define HASH_ARM_MAGIC 0x42414e44u   // 'BAND'
static RTC_NOINIT_ATTR uint32_t s_hash_magic;
static RTC_NOINIT_ATTR uint32_t s_hash_arm;
static bool hash_on(void) {
    return s_hash_magic == HASH_ARM_MAGIC && s_hash_arm != 0u;
}
static uint32_t s_band_hash = 2166136261u;
static uint32_t s_band_n    = 0;

void vg_replay_hash_arm(bool on) {
    s_hash_magic = HASH_ARM_MAGIC;
    s_hash_arm   = on ? 1u : 0u;
    s_band_hash  = 2166136261u;
    s_band_n     = 0;
}
bool vg_replay_hash_armed(void) { return hash_on(); }

void vg_replay_note_band(int band, const uint16_t* px, int n_px) {
    if (!s_timed || !hash_on() || !px) return;
    if ((s_t_n % BAND_HASH_EVERY) != 0) return;
    // The band index goes in too, so two bands swapping their contents is not a
    // silent pass. A word at a time: the buffer is 16-byte aligned and internal.
    uint32_t h = s_band_hash ^ (uint32_t)(band + 1);
    const uint32_t* w = (const uint32_t*)(const void*)px;
    const int nw = n_px >> 1;
    for (int i = 0; i < nw; i++) h = (h ^ w[i]) * 16777619u;
    s_band_hash = h;
    s_band_n++;
}

void vg_replay_note_cache(void) {
    if (!s_timed) { s_ch_primed = false; return; }
    // The flash-vs-PSRAM split of the dbus miss counter is classified by a vaddr
    // window; programmed once to the flash data range so the split can be trusted.
    static bool windowed = false;
    if (!windowed) {
        REG_WRITE(EXTMEM_DBUS_TO_FLASH_START_VADDR_REG, 0x3C000000u);
        REG_WRITE(EXTMEM_DBUS_TO_FLASH_END_VADDR_REG,   0x3DFFFFFFu);
        windowed = true;
    }
    const uint32_t now[5] = {
        REG_READ(EXTMEM_IBUS_ACS_CNT_REG),  REG_READ(EXTMEM_IBUS_ACS_MISS_CNT_REG),
        REG_READ(EXTMEM_DBUS_ACS_CNT_REG),  REG_READ(EXTMEM_DBUS_ACS_FLASH_MISS_CNT_REG),
        REG_READ(EXTMEM_DBUS_ACS_SPIRAM_MISS_CNT_REG),
    };
    for (int i = 0; i < 5; i++) {
        const uint32_t d = now[i] - s_ch_prev[i];   // wrap-safe
        s_ch_prev[i] = now[i];
        if (s_ch_primed) { s_ch_sum[i] += d; if (d > s_ch_max[i]) s_ch_max[i] = d; }
    }
    s_ch_primed = true;
}

bool vg_replay_timed(void) { return s_timed; }

// THE LAST TIMED RUN'S COST, printed again on demand.
//
// The sums are statics and survive the replay ending, so the answer exists until the next
// run overwrites it. Printing it once, at the end, made collecting it a race the host kept
// losing: USB CDC discards what it writes while no host is attached, so an answer sent
// during a reconnect ceases to exist and the tool reports that the device said nothing.
//
// Asking for it cannot race. Returns false when there is nothing to report, which is the
// honest answer before any timed run and after a reboot.
// THE CANOPY BY CORE. `can` in the COST line is one band's slower half, summed --
// the number the frame waits for. These are the two halves themselves, and the
// row the band was cut at, so a `can` that equals the whole pass can be told
// from a pass that is simply that slow.
void vg_replay_note_can(uint32_t c0, uint32_t c1, uint32_t at) {
    if (!s_timed) return;
    s_c_sum[0] += c0; s_c_sum[1] += c1; s_c_sum[2] += at;
}

bool vg_replay_report_cost(void) {
    if (!s_t_n) return false;
    Serial.printf("vg_replay: COST frames %u | can %u/%u | rast %u/%u | "
                  "prim %u/%u | sub %u/%u | upd %u/%u  (mean/worst us)\n",
                  (unsigned)s_t_n,
                  (unsigned)(s_t_sum[0] / s_t_n), (unsigned)s_t_max[0],
                  (unsigned)(s_t_sum[1] / s_t_n), (unsigned)s_t_max[1],
                  (unsigned)(s_t_sum[2] / s_t_n), (unsigned)s_t_max[2],
                  (unsigned)(s_t_sum[3] / s_t_n), (unsigned)s_t_max[3],
                  (unsigned)(s_t_sum[4] / s_t_n), (unsigned)s_t_max[4]);
    Serial.printf("vg_replay: CAN c0 %u | c1 %u | at %u  (mean us; mean rows of a band on core 0)\n",
                  (unsigned)(s_c_sum[0] / s_t_n), (unsigned)(s_c_sum[1] / s_t_n),
                  (unsigned)(s_c_sum[2] / s_t_n));
    // A SECOND LINE, because the first is already at the edge of a terminal's width and
    // because these answer a different question. The WORST column matters more than the
    // mean here: `world` is a curve to flatten, so what is wanted is how far it bends.
    Serial.printf("vg_replay: WORLD motes %u/%u | rocks %u/%u | trails %u/%u | "
                  "ships %u/%u | msl %u/%u | fire %u/%u | TOTAL %u/%u  (mean/worst us)\n",
                  (unsigned)(s_w_sum[0] / s_t_n), (unsigned)s_w_max[0],
                  (unsigned)(s_w_sum[1] / s_t_n), (unsigned)s_w_max[1],
                  (unsigned)(s_w_sum[2] / s_t_n), (unsigned)s_w_max[2],
                  (unsigned)(s_w_sum[3] / s_t_n), (unsigned)s_w_max[3],
                  (unsigned)(s_w_sum[4] / s_t_n), (unsigned)s_w_max[4],
                  (unsigned)(s_w_sum[5] / s_t_n), (unsigned)s_w_max[5],
                  (unsigned)(s_w_sum[6] / s_t_n), (unsigned)s_w_max[6]);
    // A THIRD LINE, for the same reason there is a second: this one answers "is the wire
    // waiting on the CPU, or the CPU on the wire".
    Serial.printf("vg_replay: BLIT join %u/%u | wait %u/%u | push %u/%u | res %u/%u | "
                  "overn %u/%u | overus %u/%u | sky %u/%u | scan %u/%u | idle0 %u/%u"
                  "  (mean/worst)\n",
                  (unsigned)(s_b_sum[0] / s_t_n), (unsigned)s_b_max[0],
                  (unsigned)(s_b_sum[1] / s_t_n), (unsigned)s_b_max[1],
                  (unsigned)(s_b_sum[2] / s_t_n), (unsigned)s_b_max[2],
                  (unsigned)(s_b_sum[3] / s_t_n), (unsigned)s_b_max[3],
                  (unsigned)(s_b_sum[4] / s_t_n), (unsigned)s_b_max[4],
                  (unsigned)(s_b_sum[5] / s_t_n), (unsigned)s_b_max[5],
                  (unsigned)(s_b_sum[6] / s_t_n), (unsigned)s_b_max[6],
                  (unsigned)(s_b_sum[7] / s_t_n), (unsigned)s_b_max[7],
                  (unsigned)(s_i0_sum / s_t_n), (unsigned)s_i0_max);
    // AND WHICH BANDS. Means only -- the worst of a single band across a whole session is
    // one frame's accident, and what is wanted here is the shape of the drawing's cost.
    {
        char row[NUM_BANDS * 7 + 1];
        int  k = 0;
        for (int i = 0; i < NUM_BANDS && k < (int)sizeof(row) - 7; i++)
            k += snprintf(row + k, sizeof(row) - k, "%u ",
                          (unsigned)(s_bd_sum[i] / s_t_n));
        row[k > 0 ? k - 1 : 0] = 0;
        Serial.printf("vg_replay: BANDS/%u = %s\n", (unsigned)vg_rast_band_window_us(), row);
    }
    Serial.printf("vg_replay: SUB a %u/%u | b %u/%u | wait %u/%u | arena %u/%u | "
                  "star %u/%u | hud %u/%u  (mean/worst)\n",
                  (unsigned)(s_s_sum[0] / s_t_n), (unsigned)s_s_max[0],
                  (unsigned)(s_s_sum[1] / s_t_n), (unsigned)s_s_max[1],
                  (unsigned)(s_s_sum[2] / s_t_n), (unsigned)s_s_max[2],
                  (unsigned)(s_s_sum[3] / s_t_n), (unsigned)s_s_max[3],
                  (unsigned)(s_s_sum[4] / s_t_n), (unsigned)s_s_max[4],
                  (unsigned)(s_s_sum[5] / s_t_n), (unsigned)s_s_max[5]);
    // The picture's own hash, when the run was armed for it. Absent means nobody
    // asked, which is not the same as "the pixels matched".
    if (hash_on()) {
        Serial.printf("vg_replay: BANDH %08x over %u bands, 1 frame in %d"
                      "  -- TIMINGS ABOVE ARE POLLUTED BY THE FOLD\n",
                      (unsigned)s_band_hash, (unsigned)s_band_n, BAND_HASH_EVERY);
    }

    // Which frame the pilot would have felt, and what it was made of.
    Serial.printf("vg_replay: SLOWEST %u us (%u fps) at frame %u -- upd %u sub %u rast %u"
                  " (can %u prim %u scan %u tv %u) | %u frames under 60, %u under 50\n",
                  (unsigned)s_cw_worst, (unsigned)(s_cw_worst ? 1000000u / s_cw_worst : 0),
                  (unsigned)s_cw_frame, (unsigned)s_cw_upd, (unsigned)s_cw_sub,
                  (unsigned)s_cw_rast, (unsigned)s_cw_can, (unsigned)s_cw_prim,
                  (unsigned)s_cw_scan, (unsigned)s_cw_tv,
                  (unsigned)s_cw_over16, (unsigned)s_cw_over20);
    Serial.printf("vg_replay: FRAMES 60+ %u | 57-60 %u | 54-57 %u | 50-54 %u | under50 %u\n",
                  (unsigned)s_hist[0], (unsigned)s_hist[1], (unsigned)s_hist[2],
                  (unsigned)s_hist[3], (unsigned)s_hist[4]);
    for (int i = 0; i < SLOW_TOP; i++) {
        if (!s_top_ft[i]) break;
        Serial.printf("vg_replay: SLOW%d frame %u  %u us (%u fps)  upd %u sub %u rast %u"
                      " (can %u prim %u scan %u tv %u) | A %u B %u wait %u sxr %u world %u\n",
                      i, (unsigned)s_top_fr[i], (unsigned)s_top_ft[i],
                      (unsigned)(1000000u / s_top_ft[i]), (unsigned)s_top_upd[i],
                      (unsigned)s_top_sub[i], (unsigned)s_top_rast[i], (unsigned)s_top_can[i],
                      (unsigned)s_top_prim[i], (unsigned)s_top_scan[i], (unsigned)s_top_tv[i],
                      (unsigned)s_top_a[i], (unsigned)s_top_b[i], (unsigned)s_top_wait[i],
                      (unsigned)s_top_sxr[i], (unsigned)s_top_wrld[i]);
    }
    Serial.printf("vg_replay: SLOWEST-TYPES aa %u ln %u tri %u gl %u fl %u"
                  "  -- the raster of the worst frame\n",
                  (unsigned)s_top_ty[0], (unsigned)s_top_ty[1], (unsigned)s_top_ty[2],
                  (unsigned)s_top_ty[3], (unsigned)s_top_ty[4]);
    Serial.printf("vg_replay: SLOWEST-UPD pre %u ship %u arena %u sky %u field %u trail %u"
                  " enemy %u ord %u vfx %u ai %u combat %u\n",
                  (unsigned)s_cw_upd_stage[0], (unsigned)s_cw_upd_stage[1],
                  (unsigned)s_cw_upd_stage[2], (unsigned)s_cw_upd_stage[3],
                  (unsigned)s_cw_upd_stage[4], (unsigned)s_cw_upd_stage[5],
                  (unsigned)s_cw_upd_stage[6], (unsigned)s_cw_upd_stage[7],
                  (unsigned)s_cw_upd_stage[8], (unsigned)s_cw_upd_stage[9],
                  (unsigned)s_cw_upd_stage[10]);

    // The worst rendezvous, decomposed. If b_at is far above b's mean, core 0 was late
    // and core 1 paid for it; if a_at is small, core 1 arrived early and the gap is
    // just the halves being uneven that frame.
    Serial.printf("vg_replay: WAITMAX %u us at frame %u -- a %u b %u | %u frames over 100 us\n",
                  (unsigned)s_s_max[2], (unsigned)s_sw_frame,
                  (unsigned)s_sw_a, (unsigned)s_sw_b, (unsigned)s_sw_over);
    Serial.printf("vg_replay: WARP mean %u worst %u | %u frames rebuilt | %u us at the worst wait\n",
                  (unsigned)(s_t_n ? s_wp_sum / s_t_n : 0), (unsigned)s_wp_max,
                  (unsigned)s_wp_n, (unsigned)s_sw_warp);

    // Bytes per frame: icache misses fill 32 B lines, dcache 64 B. The flash/PSRAM
    // split is the number nobody has ever had -- the canopy tables against the sky.
    if (s_t_n) {
        Serial.printf("vg_replay: CACHE im %u/%u | dfm %u/%u | dpm %u/%u | ia %u da %u  (mean/worst misses)\n",
                      (unsigned)(s_ch_sum[1] / s_t_n), (unsigned)s_ch_max[1],
                      (unsigned)(s_ch_sum[3] / s_t_n), (unsigned)s_ch_max[3],
                      (unsigned)(s_ch_sum[4] / s_t_n), (unsigned)s_ch_max[4],
                      (unsigned)(s_ch_sum[0] / s_t_n), (unsigned)(s_ch_sum[2] / s_t_n));
    }
    extern uint32_t g_course_inner_sum, g_course_calls, g_course_draws;
    Serial.printf("vg_replay: COURSE inner_sum %u calls %u draws %u\n",
                  (unsigned)g_course_inner_sum, (unsigned)g_course_calls,
                  (unsigned)g_course_draws);
    extern uint32_t g_prim_hash, g_rng_hash, g_wall_hash, g_prim_hash_l, g_prim_hash_g, g_cam_hash, g_caminp_hash, g_cnt_hash;
    Serial.printf("vg_replay: PRIMH %08x RNGH %08x WALLH %08x LINESH %08x GLYPHH %08x CAMH %08x INPH %08x CNTH %08x\n",
                  (unsigned)g_prim_hash, (unsigned)g_rng_hash, (unsigned)g_wall_hash,
                  (unsigned)g_prim_hash_l, (unsigned)g_prim_hash_g, (unsigned)g_cam_hash, (unsigned)g_caminp_hash, (unsigned)g_cnt_hash);
    Serial.printf("vg_replay: TYPES aa %u/%u | ln %u/%u | tri %u/%u | gl %u/%u | "
                  "fl %u/%u  (mean/worst)\n",
                  (unsigned)(s_ty_sum[0] / s_t_n), (unsigned)s_ty_max[0],
                  (unsigned)(s_ty_sum[1] / s_t_n), (unsigned)s_ty_max[1],
                  (unsigned)(s_ty_sum[2] / s_t_n), (unsigned)s_ty_max[2],
                  (unsigned)(s_ty_sum[3] / s_t_n), (unsigned)s_ty_max[3],
                  (unsigned)(s_ty_sum[4] / s_t_n), (unsigned)s_ty_max[4]);
    return true;
}

// THE WORST FRAME, WITH ITS OWN PARTS. Five separate maxima are five different
// frames and cannot explain each other -- the same trap WAITMAX was built to escape.
// The pilot sees a dip; this says which frame it was and what that frame was doing.
// Ranked on upd + sub + rast, which is the CPU that has to fit inside the transfer.

void vg_replay_note_cost(uint32_t can, uint32_t rast, uint32_t prim,
                         uint32_t sub, uint32_t upd,
                         uint32_t scan, uint32_t tv) {
    if (!s_timed) return;
    const uint32_t v[5] = { can, rast, prim, sub, upd };
    for (int i = 0; i < 5; i++) {
        s_t_sum[i] += v[i];
        if (v[i] > s_t_max[i]) s_t_max[i] = v[i];
    }
    // The wire still has to run whatever the CPU does, so a frame costs about
    // max(rast, wire) plus the serial stages in front of it.
    const uint32_t wire = 11520u;
    const uint32_t ft   = upd + sub + (rast > wire ? rast : wire);
    if (ft > 16667u) s_cw_over16++;      // under 60 fps
    if (ft > 20000u) s_cw_over20++;      // under 50 fps
    s_hist[ft <= 16667u ? 0 : ft <= 17544u ? 1 : ft <= 18519u ? 2 : ft <= 20000u ? 3 : 4]++;
    // Six deep, insertion sorted: the list is tiny and this runs once a frame.
    if (ft > s_top_ft[SLOW_TOP - 1]) {
        int k = SLOW_TOP - 1;
        while (k > 0 && s_top_ft[k - 1] < ft) {
            s_top_ft[k]  = s_top_ft[k - 1];  s_top_fr[k]   = s_top_fr[k - 1];
            s_top_upd[k] = s_top_upd[k - 1]; s_top_sub[k]  = s_top_sub[k - 1];
            s_top_rast[k]= s_top_rast[k - 1]; s_top_can[k] = s_top_can[k - 1];
            s_top_prim[k]= s_top_prim[k - 1];
            s_top_scan[k]= s_top_scan[k - 1]; s_top_tv[k]  = s_top_tv[k - 1];
            s_top_a[k]   = s_top_a[k - 1];    s_top_b[k]   = s_top_b[k - 1];
            s_top_wait[k]= s_top_wait[k - 1]; s_top_sxr[k] = s_top_sxr[k - 1];
            s_top_wrld[k]= s_top_wrld[k - 1];
            k--;
        }
        s_top_ft[k] = ft; s_top_fr[k] = s_t_n;
        s_top_upd[k] = upd; s_top_sub[k] = sub; s_top_rast[k] = rast; s_top_can[k] = can;
        s_top_prim[k] = prim; s_top_scan[k] = scan; s_top_tv[k] = tv;
        s_top_a[k] = g_sub_a; s_top_b[k] = g_sub_b; s_top_wait[k] = g_sub_wait;
        s_top_sxr[k] = g_sfx_render_us; s_top_wrld[k] = g_sub_world;
        if (k == 0) s_top_want = true;   // the new leader wants its types
    }
    if (ft > s_cw_worst) {
        s_cw_worst = ft; s_cw_frame = s_t_n;
        s_cw_can = can; s_cw_rast = rast; s_cw_prim = prim; s_cw_sub = sub; s_cw_upd = upd;
        s_cw_scan = scan; s_cw_tv = tv;
        // AND WHICH SPAN OF THE UPDATE. A printf here cannot survive a replay -- the
        // link owns the port -- so the stages are captured and printed with the rest
        // of the report. They reset per telemetry window rather than per frame, so
        // they carry a little of the frames before this one; against a stall of a
        // quarter of a second that is noise.
        extern uint32_t g_upd_snap[11];
        for (int i = 0; i < 11; i++) s_cw_upd_stage[i] = g_upd_snap[i];
    }
    s_t_n++;
}

uint32_t vg_replay_rand(void) {
    if (s_mode == VG_RP_PLAY) {
        // Running out means the recording and the playback disagree about how
        // many seeds this frame needs, which is a desync. Returning a constant
        // keeps it reproducible instead of pulling in a fresh hardware value
        // and making the divergence worse every frame.
        return (s_rand_i < s_rand_n) ? s_rand[s_rand_i++] : 0u;
    }
    const uint32_t v = esp_random();
    if (s_mode == VG_RP_RECORD && s_rand_n < RP_MAX_RAND) s_rand[s_rand_n++] = v;
    return v;
}

// --- blocking host reads ---------------------------------------------------

static bool rd(void* p, int n, uint32_t to_ms) {
    uint8_t* b = (uint8_t*)p;
    uint32_t last = millis();
    int got = 0;
    while (got < n) {
        if (Serial.available()) { b[got++] = (uint8_t)Serial.read(); last = millis(); }
        else if (millis() - last > to_ms) return false;
        // Yield rather than spin. This loop is the whole frame budget while
        // replaying, and starving the USB driver task would stop the very
        // bytes we are waiting to be answered.
        else {
            // A render waits on the host for as long as the host needs, and the
            // crumb has recorded thirty seconds of it. That is the harness
            // working, not the game hanging, so the dog is fed here -- the
            // timeout above is what ends this wait.
            esp_task_wdt_reset();
            delay(0);
        }
    }
    return true;
}

// --- progress snapshot -----------------------------------------------------

static void snapshot(RpSave* s) {
    s->credits  = (uint16_t)((vg.credits < 0) ? 0 : vg.credits);
    memcpy(s->callsign, vg.callsign, 4);
    s->ship     = (uint8_t)vg.ship;
    s->champion = vg.champion ? 1u : 0u;
    s->hue      = vg.trail_hue;
}

static void restore(const RpSave* s) {
    vg.credits = (int)s->credits;
    for (int i = 0; i < 3; i++) {
        const char c = s->callsign[i];
        vg.callsign[i] = (c >= 'A' && c <= 'Z') ? c : 'A';
    }
    vg.callsign[3] = 0;
    vg.ship        = (s->ship < SHIP_CLASSES) ? (ShipClass)s->ship : SHIP_AEGIS;
    vg.spec        = vg_spec(vg.ship);
    vg.champion    = (s->champion != 0);
    vg.trail_hue   = s->hue;
    // Hull follows the class, exactly as loading a save does.
    vg.health_max  = vg.spec->hull;
    vg.health      = vg.health_max;
}

// --- record ----------------------------------------------------------------

static void begin_record(void) {
    vg_link_blocking(true);     // the header must not be dropped
    s_mode   = VG_RP_RECORD;
    s_index  = 0;
    s_rand_n = 0;

    // A session must start from a state the replay can reach. Mid-game is not
    // one: the bracket, the ship, the arena and every position would have to be
    // serialised. Restarting means the whole session is reproducible from a
    // seed list and an input log, which is a few hundred bytes.
    vg_game_init();

    RpSave sv;
    snapshot(&sv);

    const uint16_t ver = 1, blob = (uint16_t)sizeof(VgInput);
    const uint8_t  nr  = (uint8_t)s_rand_n;
    vg_link_write("PHRH", 4);
    vg_link_write(&ver, 2);
    vg_link_write(&blob, 2);
    vg_link_write(&nr, 1);
    vg_link_write(s_rand, (int)nr * 4);
    vg_link_write(&sv, (int)sizeof(sv));
    Serial.flush();

    s_rand_n = 0;                 // per-frame log starts clean
}

void vg_replay_note_frame(float dt, const VgInput* in) {
    if (s_mode != VG_RP_RECORD) return;
    const uint8_t nr = (uint8_t)s_rand_n;
    vg_link_write("PHRC", 4);
    vg_link_write(&s_index, 4);
    vg_link_write(&dt, 4);
    vg_link_write(&nr, 1);
    vg_link_write(s_rand, (int)nr * 4);
    vg_link_write(in, (int)sizeof(VgInput));
    s_index++;
    s_rand_n = 0;
}

// --- play ------------------------------------------------------------------

static void begin_play(void) {
    uint16_t ver = 0, blob = 0;
    uint8_t  nr  = 0;
    RpSave   sv;

    if (!rd(&ver, 2, 3000) || !rd(&blob, 2, 3000) || !rd(&nr, 1, 3000)) return;
    if (ver != 1 || blob != sizeof(VgInput) || nr > RP_MAX_RAND) {
        // Refusing is the point. A session recorded against a different VgInput
        // layout would replay as garbage input rather than as an error.
        Serial.printf("\nvg_replay: REJECT ver %u blob %u (want 1/%u)\n",
                      (unsigned)ver, (unsigned)blob, (unsigned)sizeof(VgInput));
        return;
    }
    if (nr && !rd(s_rand, (int)nr * 4, 3000)) return;
    if (!rd(&sv, sizeof(sv), 3000)) return;

    s_rand_n = nr;
    s_rand_i = 0;
    s_mode   = VG_RP_PLAY;
    vg_link_blocking(true);     // the PLAYING announce must not be dropped

    vg_game_init();               // consumes the recorded seeds
    restore(&sv);

    s_index = 0;
    s_rand_n = 0;
    vg_link_stats_reset();
    s_t_n = 0;
    s_resync = 0;
    s_why = "still running";
    for (int i = 0; i < 5; i++) { s_t_sum[i] = 0; s_t_max[i] = 0; }
    s_c_sum[0] = s_c_sum[1] = s_c_sum[2] = 0;
    for (int i = 0; i < 7; i++) { s_w_sum[i] = 0; s_w_max[i] = 0; }
    // Announce BEFORE the transmit task starts. After it starts, this core and
    // core 0 would both write to Serial, and two writers corrupt the stream.
    //
    // THE SAME WORD EITHER WAY. The host waits for "PLAYING" to know the device is ready,
    // and a timed run is still a replay starting -- so it says the same thing and the host
    // needs no second case for it.
    Serial.printf("\nvg_replay: PLAYING\n");
    Serial.flush();
    // Streamed only when somebody wants the pixels. A timed run wants the clock instead,
    // and leaving the capture off is the entire difference between the two modes.
    if (!s_timed) vg_capture_set(VG_CAP_STREAM);
}

bool vg_replay_next(float* dt, VgInput* in) {
    if (s_mode != VG_RP_PLAY) return false;

    char tag[4];
    // Generous: the host has to encode and write the previous frame before it
    // asks for the next one, and a slow disk should not end the session.
    if (!rd(tag, 4, 30000)) {
        s_why = "no tag: host went quiet for 30 s";
        vg_replay_command('E'); return false;
    }

    // THE END IS EXPLICIT NOW, and it has to be checked before the resync below.
    //
    // The host ended a run by sending four bytes that were not a tag, which worked while
    // any mismatch meant "stop". With the resync in place a mismatch means "a byte was
    // lost", so the sentinel was being eaten by the scan looking for the next record -- the
    // session then ended on a timeout, several seconds late, and the host had already given
    // up waiting for the answer.
    if (memcmp(tag, "EEEE", 4) == 0) {
        s_why = "host said stop";
        vg_replay_command('E'); return false;
    }

    // A BAD TAG IS A DROPPED BYTE, NOT THE END OF THE SESSION.
    //
    // This used to end the replay on the first tag that was not "PHRP", which is correct
    // when the host means to stop and wrong every other time. A full session is 5,444
    // records and 451 KB over a link that is known to corrupt -- the render tool has
    // reported a failed band twice in one day -- so one lost byte anywhere in it shifted
    // the stream by one and ended the run. Measured: 5,072 frames of 5,444, reported as
    // "the device stopped answering" when the device had in fact stopped being spoken to
    // in a language it recognised.
    //
    // So the stream RESYNCS. Slide the window one byte at a time until "PHRP" lines up
    // again; a dropped byte costs one record instead of the rest of the session. The host
    // still ends the run deliberately by sending four bytes that are not a tag, and after
    // RESYNC_MAX of them with no record found, that is what this concludes.
    //
    // The window is bounded because an unbounded scan cannot tell a corrupt stream from a
    // host that has gone away, and waiting forever is worse than stopping.
    if (memcmp(tag, "PHRP", 4) != 0) {
        const int RESYNC_MAX = 512;         // ~6 records' worth of slack
        int slid = 0;
        for (;;) {
            if (++slid > RESYNC_MAX) {
                s_why = "resync gave up after 512 bytes";
                vg_replay_command('E'); return false;
            }
            tag[0] = tag[1]; tag[1] = tag[2]; tag[2] = tag[3];
            if (!rd(&tag[3], 1, 5000)) {
                s_why = "resync ran out of stream";
                vg_replay_command('E'); return false;
            }
            if (memcmp(tag, "PHRP", 4) == 0) break;
        }
        s_resync++;
    }

    uint8_t nr = 0;
    if (!rd(dt, 4, 5000) || !rd(&nr, 1, 5000)) {
        s_why = "record header truncated";
        vg_replay_command('E');
        return false;
    }
    if (nr > RP_MAX_RAND) {
        // A seed count larger than the log can hold. The tag matched, so the stream was
        // aligned a moment ago -- this is what a corrupted byte inside a record looks like.
        s_why = "seed count out of range";
        vg_replay_command('E');
        return false;
    }
    if (nr && !rd(s_rand, (int)nr * 4, 5000)) {
        s_why = "seeds truncated";
        vg_replay_command('E'); return false;
    }
    if (!rd(in, sizeof(VgInput), 5000)) {
        s_why = "input struct truncated";
        vg_replay_command('E'); return false;
    }

    s_rand_n = nr;
    s_rand_i = 0;
    s_index++;
    return true;
}

// --- command ---------------------------------------------------------------

bool vg_replay_command(int c) {
    if (c == 'R' && s_mode == VG_RP_OFF) {
        begin_record();
        return true;
    }
    if (c == 'P' && s_mode == VG_RP_OFF) {
        s_timed = false;
        begin_play();
        return true;
    }
    // 'T', and everything after it on the wire is byte for byte what 'P' expects. The two
    // modes differ in what the DEVICE does, not in what the host sends, so a timed run
    // reuses the whole session protocol unchanged.
    if (c == 'T' && s_mode == VG_RP_OFF) {
        s_timed = true;
        begin_play();
        return true;
    }
    if (c == 'E' && s_mode != VG_RP_OFF) {
        const bool was_play = (s_mode == VG_RP_PLAY);
        s_mode = VG_RP_OFF;
        if (was_play) vg_capture_set(VG_CAP_OFF);
        vg_capture_audio_off();
        vg_link_blocking(false);    // back to never blocking the game
        uint32_t wb, ws, wt, wm;
        vg_link_stats(&wb, &ws, &wt, &wm);
        uint32_t fb, fe;
        vg_capture_frame_counts(&fb, &fe);
        if (s_resync) {
            Serial.printf("\nvg_replay: RESYNC %u -- the link dropped bytes; that many "
                          "records were skipped\n", (unsigned)s_resync);
        }
        Serial.printf("\nvg_replay: WHY %s\n", s_why);
        Serial.printf("\nvg_replay: END %u frames  wrote %u bytes  short %u  "
                      "stall %u  mismatch %u  begins %u ends %u\n",
                      (unsigned)s_index, (unsigned)wb, (unsigned)ws,
                      (unsigned)wt, (unsigned)wm, (unsigned)fb, (unsigned)fe);
        // THE COST OF THE SESSION, for a timed run. Microseconds of CPU, meaned over every
        // frame the device actually ran -- so two drawings measured this way are measured
        // over the same scene and the difference between them is the drawing.
        // One format, one place. The host may miss this -- see vg_replay_report_cost --
        // and is expected to ask for it rather than depend on catching it.
        if (s_timed) vg_replay_report_cost();
        s_timed = false;
        return true;
    }
    return false;
}
