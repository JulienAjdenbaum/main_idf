#ifndef WEBSOCKET_MANAGER_H
#define WEBSOCKET_MANAGER_H

#include <stddef.h>
#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize and start the WebSocket client.
 */
esp_err_t websocket_manager_init(void);

/**
 * @brief Check if the WebSocket is currently connected.
 */
bool websocket_manager_is_connected(void);

/**
 * @brief Send binary data if connected. Returns the number of bytes sent on success, negative on error.
 */
int websocket_manager_send_bin(const char *data, size_t len);
void websocket_manager_send_rfid_event(const uint8_t *uid, size_t uid_len, bool tag_removed);

#ifdef __cplusplus
}
#endif

#endif // WEBSOCKET_MANAGER_H
