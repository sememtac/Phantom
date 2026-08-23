// Arduino.h -- the desktop stand-in.
//
// The game's sources are built UNCHANGED for the PC. Everything they expect from
// the Arduino core and from FreeRTOS is declared here instead, so the only file
// that knows it is on a PC is the port beside this one.
//
// Nothing here tries to be a real Arduino. Each symbol does the least that keeps
// the game's meaning: the clock is a real clock, the serial port is stdout, and
// the task functions REFUSE to start a task -- see the note on
// xTaskCreatePinnedToCore, which is the most load-bearing decision in this file.
#pragma once

#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

// ---------------------------------------------------------------------------
// Time. micros() is called about a hundred times a frame, so it has to be cheap
// and it has to be monotonic; QueryPerformanceCounter is both.
// ---------------------------------------------------------------------------
// C LINKAGE, like the real core's. vg_draw_arena.cpp declares micros itself as
// `extern "C" unsigned long micros(void)` rather than including anything, so a
// C++-mangled definition here would compile everywhere and link nowhere.
// Matching the Arduino signature exactly -- unsigned long, not uint32_t -- keeps
// that declaration and this one the same function on every compiler.
extern "C" {
unsigned long micros(void);
unsigned long millis(void);
void          delay(unsigned long ms);
void          delayMicroseconds(unsigned int us);
}

// The cycle counter the profiling brackets read. Scaled to the same 240 MHz the
// firmware assumes, so any number derived from it stays in the units the code
// and its comments already use.
uint32_t esp_cpu_get_cycle_count(void);

// ---------------------------------------------------------------------------
// The serial port, which on a PC is just stdout.
//
// The game writes telemetry through this constantly and the host tools speak a
// binary protocol over it. Only the telemetry half is wired up: reads always
// return nothing, so vg_capture's command poll sees an idle link and the game
// never enters a capture or a replay. That is the right default for a window
// somebody is flying -- the recording tools belong to the device.
// ---------------------------------------------------------------------------
struct HostSerial {
    void begin(unsigned long = 0) {}
    void setTxBufferSize(int) {}
    void setRxBufferSize(int) {}
    void setTxTimeoutMs(int)  {}
    int  available(void)         { return 0; }
    int  read(void)              { return -1; }
    int  availableForWrite(void) { return 0x7FFFFFFF; }
    void flush(void)             { fflush(stdout); }

    int printf(const char* f, ...) {
        va_list ap; va_start(ap, f);
        const int n = vfprintf(stdout, f, ap);
        va_end(ap);
        return n;
    }
    void print(const char* s)    { fputs(s, stdout); }
    void println(const char* s)  { fputs(s, stdout); fputc('\n', stdout); }
    void println(void)           { fputc('\n', stdout); }
    size_t write(uint8_t c)      { fputc(c, stdout); return 1; }
    size_t write(const uint8_t* p, size_t n) { return fwrite(p, 1, n, stdout); }
    size_t write(char c)         { fputc(c, stdout); return 1; }
};
extern HostSerial Serial;

// ---------------------------------------------------------------------------
// FreeRTOS, in as much as the game touches it.
//
// THE TASK FUNCTIONS ALWAYS FAIL, AND THAT IS THE DESIGN. Three places try to
// start a helper task: the band raster's second core, the input module's sensor
// task, and its input probe. Every one of them already has a single-threaded
// fallback that the firmware treats as a supported outcome -- rowsplit_start's
// own comment promises "a failed task creation costs frame rate and nothing
// else". Refusing here therefore runs the game exactly as a device with a busy
// heap would run it, through code paths that already exist and are already
// tested, instead of introducing PC threads the rest of the engine has never
// been written against.
//
// The visible consequence is that the two-core split never happens, so the
// +-1 px seam jog it produces does not either. The PC picture is the
// SPLIT_LINE_CLAMPED 0 picture. That is one of the reasons this build cannot be
// the pixel-regression instrument; see host/README.md.
// ---------------------------------------------------------------------------
#define pdPASS            1
#define pdFAIL            0
#define pdTRUE            1
#define pdFALSE           0
#define portMAX_DELAY     0xFFFFFFFFu
#define portTICK_PERIOD_MS 1
#define pdMS_TO_TICKS(ms) (ms)
#define tskNO_AFFINITY    0x7FFFFFFF

typedef void* TaskHandle_t;
typedef struct HostSem* SemaphoreHandle_t;
typedef int  BaseType_t;
typedef unsigned int UBaseType_t;
typedef uint32_t TickType_t;

SemaphoreHandle_t xSemaphoreCreateBinary(void);
SemaphoreHandle_t xSemaphoreCreateMutex(void);
BaseType_t        xSemaphoreTake(SemaphoreHandle_t s, TickType_t ticks);
BaseType_t        xSemaphoreGive(SemaphoreHandle_t s);
void              vSemaphoreDelete(SemaphoreHandle_t s);
BaseType_t        xTaskCreatePinnedToCore(void (*fn)(void*), const char* name,
                                          uint32_t stack, void* arg,
                                          UBaseType_t prio, TaskHandle_t* out,
                                          BaseType_t core);
void              vTaskDelay(TickType_t ticks);
BaseType_t        xPortInIsrContext(void);

// Which core is asking. Several per-core counter arrays are indexed by this, and
// with no second thread the answer is always 0 -- so those arrays behave exactly
// as they do on a device whose helper task failed to start.
static inline BaseType_t xPortGetCoreID(void) { return 0; }

// Stack headroom, for the telemetry line. There is no task stack to measure; a
// fixed plausible figure keeps the line readable instead of alarming.
static inline uint32_t uxTaskGetStackHighWaterMark(TaskHandle_t) { return 4096; }
