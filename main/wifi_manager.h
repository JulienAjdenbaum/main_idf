// wifi_manager.h
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Initialize Wi-Fi and connect to the configured SSID/PASS
esp_err_t wifi_manager_init(void);

#ifdef __cplusplus
}
#endif
