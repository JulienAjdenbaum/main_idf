#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/rmt_tx.h"
#include "led_strip_encoder.h"
#include "LED_button.h"
#include "wifi.h"
#include "websocket_manager.h"

#define RMT_LED_STRIP_RESOLUTION_HZ  (10 * 1000 * 1000) // 10MHz -> 0.1us per tick
#define RMT_LED_STRIP_GPIO_NUM       15                 // GPIO connected to SK6812 data
#define EXAMPLE_LED_NUMBERS          1                  // Number of LEDs in the strip
#define BUTTON_GPIO                  4                  // GPIO for push button
#define BLINK_DELAY_MS               300                // LED on/off time when blinking

static const char *TAG = "led_button_blink";

static uint8_t led_strip_pixels[EXAMPLE_LED_NUMBERS * 4]; // GRBW per LED
static rmt_channel_handle_t led_chan = NULL;
static rmt_encoder_handle_t led_encoder = NULL;
static rmt_transmit_config_t tx_config = {
    .loop_count = 0,
};

void blink_all_leds(uint8_t r, uint8_t g, uint8_t b, uint8_t w, int delay_ms)
{
    // Set color
    for (int i = 0; i < EXAMPLE_LED_NUMBERS; i++) {
        int offset = i * 4;
        led_strip_pixels[offset + 0] = g;
        led_strip_pixels[offset + 1] = r;
        led_strip_pixels[offset + 2] = b;
        led_strip_pixels[offset + 3] = w;
    }
    ESP_ERROR_CHECK(rmt_transmit(led_chan, led_encoder,
                                 led_strip_pixels, sizeof(led_strip_pixels),
                                 &tx_config));
    ESP_ERROR_CHECK(rmt_tx_wait_all_done(led_chan, portMAX_DELAY));
    vTaskDelay(pdMS_TO_TICKS(delay_ms));

    // Turn off
    memset(led_strip_pixels, 0, sizeof(led_strip_pixels));
    ESP_ERROR_CHECK(rmt_transmit(led_chan, led_encoder,
                                 led_strip_pixels, sizeof(led_strip_pixels),
                                 &tx_config));
    ESP_ERROR_CHECK(rmt_tx_wait_all_done(led_chan, portMAX_DELAY));
    vTaskDelay(pdMS_TO_TICKS(delay_ms));
}

void set_leds_color(uint8_t r, uint8_t g, uint8_t b, uint8_t w) {
    // Set color for all LEDs
    for (int i = 0; i < EXAMPLE_LED_NUMBERS; i++) {
        int offset = i * 4;
        led_strip_pixels[offset + 0] = g;
        led_strip_pixels[offset + 1] = r;
        led_strip_pixels[offset + 2] = b;
        led_strip_pixels[offset + 3] = w;
    }
    
    // Update LEDs
    ESP_ERROR_CHECK(rmt_transmit(led_chan, led_encoder,
                                 led_strip_pixels, sizeof(led_strip_pixels),
                                 &tx_config));
    ESP_ERROR_CHECK(rmt_tx_wait_all_done(led_chan, portMAX_DELAY));
}

void init_button(void) {
    ESP_LOGI(TAG, "Configuring button on GPIO %d", BUTTON_GPIO);
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << BUTTON_GPIO),
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io_conf);
}

void init_led_strip(void) {
    ESP_LOGI(TAG, "Create RMT TX channel");
    rmt_tx_channel_config_t tx_chan_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .gpio_num = RMT_LED_STRIP_GPIO_NUM,
        .mem_block_symbols = 64,
        .resolution_hz = RMT_LED_STRIP_RESOLUTION_HZ,
        .trans_queue_depth = 4,
    };
    ESP_ERROR_CHECK(rmt_new_tx_channel(&tx_chan_config, &led_chan));

    ESP_LOGI(TAG, "Install LED strip encoder");
    led_strip_encoder_config_t encoder_config = {
        .resolution = RMT_LED_STRIP_RESOLUTION_HZ,
    };
    ESP_ERROR_CHECK(rmt_new_led_strip_encoder(&encoder_config, &led_encoder));
    ESP_ERROR_CHECK(rmt_enable(led_chan));
}

void turn_off_leds(void) {
    memset(led_strip_pixels, 0, sizeof(led_strip_pixels));
    ESP_ERROR_CHECK(rmt_transmit(led_chan, led_encoder,
                                led_strip_pixels, sizeof(led_strip_pixels),
                                &tx_config));
    ESP_ERROR_CHECK(rmt_tx_wait_all_done(led_chan, portMAX_DELAY));
}

bool check_button_press(void) {
    return gpio_get_level(BUTTON_GPIO) == 0;
}

void led_debug_task(void *pvParam)
{
    const char *TAG = "LED_DEBUG_TASK";
    while (true) {
        bool wifi_ok = wifi_manager_is_connected();       // is STA connected to Wi-Fi?
        bool ap_mode = wifi_manager_is_in_ap_mode();      // did we fail STA and fall back to AP?
        bool ws_ok   = websocket_manager_is_connected();  // is WebSocket connected?

        if (ap_mode) {
            // ==============================
            // (A) AP fallback => flash YELLOW @1Hz
            // ==============================
            ESP_LOGI(TAG, "Blinking YELLOW (fallback AP mode)");
            // Turn on (yellow = R+G)
            set_leds_color(255, 180, 0, 0);
            vTaskDelay(pdMS_TO_TICKS(500));
            // Turn off
            set_leds_color(0, 0, 0, 0);
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        else if (!wifi_ok) {
            // ==============================
            // (B) Still connecting to Wi-Fi => blink RED @1Hz
            // ==============================
            ESP_LOGI(TAG, "Blinking RED (Wi-Fi not connected)");
            // Turn on
            set_leds_color(255, 0, 0, 0);
            vTaskDelay(pdMS_TO_TICKS(500));
            // Turn off
            set_leds_color(0, 0, 0, 0);
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        else if (wifi_ok && !ws_ok) {
            // ==============================
            // (C) Wi-Fi connected, but WebSocket isn't => solid RED
            // ==============================
            ESP_LOGI(TAG, "Solid RED (Wi-Fi connected, WS not connected)");
            set_leds_color(255, 0, 0, 0);
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        else {
            // ==============================
            // (D) Wi-Fi + WS both connected => stop debug
            // ==============================
            ESP_LOGI(TAG, "All connected => stopping LED debug");
            // Optionally turn LEDs off or set them to some final color
            set_leds_color(0, 0, 0, 0);

            vTaskDelete(NULL); // kill this debug task
            return; // just in case
        }
    }
}