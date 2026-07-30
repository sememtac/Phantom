#include "vg_crumb.h"
#include "vg_port.h"
#include <Arduino.h>
#include <esp_attr.h>
#include <esp_system.h>

// RTC_NOINIT_ATTR is the whole trick: RTC slow memory, and the startup code is
// told NOT to zero it. It survives a panic, a watchdog, a brownout and a
// software reset. It does not survive power being removed, which is why the
// magic exists -- on a cold boot these words are whatever the RAM settled on.
//
// ...and not surviving power is exactly how the first real report got lost. A
// player whose game locks up unplugs it, and unplugging it erases the reason.
// So the record is mirrored into flash as well; see crash_store.
#define CRUMB_MAGIC 0x50484D33u   // 'PHM3'

// Where the frame is right now. Overwritten constantly.
static RTC_NOINIT_ATTR uint32_t s_magic;
static RTC_NOINIT_ATTR uint32_t s_frame;
static RTC_NOINIT_ATTR uint32_t s_where;
static RTC_NOINIT_ATTR uint32_t s_state;

// Where the frame was when it last DIED. Written once, at the boot that follows
// a crash, and then left alone.
//
// The first version of this cleared the live crumb at boot and printed it once.
// That is useless in practice: it requires somebody to be watching the port at
// the exact moment the board reboots, which is precisely what nobody is doing
// when they are playing the game. The crash record now survives until the next
// crash overwrites it or the board loses power, so it can be collected whenever
// -- including by a host that connects several boots later.
static RTC_NOINIT_ATTR uint32_t s_crash_valid;
static RTC_NOINIT_ATTR uint32_t s_crash_reason;
static RTC_NOINIT_ATTR uint32_t s_crash_frame;
static RTC_NOINIT_ATTR uint32_t s_crash_where;
static RTC_NOINIT_ATTR uint32_t s_crash_state;

// The worst frame anyone has seen, and where. A FREEZE IS NOT A CRASH: nothing
// resets, so the reset reason and the breadcrumb both stay silent while the
// player watches a dead screen and quite reasonably calls it a crash. This is
// the only thing that would catch one.
static RTC_NOINIT_ATTR uint32_t s_stall_ms;
static RTC_NOINIT_ATTR uint32_t s_stall_state;

// The same record in flash, which power does not clear.
struct CrashRec {
    uint32_t magic, reason, frame, where, state, stall_ms, stall_state;
};

static const char* const CRUMB_NAME[CRUMB_SLOTS] = {
    "boot", "poll", "input", "update", "render", "flush"
};

// Kept next to the enum in vg_game.h. Duplicated on purpose: this file must not
// pull in the game to report on it, or a crash in the game's own headers takes
// the crash reporter with it.
static const char* const STATE_NAME[] = {
    "ATTRACT", "ENTRY", "SELECT", "REPAIR", "BRACKET", "INTRO", "PLAYING",
    "HIT", "KILL", "PAUSE", "COURSE", "ROUND_WON", "OVER", "WON"
};
#define STATE_COUNT ((int)(sizeof(STATE_NAME) / sizeof(STATE_NAME[0])))

static const char* where_name(uint32_t w) {
    return (w < CRUMB_SLOTS) ? CRUMB_NAME[w] : "?";
}
static const char* state_name(uint32_t s) {
    return ((int)s < STATE_COUNT) ? STATE_NAME[s] : "?";
}

static void crash_store(void) {
    CrashRec r = { CRUMB_MAGIC, s_crash_reason, s_crash_frame, s_crash_where,
                   s_crash_state, s_stall_ms, s_stall_state };
    vg_store_diag_save(&r, sizeof(r));
}

static bool crash_recall(void) {
    CrashRec r;
    if (!vg_store_diag_load(&r, sizeof(r))) return false;
    if (r.magic != CRUMB_MAGIC) return false;
    s_crash_reason = r.reason;   s_crash_frame = r.frame;
    s_crash_where  = r.where;    s_crash_state = r.state;
    s_stall_ms     = r.stall_ms; s_stall_state = r.stall_state;
    return true;
}

void vg_crumb_stall(uint32_t ms, uint8_t state) {
    if (ms <= s_stall_ms) return;
    s_stall_ms    = ms;
    s_stall_state = state;
    // Written through immediately: a hang deep enough to matter may never get
    // another chance to write anything, and a flash write costs nothing on a
    // frame that has already lost a quarter of a second.
    crash_store();
}

void vg_crumb_reset(void) {
    s_crash_valid = 0; s_crash_reason = 0; s_crash_frame = 0;
    s_crash_where = 0; s_crash_state  = 0;
    s_stall_ms    = 0; s_stall_state  = 0;
    crash_store();
}

void vg_crumb(uint8_t where, uint8_t state) {
    s_magic = CRUMB_MAGIC;
    s_where = where;
    s_state = state;
    if (where == CRUMB_POLL) s_frame++;
}

void vg_crumb_report(void) {
    const esp_reset_reason_t r = esp_reset_reason();

    //   1 power-on   3 software   4 PANIC   5 int watchdog   6 task watchdog
    //   7 watchdog   9 brownout  11 USB, which is our own reset pulse
    Serial.printf("reset reason: %d\n", (int)r);

    const bool cold  = (s_magic != CRUMB_MAGIC);
    const bool clean = (r == ESP_RST_POWERON || r == ESP_RST_USB
                        || r == ESP_RST_SW || r == ESP_RST_EXT);

    if (cold) {
        // Power was removed, so RTC is gone -- but flash is not, and whatever the
        // last run managed to record is still sitting there. That is the whole
        // reason it is written to flash at all.
        s_crash_valid = 0;
        s_stall_ms    = 0;
        s_stall_state = 0;
        if (crash_recall()) {
            s_crash_valid = 1;
            Serial.println("crumb: cold start, recovered from flash");
        } else {
            Serial.println("crumb: cold start");
        }
    } else if (!clean) {
        // The run that just ended died. Promote the live crumb to the record.
        s_crash_valid  = 1;
        s_crash_reason = (uint32_t)r;
        s_crash_frame  = s_frame;
        s_crash_where  = s_where;
        s_crash_state  = s_state;
        crash_store();
    }

    if (s_crash_valid) {
        // The line that matters. Everything else in this file exists to print it.
        Serial.printf("CRUMB: LAST CRASH reason %lu, died in %s, state %s, frame %lu\n",
                      (unsigned long)s_crash_reason,
                      where_name(s_crash_where),
                      state_name(s_crash_state),
                      (unsigned long)s_crash_frame);
    } else if (!cold) {
        Serial.printf("crumb: no crash on record (last run reached frame %lu)\n",
                      (unsigned long)s_frame);
    }

    // Reported separately and ALWAYS, because a freeze is not a crash: nothing
    // resets, so everything above stays silent about it while the player watches
    // a dead screen and reasonably calls it a crash.
    if (s_stall_ms > 0)
        Serial.printf("CRUMB: worst frame %lu ms, in state %s\n",
                      (unsigned long)s_stall_ms, state_name(s_stall_state));

    s_magic = CRUMB_MAGIC;
    s_frame = 0;
    s_where = CRUMB_BOOT;
    s_state = 0;
}
