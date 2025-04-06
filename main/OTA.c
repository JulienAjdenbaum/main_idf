#include "OTA.h"
#include "audio_player.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_http_client.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "websocket_manager.h"
#include "esp_heap_caps.h"
#include "LED_button.h"



static const char *TAG = "OTA";

static void ota_task(void *arg)
{

    set_leds_color(0, 0, 255, 0);

    ESP_LOGI(TAG, "LEDs set to blue");


    // Convert the passed-in URL (arg) into a local buffer
    char url[256];
    snprintf(url, sizeof(url), "%s", (char*)arg);



    // 2) Stop other tasks (WebSocket, ringbuffer, etc.) to free memory
    websocket_manager_request_shutdown();
    vTaskDelay(pdMS_TO_TICKS(500)); // allow tasks to exit
    ESP_LOGI(TAG, "Stopping WebSocket to free heap before OTA...");
    websocket_manager_stop();

    // (Optional) Wait a bit for everything to settle
    vTaskDelay(pdMS_TO_TICKS(200));

    // 3) Proceed with OTA
    ESP_LOGI(TAG, "Proceeding with OTA download...");

    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = 10000,
        .transport_type = HTTP_TRANSPORT_OVER_TCP,
        // Optional extras:
        .buffer_size = 1024,
        .buffer_size_tx = 512,
        .user_agent = "ESP32 OTA Client",
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to init http client for OTA");
        goto OTA_FAIL;
    }

    // 4) Open HTTP connection
    esp_err_t open_err = esp_http_client_open(client, 0);
    if (open_err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open http connection for OTA: %s", esp_err_to_name(open_err));
        esp_http_client_cleanup(client);
        goto OTA_FAIL;
    }

    // IMPORTANT: Explicitly fetch headers (some IDF configurations need this)
    esp_err_t fetch_err = esp_http_client_fetch_headers(client);
    if (fetch_err < 0) {
        ESP_LOGE(TAG, "esp_http_client_fetch_headers failed: %s", esp_err_to_name(fetch_err));
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        goto OTA_FAIL;
    }

    // You can now check status code and content length if desired
    int status_code = esp_http_client_get_status_code(client);
    if (status_code != 200) {
        ESP_LOGE(TAG, "Invalid HTTP status code: %d", status_code);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        goto OTA_FAIL;
    }
    int content_length = esp_http_client_get_content_length(client);
    ESP_LOGI(TAG, "HTTP status code: %d, content length: %d", status_code, content_length);

    // 5) Prepare for writing to an OTA partition
    esp_ota_handle_t update_handle = 0;
    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (!update_partition) {
        ESP_LOGE(TAG, "Failed to find OTA partition");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        goto OTA_FAIL;
    }

    ESP_LOGI(TAG, "Free heap before OTA: %u", heap_caps_get_free_size(MALLOC_CAP_DEFAULT));
    ESP_LOGI(TAG, "Writing to OTA partition subtype %d at offset 0x%x",
             update_partition->subtype, update_partition->address);

    if (esp_ota_begin(update_partition, 0, &update_handle) != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        goto OTA_FAIL;
    }

    // 6) Read the binary from the server in chunks
    char ota_buf[1024];
    int data_read;
    size_t total_read = 0;

    while ((data_read = esp_http_client_read(client, ota_buf, sizeof(ota_buf))) > 0) {
        total_read += data_read;
        // ESP_LOGI(TAG, "Downloaded %d bytes so far...", total_read);

        if (esp_ota_write(update_handle, (const void *)ota_buf, data_read) != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed");
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            esp_ota_abort(update_handle);
            goto OTA_FAIL;
        }
        ESP_LOGI(TAG, "Wrote %d bytes to OTA partition", data_read);
    }

    if (data_read < 0) {
        // data_read is an error code (negative)
        ESP_LOGE(TAG, "Error reading data: %s", esp_err_to_name(data_read));
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        esp_ota_abort(update_handle);
        goto OTA_FAIL;
    }

    // Optionally ensure we got a minimum size
    if (total_read < 1024) { // or any threshold that makes sense for your firmware
        ESP_LOGE(TAG, "Downloaded file too small: %d bytes", total_read);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        esp_ota_abort(update_handle);
        goto OTA_FAIL;
    }

    ESP_LOGI(TAG, "OTA write complete. Downloaded %d bytes. Closing HTTP client...", total_read);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    // 7) End the OTA update
    if (esp_ota_end(update_handle) != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed!");
        goto OTA_FAIL;
    }

    // 8) Set the boot partition
    if (esp_ota_set_boot_partition(update_partition) != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed!");
        goto OTA_FAIL;
    }

    ESP_LOGI(TAG, "OTA succeeded! Rebooting...");
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart(); // Reboot into new firmware

OTA_FAIL:
    ESP_LOGE(TAG, "OTA failed or ended early.");
    vTaskDelete(NULL); // Clean up the task
}

void ota_start_update(const char *url)
{
    if (!url) {
        ESP_LOGW(TAG, "ota_start_update called with NULL url => ignoring");
        return;
    }
    char *url_copy = strdup(url);
    if (!url_copy) {
        ESP_LOGE(TAG, "Failed to allocate memory for URL copy");
        return;
    }

    xTaskCreate(ota_task,
                "ota_task",
                8192,
                url_copy,
                5,
                NULL);
}

void ota_send_device_version(void)
{
    if (!websocket_manager_is_connected()) {
        ESP_LOGW(TAG, "WebSocket not connected; cannot send version info");
        return;
    }

    char message[128];
    int len = snprintf(message, sizeof(message), "%s,%s", 
                       OTA_HARDWARE_VERSION, OTA_SOFTWARE_VERSION);

    if (len <= 0 || len >= sizeof(message)) {
        ESP_LOGE(TAG, "Failed to format version string");
        return;
    }

    // Create a buffer with 0x04 prefix
    uint8_t buffer[1 + sizeof(message)];
    buffer[0] = 0x04;
    memcpy(&buffer[1], message, len);

    int sent = websocket_manager_send_bin((const char *)buffer, len + 1);
}
