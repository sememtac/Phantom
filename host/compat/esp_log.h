#pragma once
#include <stdarg.h>
// The IDF log system. Only one thing in the game touches it: vg_capture installs
// a sink to silence the I2S driver's timeout spam during a capture. With no I2S
// driver here there is nothing to silence, so installing a sink is a no-op that
// still returns something the caller can put back.
typedef int (*vprintf_like_t)(const char*, va_list);
static inline vprintf_like_t esp_log_set_vprintf(vprintf_like_t) { return 0; }
