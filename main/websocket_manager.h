#ifndef WEBSOCKET_MANAGER_H
#define WEBSOCKET_MANAGER_H

#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize and start the WebSocket client (and related tasks).
 */
esp_err_t websocket_manager_init(void);

/**
 * @brief Stop and clean up the WebSocket client and audio ring buffer.
 *        Frees associated memory. Can be restarted later with websocket_manager_init().
 */
esp_err_t websocket_manager_stop(void);

/**
 * @brief Request a graceful shutdown so tasks can exit their loops.
 */
void websocket_manager_request_shutdown(void);

/**
 * @brief Check if a shutdown has been requested.
 */
bool websocket_manager_is_shutdown_requested(void);

/**
 * @brief Check if the WebSocket is currently connected.
 */
bool websocket_manager_is_connected(void);

/**
 * @brief Send binary data if connected. Returns the number of bytes sent on success, negative on error.
 */
int websocket_manager_send_bin(const char *data, size_t len);

/**
 * @brief Send an RFID event (insertion/removal) via WebSocket.
 */
void websocket_manager_send_rfid_event(const uint8_t *uid, size_t uid_len, bool tag_removed);

#ifdef __cplusplus
}
#endif

#endif // WEBSOCKET_MANAGER_H
