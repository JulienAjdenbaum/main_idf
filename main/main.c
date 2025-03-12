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
#include "mbedtls/base64.h"
#include <sys/param.h>  // For MIN/MAX macros

// For the audio player
#include "audio_player.h"

// -------------------- USER CONFIG --------------------
#define WIFI_SSID       "Partagedeco"
#define WIFI_PASS       "jesaispasquoimettre"
#define MAXIMUM_RETRY   5

#define AUDIO_BUFFER_SIZE 4096 // Ensure buffer consistency
#define MONO_TO_STEREO_FACTOR 2

// This endpoint sends frames prefixed by 0x02 for audio and 0x01 or normal text for messages
#define WS_URI "wss://api.interaction-labs.com/esp/test"

// -------------------- GLOBALS --------------------
static const char *TAG = "WebSocketTest";
static esp_websocket_client_handle_t s_ws_client = NULL;
static bool s_ws_connected = false;
static int s_message_counter = 0;
static int s_retry_num = 0;
static bool s_wifi_connected = false;

// -------------------- FUNCTION DECLARATIONS --------------------
static void websocket_event_handler(void *handler_args, esp_event_base_t base, 
                                    int32_t event_id, void *event_data);
static void websocket_init(void);
static void websocket_test_task(void *arg);
static void wifi_init(void);
static void event_handler(void *arg, esp_event_base_t event_base, 
                          int32_t event_id, void *event_data);

// -------------------- WIFI EVENT HANDLERS --------------------
static void event_handler(void *arg, esp_event_base_t event_base, 
                          int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_wifi_connected = false;
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
        s_wifi_connected = true;

        // Start WebSocket only after getting an IP
        if (!s_ws_client) {
            ESP_LOGI(TAG, "Starting WebSocket...");
            websocket_init();
        }
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

    // Configure and start Wi-Fi
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

    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    ESP_LOGI(TAG, "Wi-Fi init done. SSID=%s", WIFI_SSID);
}

// -------------------- WEBSOCKET EVENT HANDLERS --------------------
static void websocket_event_handler(void *handler_args, esp_event_base_t base,
                                    int32_t event_id, void *event_data)
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

    case WEBSOCKET_EVENT_DATA: {
        // Received new data from the server
        // Distinguish audio frames from text frames by the first byte
        if (ws_data->data_len > 0) {
            uint8_t *rx_buf = (uint8_t *)ws_data->data_ptr;
            uint8_t prefix = rx_buf[0];

            if (prefix == 0x02 ) {
                size_t audio_len = ws_data->data_len - 1; // skip prefix
                if (audio_len > 0) {
                    uint8_t *audio_data = (uint8_t *)malloc(audio_len);
                    if (!audio_data) {
                        ESP_LOGE(TAG, "Failed to malloc audio_data");
                        return;
                    }
                    memcpy(audio_data, rx_buf + 1, audio_len);

                    // Print debug
                    #define PRINT_LEN 8
                    size_t to_print = (audio_len < PRINT_LEN) ? audio_len : PRINT_LEN;
                    ESP_LOGI(TAG, "Audio chunk size=%d, first %d bytes:", (int)audio_len, (int)to_print);
                    for (size_t ii = 0; ii < to_print; ii++) {
                        printf("%02X ", audio_data[ii]);
                    }
                    printf("\n");

                    if (audio_len > PRINT_LEN) {
                        size_t tail_start = audio_len - to_print;
                        ESP_LOGI(TAG, "Audio chunk last %d bytes:", (int)to_print);
                        for (size_t ii = tail_start; ii < audio_len; ii++) {
                            printf("%02X ", audio_data[ii]);
                        }
                        printf("\n");
                    }

                    // Suppose data is 22,050 Hz mono -> upsample to 44,100 stereo
                    size_t num_mono_samples = audio_len / 2; // each 16-bit sample is 2 bytes

                    // We'll produce 2 samples for each mono sample => stereo
                    uint16_t *out = malloc(2 * num_mono_samples * sizeof(uint16_t));
                    if (!out) {
                        ESP_LOGE(TAG, "Failed to malloc stereo buffer");
                        free(audio_data);
                        return;
                    }

                    uint16_t *in = (uint16_t *)audio_data;
                    size_t out_index = 0;

                    // For each mono sample, write (Left, Right)
                    for (size_t i = 0; i < num_mono_samples; i++) {
                        uint16_t sample = in[i];
                        out[out_index++] = sample; // Left
                        out[out_index++] = sample; // Right
                    }

                    // Now feed to I2S
                    size_t stereo_bytes = out_index * sizeof(uint16_t); 
                    audio_player_play((uint8_t *)out, stereo_bytes);

                    // Done
                    free(audio_data);

                    // If your audio task calls free() after i2s_write, do not free(out) here.
                    // If the queue expects the user to free, then free it here or after some delay.
                    // For example:
                    // free(out);
                }
            }

            else if (prefix == 0x01) {
                // TEXT FRAME (prefixed). Let's skip the prefix and just print the text
                char text_msg[128];
                size_t text_len = ws_data->data_len - 1;
                size_t cpy_len = text_len < (sizeof(text_msg) - 1) ? text_len : (sizeof(text_msg) - 1);
                memcpy(text_msg, rx_buf + 1, cpy_len);
                text_msg[cpy_len] = '\0';
                ESP_LOGI(TAG, "Received TEXT(0x01) => %s", text_msg);
            }
            else {
                // No known prefix => treat as regular text
                ESP_LOGI(TAG, "Received from server: %.*s",
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

// -------------------- WEBSOCKET INITIALIZATION --------------------
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

// -------------------- TEST TASK: SEND & PRINT --------------------
static void websocket_test_task(void *arg)
{
    ESP_LOGI(TAG, "Waiting for Wi-Fi + WebSocket connection...");
    while (true) {
        if (s_wifi_connected && s_ws_connected && s_ws_client) {
            // Send a text message every second
            char msg[50];
            int len = snprintf(msg, sizeof(msg), "Test message %d", ++s_message_counter);
            
            if (esp_websocket_client_is_connected(s_ws_client)) {
                esp_websocket_client_send_text(s_ws_client, msg, len, portMAX_DELAY);
                ESP_LOGI(TAG, "Sent: %s", msg);
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
        } else {
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }
}

// -------------------- MAIN ENTRY --------------------
void app_main(void)
{
    esp_log_level_set("*", ESP_LOG_INFO); // Ensure info logs are visible

    // 1) Init Wi-Fi
    wifi_init();

    // 2) Init the audio player
    //    e.g. for 16-bit mono at 22,050 Hz
    if (audio_player_init() == ESP_OK) {
        audio_player_set_sample_rate(44100, 2); 
        ESP_LOGI(TAG, "Audio player init done.");
    } else {
        ESP_LOGE(TAG, "Audio player init failed!");
    }

    // 3) Start a task that periodically sends messages
    xTaskCreate(
        websocket_test_task,
        "ws_test",
        4096,
        NULL,
        5,
        NULL
    );
}
