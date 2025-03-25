#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Starts the HTTP server to present a Wi-Fi configuration form.
 *
 * This function spins up `esp_http_server` on port 80 and registers handlers
 * for:
 *    GET /           -> Serve a form requesting SSID and password
 *    POST /set_creds -> Parse SSID/password and call wifi_manager_set_sta_credentials()
 *    GET /generate_204 or /gen_204 -> 302 redirect to "/"
 *
 * You should call this once your SoftAP is active (e.g. inside
 * WIFI_EVENT_AP_START).
 */
void http_server_start(void);

#ifdef __cplusplus
}
#endif
