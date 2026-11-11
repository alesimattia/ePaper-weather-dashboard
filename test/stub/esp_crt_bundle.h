#pragma once
// Tuya.h include questo header per nome, come nel core: deve esistere, ma i
// tipi stanno in esp_tls.h. Il bundle CA non serve su host: la verifica del
// certificato non si testa.
#include "esp_tls.h"

inline esp_err_t esp_crt_bundle_attach(void*) { return ESP_OK; }
