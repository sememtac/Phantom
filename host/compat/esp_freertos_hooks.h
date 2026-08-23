#pragma once
#include <stdint.h>
#define ESP_OK 0
// The idle hook counts how often a core had nothing to do, which is how the
// telemetry measures core 0's spare time. There is no idle task here, so the
// counter simply never advances and the i0 figure reads zero -- honest, since
// this build has no second core to be idle.
static inline int esp_register_freertos_idle_hook_for_cpu(bool (*)(void), uint32_t) {
    return ESP_OK;
}
