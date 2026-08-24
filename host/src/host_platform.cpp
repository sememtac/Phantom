// The bodies behind host/compat/Arduino.h.
//
// Everything the game reaches for that is not the panel, the touch surface or
// the store. Kept apart from the port so that file stays about the DEVICE the
// game thinks it is talking to, and this one stays about the runtime.
#include <Arduino.h>
#include <esp_system.h>
#include <esp_random.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <timeapi.h>   // timeBeginPeriod, so Sleep(1) is worth asking for

HostSerial Serial;

// ---------------------------------------------------------------------------
// The clock.
//
// One QueryPerformanceCounter baseline taken on first use, so micros() starts
// near zero and the 32-bit wrap lands about 71 minutes in -- the same wrap the
// firmware's own micros() has, which several counters in the game are written to
// tolerate. Matching the wrap is free and means those paths are exercised here
// exactly as they are on the board.
// ---------------------------------------------------------------------------
static LARGE_INTEGER s_freq;
static LARGE_INTEGER s_base;

static void clock_init(void) {
    if (s_freq.QuadPart) return;
    QueryPerformanceFrequency(&s_freq);
    QueryPerformanceCounter(&s_base);
    timeBeginPeriod(1);   // so Sleep(1) is worth asking for
}

static inline uint64_t ticks_since_base(void) {
    clock_init();
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (uint64_t)(now.QuadPart - s_base.QuadPart);
}

unsigned long micros(void) {
    return (unsigned long)((ticks_since_base() * 1000000ull) / (uint64_t)s_freq.QuadPart);
}

unsigned long millis(void) {
    return (unsigned long)((ticks_since_base() * 1000ull) / (uint64_t)s_freq.QuadPart);
}

void delay(unsigned long ms) { clock_init(); Sleep(ms); }

void delayMicroseconds(unsigned int us) {
    const uint32_t t0 = micros();
    while ((uint32_t)(micros() - t0) < us) { /* spin: only ever short waits */ }
}

// Scaled to the 240 MHz the firmware's brackets assume, so any figure derived
// from this stays in the units the code and its comments already use.
uint32_t esp_cpu_get_cycle_count(void) {
    return (uint32_t)((ticks_since_base() * 240000000ull) / (uint64_t)s_freq.QuadPart);
}

// ---------------------------------------------------------------------------
// FreeRTOS.
//
// The semaphores are real enough to be created and destroyed, because
// rowsplit_start makes a pair BEFORE it tries to start its task and deletes them
// again when that fails. They are never actually waited on: nothing ever runs on
// a second thread here. See the note in Arduino.h for why refusing tasks is the
// design rather than a shortcut.
// ---------------------------------------------------------------------------
struct HostSem { int count; };

SemaphoreHandle_t xSemaphoreCreateBinary(void) {
    HostSem* s = (HostSem*)calloc(1, sizeof(HostSem));
    return s;
}
SemaphoreHandle_t xSemaphoreCreateMutex(void) {
    HostSem* s = (HostSem*)calloc(1, sizeof(HostSem));
    if (s) s->count = 1;
    return s;
}
BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t) {
    if (!s) return pdFAIL;
    if (s->count > 0) { s->count--; return pdPASS; }
    // Nothing can ever give it -- there is no other thread. Failing is the
    // honest answer and every caller here treats a refusal as "carry on".
    return pdFAIL;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t s) {
    if (!s) return pdFAIL;
    s->count = 1;
    return pdPASS;
}
void vSemaphoreDelete(SemaphoreHandle_t s) { free(s); }

BaseType_t xTaskCreatePinnedToCore(void (*)(void*), const char*, uint32_t, void*,
                                   UBaseType_t, TaskHandle_t* out, BaseType_t) {
    if (out) *out = nullptr;
    return pdFAIL;      // deliberate -- see Arduino.h
}

void       vTaskDelay(TickType_t ticks) { Sleep(ticks); }
BaseType_t xPortInIsrContext(void)      { return pdFALSE; }

// ---------------------------------------------------------------------------
// The rest of the system.
// ---------------------------------------------------------------------------
// A FIXED SEED, WHEN ONE IS ASKED FOR.
//
// Zero means take the clock, which is what a session wants: two runs of the game
// should not be the same game. Anything else makes the run REPRODUCIBLE, and
// that is not a nicety -- without it two headless runs of the same build differ,
// so every A against B comparison is one sample of A against one sample of B and
// says nothing. Several were made that way before anybody checked.
static uint32_t s_seed_request = 0;

void host_random_seed(uint32_t seed) { s_seed_request = seed; }

uint32_t esp_random(void) {
    // Not the hardware RNG, and it does not need to be: a replay carries its own
    // numbers, so this only ever seeds a fresh session.
    static uint32_t s = 0;
    if (!s) s = s_seed_request ? (s_seed_request | 1u)
                               : (uint32_t)(ticks_since_base() | 1ull);
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    return s;
}

esp_reset_reason_t esp_reset_reason(void) {
    // A process that has just started was, in every sense that matters here,
    // powered on.
    return ESP_RST_POWERON;
}

void esp_restart(void) { ExitProcess(0); }
