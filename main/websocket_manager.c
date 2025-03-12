// websocket_manager.c

#include "websocket_manager.h"
#include "esp_websocket_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

#include "audio_stream.h"  // We'll call handle_incoming_audio() here

static const char *TAG = "WS_MGR";

#define WS_URI "wss://api.interaction-labs.com/esp/test"

// Globals
static esp_websocket_client_handle_t s_ws_client = NULL;
static bool s_ws_connected = false;
static int s_message_counter = 0;

// Forward declarations
static void websocket_event_handler(void *handler_args, esp_event_base_t base,
                                    int32_t event_id, void *event_data);
static void websocket_test_task(void *arg);

esp_err_t websocket_manager_init(void)
{
    // Configure the client
    esp_websocket_client_config_t cfg = {
        .uri = WS_URI,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .reconnect_timeout_ms = 5000,
        .network_timeout_ms = 10000,
        .disable_auto_reconnect = false,
        .keep_alive_enable = true,
        .ping_interval_sec = 30,
        .pingpong_timeout_sec = 10,
    };

    // Create the client
    s_ws_client = esp_websocket_client_init(&cfg);
    if (!s_ws_client) {
        ESP_LOGE(TAG, "Failed to init WebSocket client");
        return ESP_FAIL;
    }

    // Register events
    esp_websocket_register_events(
        s_ws_client,
        WEBSOCKET_EVENT_ANY,
        websocket_event_handler,
        NULL
    );

    // Start the client
    esp_err_t err = esp_websocket_client_start(s_ws_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WebSocket start failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "WebSocket started => %s", WS_URI);

    // Create the test task that sends text frames
    xTaskCreate(websocket_test_task, "ws_test_task", 4096, NULL, 5, NULL);
    return ESP_OK;
}

// The event handler
static void websocket_event_handler(void *handler_args, esp_event_base_t base,
                                    int32_t event_id, void *event_data)
{
    esp_websocket_event_data_t *ws_data = (esp_websocket_event_data_t *) event_data;

    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "WebSocket connected");
        s_ws_connected = true;
        audio_stream_reset_wav_header();  // Let the audio module reset flags
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "WebSocket disconnected");
        s_ws_connected = false;
        break;

    case WEBSOCKET_EVENT_DATA: {
        if (ws_data->data_len > 0) {
            const uint8_t *rx_buf = (const uint8_t *)ws_data->data_ptr;
            uint8_t prefix = rx_buf[0];

            if (prefix == 0x02) {
                size_t audio_len = ws_data->data_len - 1;
                if (audio_len > 0) {
                    // Copy chunk minus prefix
                    uint8_t *chunk = malloc(audio_len);
                    if (!chunk) {
                        ESP_LOGE(TAG, "malloc failed for audio chunk");
                        return;
                    }
                    memcpy(chunk, rx_buf + 1, audio_len);

                    // Pass to the audio module
                    audio_stream_handle_incoming(chunk, audio_len);
                    free(chunk);
                }
            } 
            else if (prefix == 0x01) {
                // text frame
                char text_msg[128];
                size_t text_len = ws_data->data_len - 1;
                size_t cpy_len = (text_len < sizeof(text_msg) - 1)
                                 ? text_len
                                 : (sizeof(text_msg) - 1);
                memcpy(text_msg, rx_buf + 1, cpy_len);
                text_msg[cpy_len] = '\0';
                ESP_LOGI(TAG, "Received TEXT => %s", text_msg);
            }
            else {
                // treat as text
                ESP_LOGI(TAG, "Received unknown prefix: %.*s",
                         ws_data->data_len, (char*)ws_data->data_ptr);
            }
        }
        break;
    }

    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGW(TAG, "WebSocket error");
        s_ws_connected = false;
        break;

    default:
        break;
    }
}

// A task that periodically sends text frames
static void websocket_test_task(void *arg)
{
    while (true) {
        if (s_ws_connected && s_ws_client &&
            esp_websocket_client_is_connected(s_ws_client)) 
        {
            char msg[50];
            int len = snprintf(msg, sizeof(msg),
                               "Test message %d", ++s_message_counter);
            esp_websocket_client_send_text(s_ws_client, msg, len, portMAX_DELAY);
            ESP_LOGI(TAG, "Sent: %s", msg);
            vTaskDelay(pdMS_TO_TICKS(1000));
        } else {
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }
}
