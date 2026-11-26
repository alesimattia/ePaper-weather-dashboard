#pragma once
/**
 * Minimo di esp_task_wdt.h per i test su host: non fa niente ma conta le
 * chiamate, così i test verificano che lo scheduler alimenti il watchdog del
 * loop prima di ogni task. Sul dispositivo il task non iscritto ignora il reset.
 */

typedef int esp_err_t;
#ifndef ESP_OK
#define ESP_OK 0
#endif
typedef void* TaskHandle_t;

inline int g_wdtResets = 0;
inline int g_wdtAdds = 0;
inline int g_wdtDeletes = 0;

inline esp_err_t esp_task_wdt_reset() { ++g_wdtResets; return ESP_OK; }
inline esp_err_t esp_task_wdt_add(TaskHandle_t) { ++g_wdtAdds; return ESP_OK; }
inline esp_err_t esp_task_wdt_delete(TaskHandle_t) { ++g_wdtDeletes; return ESP_OK; }
