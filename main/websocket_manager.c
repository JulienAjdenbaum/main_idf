#include "websocket_manager.h"
#include "esp_websocket_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include <string.h>
#include <stdio.h>

#include "rc522_picc.h"
#include "tag_reader.h"
#include "audio_stream.h"
#include "OTA.h"

#ifndef RC522_PICC_MAX_UID_SIZE
#define RC522_PICC_MAX_UID_SIZE 10
#endif

static const char *TAG = "WS_MGR";

// The actual WebSocket client handle
static esp_websocket_client_handle_t s_ws_client = NULL;
static bool s_ws_connected = false;

/**
 * Task handles for ringbuf monitor & audio consumer. 
 * The mic record task is in audio_record.c
 */
static TaskHandle_t s_ringbuf_monitor_handle = NULL;
static TaskHandle_t s_audio_consumer_handle  = NULL;

/** 
 * A global shutdown flag so tasks know when to exit loops.
 * Only visible inside this file, but we expose read/write 
 * functions for other modules.
 */
static bool g_shutdown_requested = false;

// Our ring buffer for inbound audio
static RingbufHandle_t s_audio_rb = NULL;

// Prototypes
static void websocket_event_handler(void *handler_args,
                                    esp_event_base_t base,
                                    int32_t event_id,
                                    void *event_data);
static void audio_consumer_task(void *arg);
static void ringbuf_monitor_task(void *arg);


// ============================= PUBLIC API =============================

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

    // Create ring buffer for inbound audio
    s_audio_rb = xRingbufferCreate(16 * 1024, RINGBUF_TYPE_BYTEBUF);
    if (!s_audio_rb) {
        ESP_LOGE(TAG, "Failed to create ring buffer");
        return ESP_FAIL;
    }

    // Start the WebSocket client
    esp_err_t err = esp_websocket_client_start(s_ws_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start WS client: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "WebSocket started!");
    // s_ws_connected = true;

    // Create consumer task (audio_consumer_task) 
    BaseType_t rc = xTaskCreatePinnedToCore(
        audio_consumer_task,
        "audio_consumer_task",
        4096,
        NULL,
        5,
        &s_audio_consumer_handle,
        1
    );
    if (rc != pdPASS) {
        ESP_LOGE(TAG, "Failed to create audio_consumer_task");
    }

    // Create ringbuf monitor task
    rc = xTaskCreatePinnedToCore(
        ringbuf_monitor_task,
        "ringbuf_monitor",
        4096,
        NULL,
        4,
        &s_ringbuf_monitor_handle,
        1
    );
    if (rc != pdPASS) {
        ESP_LOGE(TAG, "Failed to create ringbuf_monitor_task");
    }

    return ESP_OK;
}

esp_err_t websocket_manager_stop(void)
{
    ESP_LOGI(TAG, "Stopping WebSocket client...");
    if (s_ws_client) {
        esp_websocket_client_stop(s_ws_client);
        esp_websocket_client_destroy(s_ws_client);
        s_ws_client = NULL;
    }
    s_ws_connected = false;

    if (s_audio_rb) {
        ESP_LOGI(TAG, "Deleting ring buffer...");
        vRingbufferDelete(s_audio_rb);
        s_audio_rb = NULL;
    }
    ESP_LOGI(TAG, "WebSocket + ring buffer freed");
    return ESP_OK;
}

void websocket_manager_request_shutdown(void)
{
    ESP_LOGI(TAG, "Requesting shutdown...");
    g_shutdown_requested = true;
}

bool websocket_manager_is_shutdown_requested(void)
{
    return g_shutdown_requested;
}

bool websocket_manager_is_connected(void)
{
    return s_ws_connected && (s_ws_client != NULL);
}

int websocket_manager_send_bin(const char *data, size_t len)
{
    if (!s_ws_connected || !s_ws_client) {
        return -1;
    }
    int ret = esp_websocket_client_send_bin(s_ws_client, data, len, portMAX_DELAY);
    return (ret == ESP_OK) ? (int)len : -1;
}

void websocket_manager_send_rfid_event(const uint8_t *uid, size_t uid_len, bool tag_removed)
{
    if (!s_ws_connected || !s_ws_client) {
        ESP_LOGW(TAG, "WebSocket not connected: RFID event not sent");
        return;
    }

    if (tag_removed) {
        uint8_t buffer[1] = {0x03};
        ESP_LOGI(TAG, "Sending tag REMOVED (0x03)");
        websocket_manager_send_bin((const char *)buffer, 1);
    } else {
        uint8_t buffer[1 + RC522_PICC_MAX_UID_SIZE];
        buffer[0] = 0x03;
        if (uid_len > RC522_PICC_MAX_UID_SIZE) {
            ESP_LOGW(TAG, "UID too long, skipping send");
            return;
        }
        memcpy(&buffer[1], uid, uid_len);
        ESP_LOGI(TAG, "Sending tag UID (0x03...) len=%d", (int)uid_len);
        websocket_manager_send_bin((const char *)buffer, 1 + uid_len);
    }
}


// ============================= TASKS =============================

static void ringbuf_monitor_task(void *arg)
{
    while (!g_shutdown_requested) {
        if (s_audio_rb) {
            const size_t total_size = 16 * 1024; 
            size_t free_bytes = xRingbufferGetCurFreeSize(s_audio_rb);
            size_t used_bytes = total_size - free_bytes;

            ESP_LOGI(TAG, "[RingBuf Monitor] used=%u, free=%u (of %u)",
                    (unsigned)used_bytes, (unsigned)free_bytes, (unsigned)total_size);
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    ESP_LOGI(TAG, "ringbuf_monitor_task stopping!");
    vTaskDelete(NULL);
}

static void audio_consumer_task(void *arg)
{
    while (!g_shutdown_requested) {
        if (s_audio_rb) {
            size_t item_size = 0;
            // Wait for data from ring buffer
            uint8_t *audio_chunk = (uint8_t *) xRingbufferReceive(
                s_audio_rb, &item_size, pdMS_TO_TICKS(100));
            if (audio_chunk) {
                // handle incoming audio
                audio_stream_handle_incoming(audio_chunk, item_size);
                // Return memory to ring buffer
                vRingbufferReturnItem(s_audio_rb, (void*)audio_chunk);
            }
        }
        // else either no ringbuf or timed out => loop
    }
    ESP_LOGI(TAG, "audio_consumer_task stopping!");
    vTaskDelete(NULL);
}


// ============================= WS EVENT HANDLER =============================
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
        // Immediately send device version info
        ota_send_device_version();

        // Optionally if RFID is active, send last UID
        if (s_card_active && s_last_uid_len > 0) {
            ESP_LOGI(TAG, "Sending current RFID UID to server (upon connect)");
            websocket_manager_send_rfid_event(s_last_uid, s_last_uid_len, false);
        }
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "WebSocket disconnected");
        s_ws_connected = false;
        break;

    case WEBSOCKET_EVENT_DATA:
        // handle inbound data (prefix 0x02 for audio, etc.)
        if (ws_data->data_len > 0) {
            const uint8_t *rx_buf = (const uint8_t *)ws_data->data_ptr;
            uint8_t prefix = rx_buf[0];
            size_t audio_len = ws_data->data_len - 1;

            if (prefix == 0x02 && audio_len > 0) {
                if (s_audio_rb) {
                    // copy minus prefix
                    BaseType_t ok = xRingbufferSend(s_audio_rb, 
                                                    (void*)(rx_buf + 1),
                                                    audio_len,
                                                    pdMS_TO_TICKS(50));
                    if (!ok) {
                        ESP_LOGE(TAG, "Ring buffer full => dropped audio!");
                    }
                }
            }
            else if (prefix == 0x01) {
                ESP_LOGI(TAG, "Text => %.*s", (int)audio_len, (char*)(rx_buf+1));
            }
            else if (prefix == 0x04 && audio_len > 0) {
                // Possibly an OTA command => "OTA=<url>"
                char *payload_str = strndup((const char*)(rx_buf + 1), audio_len);
                if (payload_str) {
                    ESP_LOGI(TAG, "Got server OTA message => %s", payload_str);
                    if (strncmp(payload_str, "OTA=", 4) == 0) {
                        const char *url = payload_str + 4; 
                        ESP_LOGI(TAG, "Server says new firmware at: %s", url);
                        ota_start_update(url);
                    } else {
                        ESP_LOGW(TAG, "0x04 message but not an OTA command: %s", payload_str);
                    }
                    free(payload_str);
                }
            } else {
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