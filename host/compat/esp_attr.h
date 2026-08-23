// The placement attributes, which mean nothing on a PC.
//
// IRAM_ATTR and DRAM_ATTR pin code and data into internal RAM so the flash cache
// cannot stall them. A PC has no such distinction, so they vanish.
//
// RTC_NOINIT_ATTR is the interesting one. On the device it marks memory the boot
// code must NOT clear, so a crash record survives into the next boot. A PC
// process that ends takes its memory with it, so these become ordinary globals
// and start zeroed -- which the crumb module reads as a cold boot, correctly:
// nothing did survive.
#pragma once
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
