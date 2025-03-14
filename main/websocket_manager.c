// websocket_manager.c

#include "websocket_manager.h"
#include "esp_websocket_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include <string.h>
#include <stdio.h>

#include "audio_stream.h"

static const char *TAG = "WS_MGR";
static esp_websocket_client_handle_t s_ws_client = NULL;
static bool s_ws_connected = false;

// Our ring buffer for inbound audio
static RingbufHandle_t s_audio_rb = NULL;

static void websocket_event_handler(void *handler_args,
                                    esp_event_base_t base,
                                    int32_t event_id,
                                    void *event_data);

static void audio_consumer_task(void *arg);

esp_err_t websocket_manager_init(void)
{
    esp_websocket_client_config_t cfg = {
        .uri = "wss://api.interaction-labs.com/esp/api/chat",
        .crt_bundle_attach = esp_crt_bundle_attach,
        .reconnect_timeout_ms = 5000,
        .network_timeout_ms = 10000,
    };

    s_ws_client = esp_websocket_client_init(&cfg);
    if (!s_ws_client) {
        ESP_LOGE(TAG, "Failed to init WS client");
        return ESP_FAIL;
    }

    // Register the event handler
    esp_websocket_register_events(
        s_ws_client,
        WEBSOCKET_EVENT_ANY,
        websocket_event_handler,
        NULL
    );

    // Create ring buffer
    s_audio_rb = xRingbufferCreate(16 * 1024, RINGBUF_TYPE_BYTEBUF);
    if (!s_audio_rb) {
        ESP_LOGE(TAG, "Failed to create ring buffer");
        return ESP_FAIL;
    }

    // Start the WebSocket
    esp_err_t err = esp_websocket_client_start(s_ws_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start WS client: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "WebSocket started!");
    s_ws_connected = true;

    // Create consumer task
    xTaskCreatePinnedToCore(audio_consumer_task, "audio_consumer_task",
                            4096, NULL, 5, NULL, 1);

    return ESP_OK;
}

static void audio_consumer_task(void *arg)
{
    while (1) {
        size_t item_size = 0;
        // Wait for data from ring buffer
        uint8_t *audio_chunk = (uint8_t *) xRingbufferReceive(s_audio_rb, &item_size, portMAX_DELAY);
        if (audio_chunk) {
            // We have raw PCM (mono) => handle it
            audio_stream_handle_incoming(audio_chunk, item_size);

            // Return memory to ring buffer
            vRingbufferReturnItem(s_audio_rb, (void*)audio_chunk);
        }
    }
    vTaskDelete(NULL);
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
        if (ws_data->data_len > 0) {
            const uint8_t *rx_buf = (const uint8_t *)ws_data->data_ptr;
            uint8_t prefix = rx_buf[0];
            size_t audio_len = ws_data->data_len - 1;

            if (prefix == 0x02 && audio_len > 0) {
                // It's audio data
                // Debug: print first & last 6 bytes
                size_t show = (audio_len < 6) ? audio_len : 6;
                ESP_LOGI(TAG, "Audio => len=%u, first %u bytes:",
                         (unsigned)audio_len, (unsigned)show);
                for (int i = 0; i < show; i++) {
                    printf("%02X ", rx_buf[1 + i]);
                }
                printf(" | last %u bytes: ", (unsigned)show);
                for (int i = 0; i < show; i++) {
                    printf("%02X ", rx_buf[1 + audio_len - show + i]);
                }
                printf("\n");

                // Push into ring buffer minus prefix
                BaseType_t ok = xRingbufferSend(
                    s_audio_rb, (void*)(rx_buf + 1),
                    audio_len, pdMS_TO_TICKS(50)
                );
                if (!ok) {
                    ESP_LOGE(TAG, "Ring buffer full => dropped audio!");
                }
            } else if (prefix == 0x01) {
                // Text message
                ESP_LOGI(TAG, "Text => %.*s", (int)audio_len, (char*)(rx_buf+1));
            } else {
                // unknown prefix
                ESP_LOGW(TAG, "Unknown prefix=0x%02X => ignoring", prefix);
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
