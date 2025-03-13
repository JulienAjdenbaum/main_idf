// websocket_manager.c

#include "websocket_manager.h"
#include "esp_websocket_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

#include "audio_player.h" // for the queue-based approach

static const char *TAG = "WS_MGR";

#define WS_URI "wss://api.interaction-labs.com/esp/test"

static esp_websocket_client_handle_t s_ws_client = NULL;
static bool s_ws_connected = false;

static void websocket_event_handler(void *handler_args,
                                    esp_event_base_t base,
                                    int32_t event_id,
                                    void *event_data);

esp_err_t websocket_manager_init(void)
{
    if (s_ws_client) {
        ESP_LOGW(TAG, "WebSocket client already initialized.");
        return ESP_OK;
    }

    esp_websocket_client_config_t cfg = {
        .uri = WS_URI,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .reconnect_timeout_ms = 5000,
        .network_timeout_ms   = 10000,
        .disable_auto_reconnect = false,
        .keep_alive_enable    = true,
        .ping_interval_sec    = 30,
        .pingpong_timeout_sec = 10,
    };

    s_ws_client = esp_websocket_client_init(&cfg);
    if (!s_ws_client) {
        ESP_LOGE(TAG, "Failed to init WebSocket client");
        return ESP_FAIL;
    }

    esp_websocket_register_events(
        s_ws_client,
        WEBSOCKET_EVENT_ANY,
        websocket_event_handler,
        NULL);

    esp_err_t err = esp_websocket_client_start(s_ws_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WebSocket start failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "WebSocket started => %s", WS_URI);
    return ESP_OK;
}


static void websocket_event_handler(void *handler_args,
                                    esp_event_base_t base,
                                    int32_t event_id,
                                    void *event_data)
{
    esp_websocket_event_data_t *ws_data = (esp_websocket_event_data_t *) event_data;

    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "WebSocket connected");
        s_ws_connected = true;
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "WebSocket disconnected");
        s_ws_connected = false;
        break;

    case WEBSOCKET_EVENT_DATA:
        // We have incoming data
        if (ws_data->data_len > 0) {
            const uint8_t *rx_buf = (const uint8_t *)ws_data->data_ptr;
            uint8_t prefix = rx_buf[0];

            if (prefix == 0x02) {
                // This is an audio packet
                size_t audio_len = ws_data->data_len - 1; // skip prefix
                if (audio_len > 0) {
                    // Convert from mono -> stereo if needed
                    // (This is an example; or feed it to your "audio_stream_handle_incoming()")
                    const uint16_t *mono_samples = (const uint16_t *)(rx_buf + 1);
                    size_t num_mono_samples = audio_len / 2; // 16-bit samples
                    size_t stereo_bytes = num_mono_samples * 2 * sizeof(uint16_t);

                    // Grab an empty buffer
                    audio_buffer_t *buf = audio_player_get_buffer_blocking();
                    if (!buf) {
                        ESP_LOGE(TAG, "No free buffer for incoming audio!");
                        break;
                    }

                    if (stereo_bytes > AUDIO_BUFFER_SIZE) {
                        ESP_LOGW(TAG, "Incoming audio bigger than buffer => truncated");
                        stereo_bytes = AUDIO_BUFFER_SIZE;
                    }

                    uint16_t *out = (uint16_t *) buf->data;
                    size_t out_samples = stereo_bytes / 2; // # of 16-bit slots

                    // replicate each mono sample into left & right
                    for (size_t i = 0, j = 0; i < num_mono_samples && (j+1) < out_samples; i++, j+=2) {
                        out[j]   = mono_samples[i];
                        out[j+1] = mono_samples[i];
                    }

                    buf->length = stereo_bytes;
                    audio_player_submit_buffer(buf);
                }

            } else {
                // Probably text or other data
                ESP_LOGI(TAG, "Received text message: %.*s",
                         ws_data->data_len, (char*)ws_data->data_ptr);
            }
        }
        break;

    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGW(TAG, "WebSocket error");
        s_ws_connected = false;
        break;

    default:
        break;
    }
}

bool websocket_manager_is_connected(void)
{
    return s_ws_connected && s_ws_client &&
           esp_websocket_client_is_connected(s_ws_client);
}

int websocket_manager_send_bin(const char *data, size_t len)
{
    if (!websocket_manager_is_connected()) {
        return -1; // Not connected
    }
    // blocks until all data is sent
    int sent = esp_websocket_client_send_bin(s_ws_client, data, len, portMAX_DELAY);
    return sent;
}
