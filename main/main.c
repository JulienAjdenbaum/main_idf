// main.c

#include <stdio.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h" // Include for EventBits_t

#include "wifi.h"
#include "websocket_manager.h"
#include "LED_button.h"
#include "audio_player.h"
#include "audio_record.h"
#include "tag_reader.h"

// Assuming s_wifi_event_group is declared globally in wifi.h or similar
// extern EventGroupHandle_t s_wifi_event_group;
// Assuming WIFI_CONNECTED_BIT and WIFI_FAIL_BIT are defined in wifi.h or similar
// extern const int WIFI_CONNECTED_BIT;
// extern const int WIFI_FAIL_BIT;

// Placeholder function declarations if not included via headers
// void init_led_strip(void);
// void led_debug_task(void *pvParameters);
// void init_button(void);
// void audio_player_init(void);
// void audio_record_init(void);
// void tag_reader_init(void);


void app_main(void)
{
    const TickType_t xDelay = pdMS_TO_TICKS(100); // 100ms delay

    ESP_LOGI("MAIN", "Starting app_main...");

    // 1) Initialize the LED strip immediately
    ESP_LOGI("MAIN", "Initializing LED strip...");
    init_led_strip();
    vTaskDelay(xDelay); // Small delay

    // 2) Start Wi-Fi initialization
    ESP_LOGI("MAIN", "Initializing Wi-Fi...");
    wifi_manager_init(); // This likely starts a task and returns quickly
    vTaskDelay(xDelay); // Small delay

    // 3) Create LED debug task (so we can watch Wi-Fi progress and blink accordingly)
    ESP_LOGI("MAIN", "Creating LED debug task...");
    xTaskCreate(led_debug_task, "led_debug_task", 2048, NULL, 5, NULL);
    vTaskDelay(xDelay); // Small delay, allow task scheduler potentially

    // 4) Wait for Wi-Fi connection result (STA success or fail)
    ESP_LOGI("MAIN", "Waiting for Wi-Fi connection result...");
    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE, // Clear bits on exit = No
        pdFALSE, // Wait for all bits = No (wait for either)
        portMAX_DELAY // Wait indefinitely
    );

    // Optional delay after waiting, before processing the result
    vTaskDelay(xDelay);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI("MAIN", "Wi-Fi Connected (STA mode). Starting other services...");

        ESP_LOGI("MAIN", "Initializing WebSocket Manager...");
        websocket_manager_init(); // Assumes this is non-blocking or starts its own task
        vTaskDelay(xDelay);

        ESP_LOGI("MAIN", "Initializing Button...");
        init_button();
        vTaskDelay(xDelay);

        ESP_LOGI("MAIN", "Initializing Audio Player...");
        audio_player_init(); // Assumes this is non-blocking or starts its own task
        vTaskDelay(xDelay);

        ESP_LOGI("MAIN", "Initializing Audio Recorder...");
        audio_record_init(); // Assumes this is non-blocking or starts its own task
        vTaskDelay(xDelay);

        ESP_LOGI("MAIN", "Initializing Tag Reader...");
        tag_reader_init(); // Assumes this is non-blocking or starts its own task
        vTaskDelay(xDelay);

        ESP_LOGI("MAIN", "All services initialized.");

    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGW("MAIN", "Wi-Fi Connection Failed (STA mode) -> fallback AP is active.");
        ESP_LOGW("MAIN", "Not starting network-dependent services.");
        // If needed, run AP-mode–only logic here after a small delay
        vTaskDelay(xDelay);
        // e.g., start_ap_mode_service();

    } else {
        // Should not happen with portMAX_DELAY unless event group was deleted?
        ESP_LOGE("MAIN", "Unexpected state after xEventGroupWaitBits");
    }

    // 5) Keep the main task alive (optional, could be deleted if app_main finishes)
    // This loop is often unnecessary if all work is done by other tasks.
    // If app_main exits, the FreeRTOS scheduler continues running other tasks.
    ESP_LOGI("MAIN", "Initialization complete. Main task entering idle loop.");
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(5000)); // Can be a longer delay here
        // Maybe print some status or check high-level system health
    }
}