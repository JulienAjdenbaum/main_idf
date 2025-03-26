// OTA.h
#ifndef OTA_H
#define OTA_H

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#define OTA_HARDWARE_VERSION  "HW_v2.0"
#define OTA_SOFTWARE_VERSION  "FW_v2.0.1"

esp_err_t ota_init(void);
void ota_send_device_version(void);

/**
 * @brief Start the OTA update process with the given firmware URL.
 *        This function spawns a dedicated task which:
 *          - waits for audio to finish
 *          - downloads the bin
 *          - writes to OTA partition
 *          - reboots
 */
void ota_start_update(const char *url);

#endif // OTA_H
