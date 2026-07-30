#include "vg_crumb.h"
#include <Arduino.h>
#include <esp_attr.h>
#include <esp_system.h>

// RTC_NOINIT_ATTR is the whole trick: RTC slow memory, and the startup code is
// told NOT to zero it. It survives a panic, a watchdog, a brownout and a
// software reset. It does not survive power being removed, which is why the
// magic exists -- on a cold boot these words are whatever the RAM settled on.
#define CRUMB_MAGIC 0x50484D32u   // 'PHM2'

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
        // Power was removed, so both the live crumb and any stored crash record
        // are gone. Start clean rather than reporting noise as a crash.
        s_crash_valid = 0;
        Serial.println("crumb: cold start");
    } else if (!clean) {
        // The run that just ended died. Promote the live crumb to the record.
        s_crash_valid  = 1;
        s_crash_reason = (uint32_t)r;
        s_crash_frame  = s_frame;
        s_crash_where  = s_where;
        s_crash_state  = s_state;
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

    s_magic = CRUMB_MAGIC;
    s_frame = 0;
    s_where = CRUMB_BOOT;
    s_state = 0;
}
