// main.c

#include <stdio.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "wifi.h"
#include "websocket_manager.h"
#include "LED_button.h"
#include "audio_player.h"
#include "audio_record.h"
#include "tag_reader.h"


void app_main(void)
{
    ESP_LOGI("MAIN", "Starting app_main...");

    // 1) Initialize the LED strip immediately
    init_led_strip();

    // 2) Start Wi-Fi
    ESP_LOGI("MAIN", "Initializing Wi-Fi...");
    wifi_manager_init();

    // 3) Create LED debug task (so we can watch Wi-Fi progress and blink accordingly)
    xTaskCreate(led_debug_task, "led_debug_task", 2048, NULL, 5, NULL);

    // 4) Wait for STA success or fail (optional if your code needs blocking)
    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE,
        pdFALSE,
        portMAX_DELAY
    );

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI("MAIN", "STA connected. Starting other services...");

        // If you also want to show the WebSocket connection progress right away
        websocket_manager_init();

        // You can init other stuff here
        init_button();
        audio_player_init();
        audio_record_init();
        tag_reader_init();
        // ...
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGW("MAIN", "STA failed -> fallback AP is active. Not starting the rest...");
        // If needed, run AP-mode–only logic here
    }

    // 5) Keep the main task alive
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
