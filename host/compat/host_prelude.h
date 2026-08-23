// Force-included into every translation unit of the desktop build.
//
// It exists for one reason: GCC's __attribute__ syntax, which MSVC does not
// have. The game uses it in exactly two forms, noinline and always_inline, and
// both are inlining HINTS -- dropping them changes how the compiler chooses to
// lay code out and nothing about what the code means.
//
// Checked before writing this: there is no __attribute__((aligned)) anywhere in
// the sources. That one could not be dropped, because the raster relies on band
// buffers being 16-byte aligned and losing it would be a silent correctness bug
// rather than a slower frame. Alignment here is spelled with C++11 alignas,
// which MSVC supports natively.
#pragma once

#if defined(_MSC_VER)
#define __attribute__(x)
#endif

// The placement attributes, here as well as in esp_attr.h. On the device they
// arrive through whichever ESP header a file happens to pull in first; several
// sources use IRAM_ATTR without including anything that obviously provides it.
// Defining them here means include order cannot decide whether a file compiles.
// The definitions are identical to esp_attr.h's, which is what makes repeating
// them legal rather than a clash.
#ifndef IRAM_ATTR
#define IRAM_ATTR
#endif
#ifndef DRAM_ATTR
#define DRAM_ATTR
#endif
#ifndef RTC_NOINIT_ATTR
#define RTC_NOINIT_ATTR
#endif
#ifndef RTC_DATA_ATTR
#define RTC_DATA_ATTR
#endif
#ifndef ARDUINO_ISR_ATTR
#define ARDUINO_ISR_ATTR
#endif

#if defined(_MSC_VER)
#include <intrin.h>          // _ReadWriteBarrier, for the one compiler fence
#include <esp_system.h>      // esp_restart, reached from files that include no esp header

// GCC's atomic builtins, which MSVC does not have.
//
// SAFE TO MAKE THESE PLAIN ACCESSES, and only because of the decision in
// Arduino.h: no task ever starts, so there is no second thread and nothing to
// synchronise with. The seqlock in vg_input.cpp that uses them is publishing
// between the sensor task and the render thread -- and here they are the same
// thread, so the sequence counter simply never disagrees with itself.
//
// If a future desktop build ever does start a real thread, these must become
// std::atomic operations before that happens. Nothing else in this shim would
// notice; this is the one place that would be wrong.
#define __ATOMIC_RELAXED 0
#define __ATOMIC_CONSUME 1
#define __ATOMIC_ACQUIRE 2
#define __ATOMIC_RELEASE 3
#define __ATOMIC_ACQ_REL 4
#define __ATOMIC_SEQ_CST 5
#define __atomic_load_n(p, m)       (*(p))
#define __atomic_store_n(p, v, m)   ((void)(*(p) = (v)))
#define __atomic_thread_fence(m)    _ReadWriteBarrier()
#endif
