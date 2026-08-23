#pragma once
#include <stdint.h>
// The external-memory cache counters. These are real hardware registers on the
// S3 and the replay report reads them to split instruction misses from data
// misses. A PC has caches too, but not these, and not ones a program may read
// -- so every counter reads zero and the CACHE line in the report is empty
// rather than wrong.
#define EXTMEM_IBUS_ACS_CNT_REG               0
#define EXTMEM_IBUS_ACS_MISS_CNT_REG          0
#define EXTMEM_DBUS_ACS_CNT_REG               0
#define EXTMEM_DBUS_ACS_FLASH_MISS_CNT_REG    0
#define EXTMEM_DBUS_ACS_SPIRAM_MISS_CNT_REG   0
#define EXTMEM_DBUS_TO_FLASH_START_VADDR_REG  0
#define EXTMEM_DBUS_TO_FLASH_END_VADDR_REG    0
static inline uint32_t REG_READ(uint32_t)            { return 0; }
static inline void     REG_WRITE(uint32_t, uint32_t) {}
