#pragma once
// The task watchdog exists to turn a hang into a reboot on a device nobody can
// reach. A window on a desktop can simply be closed, so all of this is inert --
// but the calls stay so the frame loop reads the same on both.
#pragma once
#include <stdint.h>
#define ESP_OK 0
#define ESP_ERR_INVALID_STATE 0x103
typedef int esp_err_t;
typedef struct { uint32_t timeout_ms; uint32_t idle_core_mask; bool trigger_panic; }
        esp_task_wdt_config_t;
static inline esp_err_t esp_task_wdt_init(const esp_task_wdt_config_t*) { return ESP_OK; }
static inline esp_err_t esp_task_wdt_reconfigure(const esp_task_wdt_config_t*) { return ESP_OK; }
static inline esp_err_t esp_task_wdt_add(void*)   { return ESP_OK; }
static inline esp_err_t esp_task_wdt_reset(void)  { return ESP_OK; }
