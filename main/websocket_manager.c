#include "websocket_manager.h"
#include "esp_websocket_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include <string.h>
#include <stdio.h>
#include <stdbool.h>

// Include NVS headers for storing/loading API key
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_system.h"    // for esp_fill_random()

#include "rc522_picc.h"
#include "tag_reader.h"
#include "audio_stream.h"
#include "OTA.h"
#include "esp_timer.h"
#include "coredump_manager.h"

#ifndef RC522_PICC_MAX_UID_SIZE
#define RC522_PICC_MAX_UID_SIZE 10
#endif

#define NVS_NAMESPACE_API_KEY   "api_store"
#define NVS_KEY_API_KEY         "api_key"


static const char *TAG = "WS_MGR";

#define API_KEY_RANDOM_BYTES    16 
#define API_KEY_HEADER_NAME     "X-API-Key"

// The actual WebSocket client handle
static esp_websocket_client_handle_t s_ws_client = NULL;
static bool s_ws_connected = false;

static char s_api_key[API_KEY_RANDOM_BYTES * 2 + 1] = {0};  // e.g. 32 hex chars + null
static char s_api_key_header[64] = {0};                    // "X-API-Key: <key>\r\n"

static TaskHandle_t s_ws_monitor_handle = NULL;
static volatile int64_t s_last_ping_time = 0;
static volatile bool s_ws_just_connected = false;
#define WS_CONNECT_DELAY_MS  1000
#define WS_PING_TIMEOUT_MS   10000

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

static bool s_crash_triggered = false;

static TaskHandle_t s_ws_manager_task = NULL;

static void coredump_test_task(void *arg)
{
    // Wait a few seconds so logs can flush and Wi-Fi can stabilize
    vTaskDelay(pdMS_TO_TICKS(5000));

    ESP_LOGW("CORE_TEST", "About to forcibly crash => testing coredump handling...");

    // Option 1: Call abort() (preferred because it's a standard IDF panic)
    abort();

    // Option 2: Could do assert(0) or something similarly guaranteed to crash
    // assert(false);

    // We'll never get here, but in case:
    vTaskDelete(NULL);
}

static void stop_audio_consumer_and_ringbuf(void)
{
    ESP_LOGI(TAG, "Stopping audio consumer tasks...");

    s_ws_manager_task = xTaskGetCurrentTaskHandle();

    // 1) Signal tasks to exit
    g_shutdown_requested = true;

    // 2) Wait for audio_consumer_task to exit (if handle is known)
    if (s_audio_consumer_handle) {
        ESP_LOGI(TAG, "Waiting for audio_consumer_task to stop...");
        // Wait until the task deletes itself
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        s_audio_consumer_handle = NULL; 
    }

    // 3) Also wait for ringbuf_monitor_task if you want
    if (s_ringbuf_monitor_handle) {
        ESP_LOGI(TAG, "Waiting for ringbuf_monitor_task to stop...");
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        s_ringbuf_monitor_handle = NULL;
    }

    // 4) Now we can safely delete the ring buffer
    if (s_audio_rb) {
        ESP_LOGI(TAG, "Deleting ring buffer...");
        vRingbufferDelete(s_audio_rb);
        s_audio_rb = NULL;
    }
    s_ws_manager_task = NULL;
    // Reset our shutdown flag for the next time
    g_shutdown_requested = false;
    ESP_LOGI(TAG, "Audio consumer + ringbuf fully stopped.");
}


static void ws_monitor_task(void *arg)
{
    // Convert ms => microseconds for our checks
    int64_t ping_timeout_us  = WS_PING_TIMEOUT_MS * 1000;  // 4000 ms
    int64_t connect_delay_us = WS_CONNECT_DELAY_MS * 1000; // 1000 ms

    while (!g_shutdown_requested) {
        if (s_ws_connected) {
            // If we just connected, wait 1 second and clear the flag
            if (s_ws_just_connected) {
                vTaskDelay(pdMS_TO_TICKS(WS_CONNECT_DELAY_MS));
                s_ws_just_connected = false;
            }

            // Check how long since last ping
            int64_t now_us = esp_timer_get_time();
            int64_t elapsed_us = now_us - s_last_ping_time;

            if (elapsed_us > ping_timeout_us) {
                ESP_LOGE(TAG, "No ping received in %d ms => restarting WebSocket",
                         WS_PING_TIMEOUT_MS);

                // Cleanly stop current WS
                websocket_manager_stop();

                // Small delay before re-init
                vTaskDelay(pdMS_TO_TICKS(500));

                // Re-initialize WS
                websocket_manager_init();

                // Because we reconnected inside this task, let's
                // give it a second or two before next check:
                vTaskDelay(pdMS_TO_TICKS(2000));
                continue;
            }
        }

        // Sleep a bit between checks
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    ESP_LOGI(TAG, "ws_monitor_task stopping!");
    vTaskDelete(NULL);
}

static esp_err_t get_or_create_api_key(char *out_key, size_t out_key_size)
{
    if (!out_key || out_key_size < (API_KEY_RANDOM_BYTES * 2 + 1)) {
        return ESP_ERR_INVALID_ARG;
    }

    // Initialize with zero
    memset(out_key, 0, out_key_size);

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE_API_KEY, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS namespace (%s)", esp_err_to_name(err));
        return err;
    }

    // Try to get the existing key
    size_t required_size = out_key_size;
    err = nvs_get_str(nvs_handle, NVS_KEY_API_KEY, out_key, &required_size);

    if (err == ESP_OK) {
        // We successfully loaded an existing API key
        ESP_LOGI(TAG, "Loaded existing API key from NVS: %s", out_key);
        nvs_close(nvs_handle);
        return ESP_OK;
    } 
    else if (err == ESP_ERR_NVS_NOT_FOUND) {
        // Not found: generate a new random key
        uint8_t random_bytes[API_KEY_RANDOM_BYTES];
        esp_fill_random(random_bytes, sizeof(random_bytes));

        // Convert to hex
        for (int i = 0; i < API_KEY_RANDOM_BYTES; i++) {
            sprintf(out_key + (i * 2), "%02X", random_bytes[i]);
        }
        out_key[API_KEY_RANDOM_BYTES * 2] = '\0'; // Null-terminate

        ESP_LOGI(TAG, "Generated new API key: %s", out_key);

        // Store in NVS
        err = nvs_set_str(nvs_handle, NVS_KEY_API_KEY, out_key);
        if (err == ESP_OK) {
            err = nvs_commit(nvs_handle);
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Error storing API key in NVS: %s", esp_err_to_name(err));
            nvs_close(nvs_handle);
            return err;
        }

        nvs_close(nvs_handle);
        return ESP_OK;
    } 
    else {
        // Some other error
        ESP_LOGE(TAG, "Error reading API key from NVS: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }
}

// ============================= PUBLIC API =============================

esp_err_t websocket_manager_init(void)
{
    coredump_manager_check_and_load();

    // 1. Get or create the API key from NVS
    esp_err_t err = get_or_create_api_key(s_api_key, sizeof(s_api_key));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get/create API key: %s", esp_err_to_name(err));
        return err;
    }
    // 3) Build your header with or without X-Coredump
    memset(s_api_key_header, 0, sizeof(s_api_key_header));

    if (coredump_manager_found()) {
        const char* coredump_b64 = coredump_manager_get_base64();
        snprintf(s_api_key_header, sizeof(s_api_key_header),
                 "X-API-Key: %s\r\nX-Coredump: %s\r\n",
                 s_api_key, coredump_b64);
    } else {
        snprintf(s_api_key_header, sizeof(s_api_key_header),
                 "X-API-Key: %s\r\n",
                 s_api_key);
    }

    ESP_LOGI(TAG, "Final WebSocket headers:\n%s", s_api_key_header);


    // 3. Prepare the WebSocket config, adding our custom header
    esp_websocket_client_config_t cfg = {
        .uri = "ws://api.interaction-labs.com/tests/esp/api/chat",
        // .crt_bundle_attach = esp_crt_bundle_attach,
        .reconnect_timeout_ms = 5000,
        .network_timeout_ms = 10000,
        .headers = s_api_key_header, // <---- CUSTOM HEADER HERE
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
    s_audio_rb = xRingbufferCreate(32 * 1024, RINGBUF_TYPE_BYTEBUF);
    if (!s_audio_rb) {
        ESP_LOGE(TAG, "Failed to create ring buffer");
        return ESP_FAIL;
    }

    BaseType_t rc = xTaskCreatePinnedToCore(
        ws_monitor_task,
        "ws_monitor_task",
        4096,
        NULL,
        4,
        &s_ws_monitor_handle,
        1
    );
    if (rc != pdPASS) {
        ESP_LOGE(TAG, "Failed to create ws_monitor_task");
    }

    // Start the WebSocket client
    err = esp_websocket_client_start(s_ws_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start WS client: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "WebSocket started with header [%s]", s_api_key_header);

    // Create consumer task (audio_consumer_task)
    rc = xTaskCreatePinnedToCore(
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

    // (1) Stop audio-related stuff first
    stop_audio_consumer_and_ringbuf();

    // (2) Immediately mark WebSocket disconnected to block other logic
    s_ws_connected = false;

    if (s_ws_client) {
        // (3) Stop the WebSocket client
        esp_err_t stop_err = esp_websocket_client_stop(s_ws_client);
        if (stop_err != ESP_OK) {
            ESP_LOGW(TAG, "WebSocket stop failed: %s", esp_err_to_name(stop_err));
        }

        // (4) Unregister all events before destroying
        esp_websocket_unregister_events(
            s_ws_client,
            WEBSOCKET_EVENT_ANY,
            websocket_event_handler
        );

        // (5) Destroy the client handle
        esp_websocket_client_destroy(s_ws_client);
        s_ws_client = NULL;
    }

    // (6) Delete ring buffer if it still exists (safety)
    if (s_audio_rb) {
        ESP_LOGI(TAG, "Deleting ring buffer...");
        vRingbufferDelete(s_audio_rb);
        s_audio_rb = NULL;
    }

    ESP_LOGI(TAG, "WebSocket + ring buffer fully stopped.");
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
    ESP_LOGI(TAG, "ringbuf_monitor_task starting!");
    while (!g_shutdown_requested) {
        if (s_audio_rb) {
            const size_t total_size = 32 * 1024; 
            size_t free_bytes = xRingbufferGetCurFreeSize(s_audio_rb);
            size_t used_bytes = total_size - free_bytes;

            ESP_LOGI(TAG, "[RingBuf Monitor] used=%u, free=%u (of %u)",
                    (unsigned)used_bytes, (unsigned)free_bytes, (unsigned)total_size);
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    ESP_LOGI(TAG, "ringbuf_monitor_task stopping!");
    
    if (s_ws_manager_task) {
        xTaskNotifyGive(s_ws_manager_task);
    }
    vTaskDelete(NULL);
}

static void audio_consumer_task(void *arg)
{
    ESP_LOGI(TAG, "audio_consumer_task starting!");
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
            vTaskDelay(pdMS_TO_TICKS(1));
        }
        // else either no ringbuf or timed out => loop
    }
    ESP_LOGI(TAG, "audio_consumer_task stopping!");
    
    if (s_ws_manager_task) {
        xTaskNotifyGive(s_ws_manager_task);
    }
    // xTaskNotifyGive(xTaskGetCurrentTaskHandle());
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
        s_ws_connected      = true;
        s_ws_just_connected = true;
        s_last_ping_time    = esp_timer_get_time(); // reset ping timer

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
            
            if (prefix == 0x05) {
                // 1) Received custom "Ping" from server => respond with Pong (0x06)
                // ESP_LOGI(TAG, "Received custom Ping (0x05) => sending Pong (0x06).");
                uint8_t pong_data[1] = { 0x06 };
                websocket_manager_send_bin((const char *)pong_data, 1);

                // 2) Update last-ping time
                s_last_ping_time = esp_timer_get_time();
            }

            else if (prefix == 0x02 && audio_len > 0) {

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