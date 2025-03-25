// main.c
#include <stdio.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "wifi.h"
#include "audio_player.h"
#include "audio_record.h"
#include "LED_button.h"
#include "websocket_manager.h"
#include "tag_reader.h"

static const char *TAG = "MAIN";

void app_main(void)
{
    ESP_LOGI(TAG, "Starting app_main...");

    // 1) Start Wi-Fi
    wifi_manager_init();

    // 2) Wait for STA success or fail
    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE,
        pdFALSE,
        portMAX_DELAY
    );

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "STA connected! Starting other services...");
        init_led_strip();
        init_button();
        audio_player_init();
        websocket_manager_init();
        audio_record_init();
        tag_reader_init();
        // ...
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGW(TAG, "STA failed -> fallback AP is active. Not starting the rest...");
        // If needed, run AP-mode–only logic here
    }

    // 3) Keep the main task alive (or do other app logic)
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
