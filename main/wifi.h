// wifi.h
#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Bits used to signal Wi-Fi success/fail in the event group
#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1

// Extern so we only define it once in wifi.c
extern EventGroupHandle_t s_wifi_event_group;

/**
 * @brief Initialize Wi-Fi in station mode; fallback to AP if STA fails.
 *        Reads stored credentials from NVS if available.
 *
 * @return esp_err_t
 */
esp_err_t wifi_manager_init(void);

/**
 * @brief Store new credentials (ssid, password) in NVS, then re-init STA mode.
 *
 * @param ssid Null-terminated SSID string
 * @param pass Null-terminated Password string
 * @return esp_err_t
 */
esp_err_t wifi_manager_set_sta_credentials(const char *ssid, const char *pass);

#ifdef __cplusplus
}
#endif
