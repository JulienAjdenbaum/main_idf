#include "audio_player.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "driver/i2s.h"
static const char *TAG = "AudioPlayer";
static QueueHandle_t audio_queue = NULL;

static void audio_task(void *param) {
    while (1) {
        audio_chunk_t chunk;
        if (xQueueReceive(audio_queue, &chunk, portMAX_DELAY)) {
            size_t bytes_written;
            esp_err_t err = i2s_write(I2S_NUM, chunk.data, chunk.length, &bytes_written, portMAX_DELAY);
            
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "I2S write error: %s", esp_err_to_name(err));
            }
            free(chunk.data);
        }
    }
    vTaskDelete(NULL);
}

esp_err_t audio_player_init(void) {
    // I2S configuration
    i2s_config_t i2s_config = {
        .mode = I2S_MODE_MASTER | I2S_MODE_TX,
        .sample_rate = 44100,  // Set to common base rate (will be overridden)
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .dma_buf_count = 16,  // Increased from 8
        .dma_buf_len = 512,   // Reduced from 1024
        .use_apll = true,
        .tx_desc_auto_clear = true,
        .fixed_mclk = true,
    };

    i2s_pin_config_t pin_config = {
        .bck_io_num = I2S_BCK_IO,
        .ws_io_num = I2S_WS_IO,
        .data_out_num = I2S_DO_IO,
        .data_in_num = I2S_DI_IO
    };

    // Initialize I2S
    esp_err_t ret = i2s_driver_install(I2S_NUM, &i2s_config, 0, NULL);
    if (ret != ESP_OK) return ret;
    
    ret = i2s_set_pin(I2S_NUM, &pin_config);
    if (ret != ESP_OK) return ret;

    // Create audio queue
    audio_queue = xQueueCreate(10, sizeof(audio_chunk_t));
    if (!audio_queue) return ESP_FAIL;

    // Start audio task
    xTaskCreatePinnedToCore(audio_task, "AudioTask", 4096*2, NULL, 5, NULL, 1);
    
    return ESP_OK;
}

void audio_player_play(uint8_t *data, size_t length) {
    audio_chunk_t chunk = {
        .data = data,
        .length = length
    };
    xQueueSend(audio_queue, &chunk, portMAX_DELAY);
}

void audio_player_shutdown(void) {
    i2s_driver_uninstall(I2S_NUM);
    vQueueDelete(audio_queue);
}

void audio_player_set_sample_rate(uint32_t sample_rate, uint16_t num_channels) {
    i2s_set_clk(I2S_NUM, 
               sample_rate,
               I2S_BITS_PER_SAMPLE_16BIT,
               num_channels > 1 ? I2S_CHANNEL_STEREO : I2S_CHANNEL_MONO);
} 