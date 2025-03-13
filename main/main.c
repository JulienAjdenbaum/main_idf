#include <stdio.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "wifi_manager.h"
#include "websocket_manager.h"
#include "audio_player.h"
#include "audio_record.h"

static const char *TAG = "MAIN";

void app_main(void)
{
    ESP_LOGI(TAG, "Starting app_main...");

    // 1) Start Wi-Fi
    wifi_manager_init();

    // 2) Initialize audio playback
    audio_player_init();

    // 3) (Optional) Initialize microphone recording
    audio_record_init();

    // Your app's main loop
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
