#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize Wi-Fi in station mode and connect.
 */
esp_err_t wifi_manager_init(void);

#ifdef __cplusplus
}
#endif

#endif // WIFI_MANAGER_H
