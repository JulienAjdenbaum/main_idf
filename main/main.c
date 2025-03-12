#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "driver/i2s.h"              // (legacy) I2S
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
#include "mbedtls/base64.h"
#include <sys/param.h>  // For MIN/MAX macros

// -------------------- USER CONFIG --------------------
#define WIFI_SSID       "Partagedeco"
#define WIFI_PASS       "jesaispasquoimettre"
#define MAXIMUM_RETRY   5

// I2S pins for mic input
#define I2S_PORT        I2S_NUM_0
#define I2S_BCK_IO      26
#define I2S_WS_IO       25
#define I2S_DATA_IN_IO  32
#define I2S_DATA_OUT_IO -1  // not used for mic‑only

// Audio settings
#define I2S_SAMPLE_RATE        22050
#define I2S_BITS_PER_SAMPLE    I2S_BITS_PER_SAMPLE_16BIT
#define I2S_READ_BUF_SIZE      1024   // how many raw bytes to read per i2s_read() call
#define RECORDING_DURATION_MS  10000  // record for 10s once WebSocket is up

// WebSocket
#define WS_URI "wss://api.interaction-labs.com/esp/record"

// -------------------- GLOBALS --------------------
static const char *TAG = "AudioWebsocket";

static esp_websocket_client_handle_t s_ws_client = NULL;
static bool s_ws_connected = false;
static portMUX_TYPE s_ws_spinlock = portMUX_INITIALIZER_UNLOCKED;

static int s_retry_num = 0;


// -------------------- WIFI EVENT HANDLERS --------------------
static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        // Attempt to connect as soon as STA starts
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


static void wifi_init(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Create default STA
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // Register for Wi-Fi events
    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, &instance_got_ip));

    // Configure and start
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

    // Optionally disable power save
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    ESP_LOGI(TAG, "Wi-Fi init done. SSID=%s", WIFI_SSID);
}


// -------------------- I2S INIT --------------------
static void i2s_init_for_mic(void)
{
    i2s_config_t i2s_config = {
        .mode = I2S_MODE_MASTER | I2S_MODE_RX,
        .sample_rate = I2S_SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,  // **Use 16-bit**
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,  // **Mono Channel**
        .communication_format = I2S_COMM_FORMAT_I2S,
        .dma_buf_count = 8,
        .dma_buf_len = 512,
        .use_apll = false,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1
    };

    i2s_pin_config_t pin_config = {
        .bck_io_num = I2S_BCK_IO,
        .ws_io_num = I2S_WS_IO,
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num = I2S_DATA_IN_IO
    };

    ESP_ERROR_CHECK(i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL));
    ESP_ERROR_CHECK(i2s_set_pin(I2S_PORT, &pin_config));

    ESP_LOGI(TAG, "I2S initialized for mono microphone input");
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
        // If the server sends data back, you can handle it here
        ESP_LOGI(TAG, "WS DATA: len=%d: %.*s",
                 ws_data->data_len,
                 ws_data->data_len,
                 (char*)ws_data->data_ptr);
        break;

    case WEBSOCKET_EVENT_ERROR:
    {
        const esp_err_t *err_code = (esp_err_t*) ws_data->data_ptr;
        ESP_LOGW(TAG, "WebSocket ERROR: %s", esp_err_to_name(*err_code));
        // Mark disconnected
        s_ws_connected = false;
        break;
    }
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

// -------------------- RECORD + SEND TASK --------------------
static void i2s_record_and_send_task(void *arg)
{
    ESP_LOGI(TAG, "Waiting for WebSocket to connect...");
    while (!s_ws_connected) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    ESP_LOGI(TAG, "WebSocket connected, begin recording for %d ms", RECORDING_DURATION_MS);

    TickType_t start_ticks = xTaskGetTickCount();
    TickType_t stop_ticks  = start_ticks + pdMS_TO_TICKS(RECORDING_DURATION_MS);

    int16_t sample_buffer[I2S_READ_BUF_SIZE / 2]; // Mono => each sample is 2 bytes
    size_t bytes_read = 0;

    while (xTaskGetTickCount() < stop_ticks)
    {
        if (!s_ws_connected) {
            ESP_LOGW(TAG, "WebSocket disconnected mid-record");
            break;
        }

        // Read I2S data
        esp_err_t err = i2s_read(I2S_PORT, sample_buffer, I2S_READ_BUF_SIZE, &bytes_read, portMAX_DELAY);

        if (err != ESP_OK) {
            ESP_LOGE(TAG, "i2s_read error: %s", esp_err_to_name(err));
            continue;
        }

        if (bytes_read == 0) {
            continue;
        }

        // Ensure WebSocket is still connected before sending
        if (esp_websocket_client_is_connected(s_ws_client)) {
            int sent = esp_websocket_client_send_bin(s_ws_client,
                                                     (const char *)sample_buffer,
                                                     bytes_read, // Send raw mono data size
                                                     pdMS_TO_TICKS(100));
            if (sent < 0) {
                ESP_LOGE(TAG, "WebSocket send failed");
                break;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(1)); // Small yield to prevent watchdog issues
    }

    ESP_LOGI(TAG, "Finished I2S record + WS send task");
    vTaskDelete(NULL);
}



void app_main(void)
{

    esp_log_level_set("TRANSPORT", ESP_LOG_DEBUG);
    esp_log_level_set("esp-tls", ESP_LOG_DEBUG);
    esp_log_level_set("WEBSOCKET_CLIENT", ESP_LOG_DEBUG);
    // 1) Wi-Fi
    wifi_init();

    // 2) I2S init
    i2s_init_for_mic();

    // 3) WebSocket init
    websocket_init();

    // 4) Create a task that waits for the WS connect, then records 10s
    xTaskCreate(
        i2s_record_and_send_task,
        "i2s_record_and_send",
        16384,
        NULL,
        5,
        NULL
    );
}