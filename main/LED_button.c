#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/rmt_tx.h"
#include "led_strip_encoder.h"
#include "LED_button.h"

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