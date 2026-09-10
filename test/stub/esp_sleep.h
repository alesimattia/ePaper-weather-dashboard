#pragma once
#include <cstdint>
typedef int esp_err_t;
#define ESP_OK 0
inline void esp_sleep_enable_timer_wakeup(uint64_t) {}
inline esp_err_t esp_light_sleep_start() { return ESP_OK; }
