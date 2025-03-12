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

// ---------------------------------------------------
// USER CONFIG
// ---------------------------------------------------
#define WIFI_SSID       "Partagedeco"
#define WIFI_PASS       "jesaispasquoimettre"
#define MAXIMUM_RETRY   5

// This endpoint sends frames prefixed by 0x02 for audio
#define WS_URI "wss://api.interaction-labs.com/esp/test"

// ---------------------------------------------------
// GLOBALS
// ---------------------------------------------------
static const char *TAG = "WebSocketTest";
static esp_websocket_client_handle_t s_ws_client = NULL;
static bool s_ws_connected = false;
static int s_message_counter = 0;
static int s_retry_num = 0;
static bool s_wifi_connected = false;

// WAV header structure (44 bytes)
typedef struct __attribute__((packed)) {
    char     riff[4];           // "RIFF"
    uint32_t overall_size;      
    char     wave[4];           // "WAVE"
    char     fmt[4];            // "fmt "
    uint32_t fmt_chunk_size;    
    uint16_t audio_format;      // 1 = PCM
    uint16_t num_channels;      
    uint32_t sample_rate;       
    uint32_t byte_rate;         
    uint16_t block_align;       
    uint16_t bits_per_sample;   
    char     data[4];           // "data"
    uint32_t data_size;
} wav_header_t;

// Flag & buffer for partial WAV header parsing
static bool     s_wav_header_parsed = false;
static size_t   s_header_bytes_collected = 0;
static uint8_t  s_header_buf[sizeof(wav_header_t)];

// These will store the actual WAV parameters from the header
static uint32_t s_wav_sample_rate = 44100;
static uint16_t s_wav_num_channels = 1;

// ---------------------------------------------------
// FORWARD DECLARATIONS
// ---------------------------------------------------
static void websocket_event_handler(void *handler_args, esp_event_base_t base,
                                    int32_t event_id, void *event_data);
static void websocket_init(void);
static void websocket_test_task(void *arg);
static void wifi_init(void);
static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data);

// This helper feeds raw PCM bytes to the I2S driver, possibly
// duplicating channels if the WAV is mono but I2S is set to stereo.
static void feed_pcm_data(const uint8_t *pcm_in, size_t in_len)
{
    // If the WAV is stereo, we can pass directly. If it's mono, we need
    // to duplicate each sample for the second channel.
    if (s_wav_num_channels == 1) {
        // 16-bit samples => in_len is a multiple of 2
        size_t num_mono_samples = in_len / 2;
        const uint16_t *in_samples = (const uint16_t *)pcm_in;

        // We'll produce 2 * num_mono_samples for stereo
        uint16_t *stereo_buf = malloc(num_mono_samples * 2 * sizeof(uint16_t));
        if (!stereo_buf) {
            ESP_LOGE(TAG, "Failed to alloc stereo_buf");
            return;
        }

        for (size_t i = 0; i < num_mono_samples; i++) {
            uint16_t sample = in_samples[i];
            stereo_buf[2*i + 0] = sample; // left
            stereo_buf[2*i + 1] = sample; // right
        }

        size_t stereo_bytes = num_mono_samples * 2 * sizeof(uint16_t);
        audio_player_play((uint8_t *)stereo_buf, stereo_bytes);
    } else {
        // 2-channel WAV => just push the data
        audio_player_play((uint8_t *)pcm_in, in_len);
    }
}

// Parse WAV header from the 44-byte buffer. Returns true if valid.
static bool parse_wav_header(const uint8_t *hdr)
{
    const wav_header_t *h = (const wav_header_t *)hdr;

    // Basic checks for "RIFF", "WAVE", "fmt "
    if (memcmp(h->riff, "RIFF", 4) != 0 ||
        memcmp(h->wave, "WAVE", 4) != 0 ||
        memcmp(h->fmt,  "fmt ", 4) != 0)
    {
        ESP_LOGE(TAG, "Invalid WAV header!");
        return false;
    }

    // Usually for PCM: audio_format=1
    if (h->audio_format != 1) {
        ESP_LOGE(TAG, "Unsupported WAV format (must be PCM=1). Got=%u", h->audio_format);
        return false;
    }

    ESP_LOGI(TAG, "=== WAV Header Info ===");
    ESP_LOGI(TAG, "  Sample Rate     : %u Hz",   h->sample_rate);
    ESP_LOGI(TAG, "  Channels        : %u",      h->num_channels);
    ESP_LOGI(TAG, "  Bits per Sample : %u",      h->bits_per_sample);
    ESP_LOGI(TAG, "  Data Size       : %u bytes",h->data_size);

    // Store globally for playback
    s_wav_sample_rate  = h->sample_rate;
    s_wav_num_channels = h->num_channels;

    // Now update the I2S with the new sample rate, etc.
    // We set the hardware to stereo if #channels >= 2,
    // or to mono if #channels == 1. 
    // However, often you physically want I2S in stereo mode.
    audio_player_set_sample_rate(s_wav_sample_rate,
                                 (s_wav_num_channels == 1) ? 2 : 2);
    // In the above call, we set 2 channels at the driver level, 
    // but we'll handle the "mono->stereo" duplication in code. 
    // If your hardware truly supports mono, you can call:
    //    audio_player_set_sample_rate(s_wav_sample_rate, s_wav_num_channels);

    return true;
}

// Called each time we receive an audio chunk (prefix 0x02 removed).
// Manages partial WAV header and then sends PCM data to feed_pcm_data().
static void handle_incoming_audio(const uint8_t *rx_buf, size_t rx_len)
{
    // If we haven't parsed the 44-byte header yet, accumulate until we have it
    if (!s_wav_header_parsed) {
        size_t needed = sizeof(wav_header_t) - s_header_bytes_collected;
        if (rx_len < needed) {
            // Still not enough to complete the header
            memcpy(&s_header_buf[s_header_bytes_collected], rx_buf, rx_len);
            s_header_bytes_collected += rx_len;
            return;
        } else {
            // We can complete the header
            memcpy(&s_header_buf[s_header_bytes_collected], rx_buf, needed);
            s_header_bytes_collected += needed;

            // Parse
            if (!parse_wav_header(s_header_buf)) {
                // Invalid WAV -> just return or handle error
                return;
            }

            s_wav_header_parsed = true;

            // The rest of rx_buf is PCM data
            size_t pcm_len = rx_len - needed;
            if (pcm_len > 0) {
                const uint8_t *pcm_ptr = rx_buf + needed;
                feed_pcm_data(pcm_ptr, pcm_len);
            }
        }
    }
    else {
        // Header already parsed => entire chunk is PCM
        feed_pcm_data(rx_buf, rx_len);
    }
}

// ---------------------------------------------------
// WIFI EVENT HANDLERS
// ---------------------------------------------------
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
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
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

// ---------------------------------------------------
// WEBSOCKET EVENT HANDLERS
// ---------------------------------------------------
static void websocket_event_handler(void *handler_args, esp_event_base_t base,
                                    int32_t event_id, void *event_data)
{
    esp_websocket_event_data_t *ws_data = (esp_websocket_event_data_t *) event_data;

    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "WebSocket connected");
        s_ws_connected = true;

        // Reset these so each new connection can start fresh if needed
        s_wav_header_parsed = false;
        s_header_bytes_collected = 0;
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "WebSocket disconnected");
        s_ws_connected = false;
        break;

    case WEBSOCKET_EVENT_DATA: {
        // Received new data from the server
        if (ws_data->data_len > 0) {
            const uint8_t *rx_buf = (const uint8_t *)ws_data->data_ptr;
            uint8_t prefix = rx_buf[0];

            if (prefix == 0x02) {
                // Audio data (minus prefix)
                size_t audio_len = ws_data->data_len - 1;
                if (audio_len > 0) {
                    // Copy chunk to a new buffer so we can feed to handle_incoming_audio
                    uint8_t *chunk = malloc(audio_len);
                    if (!chunk) {
                        ESP_LOGE(TAG, "malloc failed for audio chunk!");
                        return;
                    }
                    memcpy(chunk, rx_buf + 1, audio_len);

                    // Process the chunk (parse header if needed, then feed PCM)
                    handle_incoming_audio(chunk, audio_len);

                    // handle_incoming_audio() might pass it along to audio_player
                    // Freed there if we convert to stereo, or not used if header only
                    // So let's free it here ONLY if not used by feed_pcm_data:
                    // Actually, feed_pcm_data duplicates the data again if needed,
                    // so it's safe to free now.
                    free(chunk);
                }
            }
            else if (prefix == 0x01) {
                // TEXT FRAME (prefixed). Just skip prefix and print the text
                char text_msg[128];
                size_t text_len = ws_data->data_len - 1;
                size_t cpy_len = (text_len < sizeof(text_msg) - 1)
                                ? text_len
                                : (sizeof(text_msg) - 1);
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

// ---------------------------------------------------
// WEBSOCKET INITIALIZATION
// ---------------------------------------------------
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

// ---------------------------------------------------
// TEST TASK: SEND & PRINT
// ---------------------------------------------------
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

// ---------------------------------------------------
// MAIN
// ---------------------------------------------------
void app_main(void)
{
    esp_log_level_set("*", ESP_LOG_INFO); // ensure info logs are visible

    // 1) Init Wi-Fi
    wifi_init();

    // 2) Init the audio player
    esp_err_t audio_ok = audio_player_init();
    if (audio_ok != ESP_OK) {
        ESP_LOGE(TAG, "Audio player init failed!");
        return;
    }
    // (Optional) set an initial sample rate if needed:
    // audio_player_set_sample_rate(44100, 2);

    // 3) Start a task that periodically sends messages to the server
    xTaskCreate(
        websocket_test_task,
        "ws_test",
        4096,
        NULL,
        5,
        NULL
    );
}
