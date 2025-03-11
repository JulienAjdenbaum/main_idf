#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"

#include "esp_netif.h"
#include "esp_websocket_client.h"
#include "esp_crt_bundle.h"
#include "lwip/sockets.h"
#include "esp_sleep.h"
#include "lwip/netdb.h"  // For getaddrinfo/freeaddrinfo

#include "audio_recorder.h"  // Include the modified I2S recording functions

// -------------------- USER CONFIG --------------------
#define WIFI_SSID       "Partagedeco"
#define WIFI_PASS       "jesaispasquoimettre"
#define MAXIMUM_RETRY   5

// WebSocket
#define WS_URI "wss://api.interaction-labs.com/esp/record"

// -------------------- GLOBALS --------------------
static const char *TAG = "AudioWebsocket";

static esp_websocket_client_handle_t s_ws_client = NULL;
static bool s_ws_connected = false;
static int s_retry_num = 0;

// -------------------- WIFI EVENT HANDLERS --------------------
static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "Retrying Wi-Fi connection...");
        } else {
            ESP_LOGE(TAG, "Failed to connect after %d retries", MAXIMUM_RETRY);
        }
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
    }
}

// -------------------- WIFI INIT --------------------
static void wifi_init(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Wi-Fi init done. SSID=%s", WIFI_SSID);
}

// -------------------- WEBSOCKET HANDLERS --------------------
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
        case WEBSOCKET_EVENT_ERROR:
            ESP_LOGE(TAG, "WebSocket error");
            s_ws_connected = false;
            break;
        default:
            break;
    }
}

static void websocket_init(void)
{
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

    s_ws_client = esp_websocket_client_init(&cfg);
    if (!s_ws_client) {
        ESP_LOGE(TAG, "Failed to init WebSocket client");
        return;
    }

    esp_websocket_register_events(
        s_ws_client,
        WEBSOCKET_EVENT_ANY,
        websocket_event_handler,
        NULL
    );

    esp_err_t err = esp_websocket_client_start(s_ws_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WebSocket start failed: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "WebSocket started => %s", WS_URI);
}

// -------------------- RECORD & SEND TASK --------------------
static void record_and_send_audio(void *arg)
{
    ESP_LOGI(TAG, "Waiting for WebSocket to connect...");
    while (!s_ws_connected) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    ESP_LOGI(TAG, "WebSocket connected, starting recording...");
    
    // Use the existing recording function
    record_audio_to_sd_card("recording.wav");

    ESP_LOGI(TAG, "Finished recording, now sending...");

    FILE *file = fopen("/sdcard/recording.wav", "rb");
    if (!file) {
        ESP_LOGE(TAG, "Failed to open WAV file");
        vTaskDelete(NULL);
        return;
    }

    uint8_t buffer[1024];
    size_t bytes_read;
    
    while ((bytes_read = fread(buffer, 1, sizeof(buffer), file)) > 0) {
        if (!s_ws_connected) {
            ESP_LOGE(TAG, "WebSocket disconnected mid-transfer");
            break;
        }
        esp_websocket_client_send_bin(s_ws_client, (char *)buffer, bytes_read, pdMS_TO_TICKS(100));
    }
    
    fclose(file);
    ESP_LOGI(TAG, "File sent successfully");

    vTaskDelete(NULL);
}

// -------------------- MAIN --------------------
void app_main(void)
{
    esp_log_level_set("WEBSOCKET_CLIENT", ESP_LOG_DEBUG);

    // 1) Wi-Fi
    wifi_init();

    // 2) I2S init (calls function from audio_recorder.c)
    i2s_init_for_mic();

    // 3) WebSocket init
    websocket_init();

    // 4) Start recording and sending task
    xTaskCreate(
        record_and_send_audio,
        "record_and_send_audio",
        16384,
        NULL,
        5,
        NULL
    );
}
