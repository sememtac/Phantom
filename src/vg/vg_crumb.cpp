#include "vg_crumb.h"
#include "vg_port.h"
#include "vg_capture.h"   // vg_link_busy: no flash writes into a live stream
#include <Arduino.h>
#include <esp_attr.h>
#include <esp_system.h>
#include <stdio.h>

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
// EVERY BOOT'S REASON, not just the ones that count as crashes.
//
// A restart whose reason is POWERON, SW, EXT or USB is treated as clean and writes no
// record at all, so a board that is quietly restarting every twenty seconds looks exactly
// like a board that has been up the whole time -- the only trace is the frame counter
// starting over, and nothing prints that unless someone asks.
//
// It cannot be caught by watching the port either: the restarts only happen when NOTHING
// is reading, and attaching a reader is what stops them. So the device remembers instead.
// Eight reasons, four bits each, newest in the low nibble.
static RTC_NOINIT_ATTR uint32_t s_reason_hist;

static RTC_NOINIT_ATTR uint32_t s_stall_ms;
static RTC_NOINIT_ATTR uint32_t s_stall_state;
static RTC_NOINIT_ATTR uint32_t s_stall_where;

// The same record in flash, which power does not clear.
struct CrashRec {
    uint32_t magic, reason, frame, where, state, stall_ms, stall_state, stall_where;
};

static const char* const CRUMB_NAME[CRUMB_SLOTS] = {
    "boot", "poll", "input", "update", "render", "flush",
    "flush-wait", "flush-draw", "flush-scan", "flush-push",
    "flush-end", "tail", "telem"
};

// Kept next to the enum in vg_game.h. Duplicated on purpose: this file must not
// pull in the game to report on it, or a crash in the game's own headers takes
// the crash reporter with it.
static const char* const STATE_NAME[] = {
    "ATTRACT", "ENTRY", "SELECT", "REPAIR", "BRACKET", "INTRO", "PLAYING",
    "HIT", "KILL", "PAUSE", "COURSE", "ROUND_WON", "OVER", "WON"
};
#define STATE_COUNT ((int)(sizeof(STATE_NAME) / sizeof(STATE_NAME[0])))

static const char* state_name(uint32_t s);

// THE SECOND BYTE MEANS TWO DIFFERENT THINGS, and printing it as one is what made
// two crash records unreadable.
//
// vg_crumb.h has always said so -- "the state byte carries the band index for these,
// not the state" -- and the writers have always honoured it. Only the report did not:
// it sent the byte through state_name() whatever the phase, and STATE_NAME has exactly
// fourteen entries, ATTRACT..WON. So a crash while pushing band 14 of 15 printed
// "state ?(14)", which reads as a corrupt state and is nothing of the kind.
//
// It cost more than one confusing line. The record kept from an earlier playtest --
// "died in flush-push, state ?(14), frame 38137" -- was read as an unknown state and
// filed as a curiosity. It was band 14. A second crash then landed on band 14 as well,
// and TWO crashes agreeing on the last band's push is a real clue that was sitting in
// plain sight behind a wrong label.
//
// Nothing about what is STORED changes here, so records already in RTC and in flash
// start reading correctly the moment this ships.
//
// FWAIT is the exception among the flush phases: it waits on the previous transfer and
// is not per-band, so its writer passes a literal 0 and there is nothing to name.
static void detail_str(char* buf, size_t n, uint32_t where, uint32_t val) {
    switch (where) {
        case CRUMB_FDRAW:
        case CRUMB_FSCAN:
        case CRUMB_FPUSH:
            snprintf(buf, n, ", band %u", (unsigned)val);
            break;
        case CRUMB_FWAIT:
        case CRUMB_FEND:
        case CRUMB_BOOT:
            buf[0] = '\0';                     // neither, and 0 is not ATTRACT
            break;
        default:
            snprintf(buf, n, ", state %s(%u)", state_name(val), (unsigned)val);
            break;
    }
}

static const char* where_name(uint32_t w) {
    return (w < CRUMB_SLOTS) ? CRUMB_NAME[w] : "?";
}
static const char* state_name(uint32_t s) {
    return ((int)s < STATE_COUNT) ? STATE_NAME[s] : "?";
}

static void crash_store(void) {
    CrashRec r = { CRUMB_MAGIC, s_crash_reason, s_crash_frame, s_crash_where,
                   s_crash_state, s_stall_ms, s_stall_state, s_stall_where };
    vg_store_diag_save(&r, sizeof(r));
}

static bool crash_recall(void) {
    CrashRec r;
    if (!vg_store_diag_load(&r, sizeof(r))) return false;
    if (r.magic != CRUMB_MAGIC) return false;
    s_crash_reason = r.reason;   s_crash_frame = r.frame;
    s_crash_where  = r.where;    s_crash_state = r.state;
    s_stall_ms     = r.stall_ms; s_stall_state = r.stall_state;
    s_stall_where  = r.stall_where;
    return true;
}

void vg_crumb_stall(uint32_t ms, uint8_t state, uint8_t where) {
    // NOT WHILE THE LINK IS CARRYING FRAMES, for two separate reasons.
    //
    // The write below is an NVS commit. A flash write disables the instruction
    // cache while it runs, which is long enough to corrupt a pixel stream the
    // host is reading -- it comes back as a palette index past the end of the
    // table, several hundred frames into a render, with nothing to point at.
    // tools/README.md claims every write to flash is blocked during a render;
    // vg_save_store honours that, and this path never did.
    //
    // And a stall during a render is not a stall worth recording. The host paces
    // the device, a venue sky costs 200ms to generate, and the reading that
    // results describes the capture rather than the game. The worst-frame record
    // was once left holding thirty seconds from a render that was waiting on the
    // host, which suppressed every real stall until somebody cleared it.
    if (vg_link_busy()) return;
    if (ms <= s_stall_ms) return;
    s_stall_ms    = ms;
    s_stall_state = state;
    s_stall_where = where;
    // Written through immediately: a hang deep enough to matter may never get
    // another chance to write anything, and a flash write costs nothing on a
    // frame that has already lost a quarter of a second.
    crash_store();
}

void vg_crumb_reset(void) {
    s_crash_valid = 0; s_crash_reason = 0; s_crash_frame = 0;
    s_crash_where = 0; s_crash_state  = 0;
    s_stall_ms    = 0; s_stall_state  = 0; s_stall_where = 0;
    crash_store();
}

void vg_crumb(uint8_t where, uint8_t detail) {
    s_magic = CRUMB_MAGIC;
    s_where = where;
    s_state = detail;
    if (where == CRUMB_POLL) s_frame++;
}

void vg_crumb_report(void) {
    const esp_reset_reason_t r = esp_reset_reason();

    //   1 power-on   3 software   4 PANIC   5 int watchdog   6 task watchdog
    //   7 watchdog   9 brownout  11 USB, which is our own reset pulse
    Serial.printf("reset reason: %d\n", (int)r);

    const bool cold  = (s_magic != CRUMB_MAGIC);
    // Pushed BEFORE anything can return, so the history is complete even on a cold boot.
    if (cold) s_reason_hist = 0;
    s_reason_hist = (s_reason_hist << 4) | ((uint32_t)r & 0xFu);
    {
        char h[64];
        int  at = 0;
        for (int i = 0; i < 8 && at < (int)sizeof(h) - 4; i++) {
            const uint32_t v = (s_reason_hist >> (i * 4)) & 0xFu;
            if (!v) break;                    // 0 is not a reason; nothing older
            at += snprintf(h + at, sizeof(h) - at, "%lu ", (unsigned long)v);
        }
        h[at > 0 ? at - 1 : 0] = 0;
        Serial.printf("crumb: recent resets (newest first): %s\n", h);
    }
    const bool clean = (r == ESP_RST_POWERON || r == ESP_RST_USB
                        || r == ESP_RST_SW || r == ESP_RST_EXT);

    if (cold) {
        // Power was removed, so RTC is gone -- but flash is not, and whatever the
        // last run managed to record is still sitting there. That is the whole
        // reason it is written to flash at all.
        s_crash_valid = 0;
        s_stall_ms    = 0;
        s_stall_state = 0;
        s_stall_where = 0;
        if (crash_recall()) {
            // A STORED RECORD IS NOT A STORED CRASH. vg_crumb_reset writes an
            // all-zero record deliberately, and reading that back as valid
            // reported "LAST CRASH reason 0, died in boot, frame 0" -- a crash
            // that never happened, in a place nothing runs, which is exactly the
            // sort of thing that costs an hour later.
            s_crash_valid = (s_crash_reason != 0) ? 1u : 0u;
            Serial.println(s_crash_valid ? "crumb: cold start, crash recovered from flash"
                                         : "crumb: cold start, flash record is clear");
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
        char det[40];
        detail_str(det, sizeof(det), s_crash_where, s_crash_state);
        Serial.printf("CRUMB: LAST CRASH reason %lu, died in %s%s, frame %lu\n",
                      (unsigned long)s_crash_reason,
                      where_name(s_crash_where), det,
                      (unsigned long)s_crash_frame);
    } else if (!cold) {
        Serial.printf("crumb: no crash on record (last run reached frame %lu)\n",
                      (unsigned long)s_frame);
    }

    // Reported separately and ALWAYS, because a freeze is not a crash: nothing
    // resets, so everything above stays silent about it while the player watches
    // a dead screen and reasonably calls it a crash.
    // NOT routed through detail_str, and that is not an oversight. The stall's writer
    // passes vg.state explicitly (see main.cpp), so this byte is always a state even when
    // the phase beside it is a flush sub-phase. Only the CRASH record borrows the field.
    if (s_stall_ms > 0)
        Serial.printf("CRUMB: worst frame %lu ms, in state %s, phase %s\n",
                      (unsigned long)s_stall_ms, state_name(s_stall_state),
                      where_name(s_stall_where));

    s_magic = CRUMB_MAGIC;
    s_frame = 0;
    s_where = CRUMB_BOOT;
    s_state = 0;
}
