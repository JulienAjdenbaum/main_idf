#include "audio_player.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "driver/i2s.h"

static const char *TAG = "AudioPlayer";

static QueueHandle_t audio_queue = NULL;

static void audio_task(void *param)
{
    while (1)
    {
        audio_chunk_t chunk;
        if (xQueueReceive(audio_queue, &chunk, portMAX_DELAY))
        {
            size_t bytes_written = 0;
            esp_err_t err = i2s_write(I2S_NUM_0, chunk.data, chunk.length,
                                      &bytes_written, portMAX_DELAY);
            if (err != ESP_OK)
            {
                ESP_LOGE(TAG, "I2S write error: %s", esp_err_to_name(err));
            }
            // Free the data after it’s been written out
            free(chunk.data);
        }
    }
    vTaskDelete(NULL);
}

esp_err_t audio_player_init(void)
{
    i2s_config_t i2s_config = {
        .mode = I2S_MODE_MASTER | I2S_MODE_TX,
        .sample_rate = 44100,                        // Default; can be changed later
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT, // 2 channels
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .dma_buf_count = 16,
        .dma_buf_len = 1024,
        .use_apll = false,
        .tx_desc_auto_clear = true, // optional depending on IDF
        .intr_alloc_flags = 0       // default interrupt priority
    };

    i2s_pin_config_t pin_config = {
        .bck_io_num = I2S_BCK_IO,
        .ws_io_num = I2S_WS_IO,
        .data_out_num = I2S_DO_IO,
        .data_in_num = I2S_DI_IO
    };

    // Install and start I2S driver
    esp_err_t ret = i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_driver_install failed");
        return ret;
    }

    // Set the pins
    ret = i2s_set_pin(I2S_NUM_0, &pin_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_set_pin failed");
        return ret;
    }

    // Create the audio queue
    audio_queue = xQueueCreate(10, sizeof(audio_chunk_t));
    if (!audio_queue) {
        ESP_LOGE(TAG, "Failed to create audio queue");
        return ESP_FAIL;
    }

    // Start the task that will do the actual i2s writes
    xTaskCreatePinnedToCore(
        audio_task,          // Task function
        "AudioTask",         // Name
        4096 * 2,            // Stack size
        NULL,                // Param
        5,                   // Priority
        NULL,                // Out handle
        1                    // Core
    );

    return ESP_OK;
}

void audio_player_play(uint8_t *data, size_t length)
{
    audio_chunk_t chunk = {
        .data = data,
        .length = length
    };
    // Push the audio buffer to the queue for i2s_write in the other task
    xQueueSend(audio_queue, &chunk, portMAX_DELAY);
}

void audio_player_shutdown(void)
{
    i2s_driver_uninstall(I2S_NUM_0);
    if (audio_queue) {
        vQueueDelete(audio_queue);
        audio_queue = NULL;
    }
}

void audio_player_set_sample_rate(uint32_t sample_rate, uint16_t num_channels)
{
    // Convert num_channels to I2S_CHANNEL_MONO or I2S_CHANNEL_STEREO
    i2s_channel_t chan = (num_channels > 1) ? I2S_CHANNEL_STEREO : I2S_CHANNEL_MONO;
    i2s_set_clk(I2S_NUM_0, sample_rate, I2S_BITS_PER_SAMPLE_16BIT, chan);
    ESP_LOGI(TAG, "Audio sample rate set to %u Hz, %s",
             sample_rate,
             (chan == I2S_CHANNEL_STEREO) ? "stereo" : "mono");
}
