#include "vg_crumb.h"
#include <Arduino.h>
#include <esp_attr.h>
#include <esp_system.h>

// RTC_NOINIT_ATTR is the whole trick: RTC slow memory, and the startup code is
// told NOT to zero it. It survives a panic, a watchdog, a brownout and a
// software reset. It does not survive power being removed, which is why the
// magic exists -- on a cold boot these four words are whatever the RAM happened
// to settle on.
#define CRUMB_MAGIC 0x50484D31u   // 'PHM1'

static RTC_NOINIT_ATTR uint32_t s_magic;
static RTC_NOINIT_ATTR uint32_t s_frame;
static RTC_NOINIT_ATTR uint32_t s_where;
static RTC_NOINIT_ATTR uint32_t s_state;

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

void vg_crumb(uint8_t where, uint8_t state) {
    s_magic = CRUMB_MAGIC;
    s_where = where;
    s_state = state;
    if (where == CRUMB_POLL) s_frame++;
}

void vg_crumb_report(void) {
    const esp_reset_reason_t r = esp_reset_reason();

    //   1 power-on   3 software   4 PANIC   5 int watchdog   6 task watchdog
    //   7 watchdog   9 brownout  14 USB, which is our own reset pulse
    Serial.printf("reset reason: %d\n", (int)r);

    const bool clean = (r == ESP_RST_POWERON || r == ESP_RST_USB
                        || r == ESP_RST_SW);

    if (s_magic != CRUMB_MAGIC) {
        Serial.println("crumb: cold start, nothing to report");
    } else if (clean) {
        Serial.printf("crumb: last run reached frame %lu, ended cleanly\n",
                      (unsigned long)s_frame);
    } else {
        const char* w = (s_where < CRUMB_SLOTS) ? CRUMB_NAME[s_where] : "?";
        const char* g = ((int)s_state < STATE_COUNT) ? STATE_NAME[s_state] : "?";
        // The line that matters. Everything else in this file exists to print it.
        Serial.printf("CRUMB: DIED IN %s, state %s, frame %lu\n",
                      w, g, (unsigned long)s_frame);
    }

    s_magic = CRUMB_MAGIC;
    s_frame = 0;
    s_where = CRUMB_BOOT;
    s_state = 0;
}
