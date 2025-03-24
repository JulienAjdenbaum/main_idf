#include <stdio.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tag_reader.h"

#include "wifi_manager.h"        // your Wi-Fi code
#include "websocket_manager.h"   // the code above
#include "audio_player.h"        // queue-based player
#include "audio_record.h"        // audio recording
static const char *TAG = "MAIN";

void app_main(void)
{
    ESP_LOGI(TAG, "Starting app_main...");

    wifi_manager_init();
    audio_player_init();
    // websocket_manager_init();
    audio_record_init();
    tag_reader_init();
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
