// main.c - High-level entry point only

#include <stdio.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "wifi_manager.h"
#include "websocket_manager.h"
#include "audio_player.h"

static const char *TAG = "MAIN";

void app_main(void)
{
    ESP_LOGI(TAG, "Starting app_main...");

    // 1) Initialize Wi-Fi
    wifi_manager_init();

    // 2) Initialize the audio player (I2S + queue)
    if (audio_player_init() != ESP_OK) {
        ESP_LOGE(TAG, "Audio player init failed!");
        return;
    }

    // 3) Initialize WebSocket + start test task
    websocket_manager_init();
    
    // That’s it. Everything else runs in tasks or callbacks.
    // If you have other application logic, you can start more tasks here.
}
