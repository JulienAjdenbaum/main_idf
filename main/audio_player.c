// audio_player.c

#include "audio_player.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "driver/i2s.h"
#include "esp_log.h"

static const char *TAG = "AudioPlayer";

// Our pool of buffers:
static audio_buffer_t s_buffers[NUM_AUDIO_BUFFERS];

// Two queues:
//   s_empty_queue: holds indices of buffers available to be filled
//   s_ready_queue: holds indices of buffers with data ready to play
static QueueHandle_t s_empty_queue = NULL;
static QueueHandle_t s_ready_queue = NULL;


// Forward declarations
static void audio_task(void *param);
static void audio_monitor_task(void *param);

esp_err_t audio_player_init(void)
{
    // 1) Configure and install I2S
    i2s_config_t i2s_config = {
        .mode                 = I2S_MODE_MASTER | I2S_MODE_TX,
        .sample_rate          = 44100,
        .bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format       = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .dma_buf_count        = 8,
        .dma_buf_len          = 1024,
        .use_apll            = false,
        .intr_alloc_flags    = 0,
        .tx_desc_auto_clear  = true,
        .fixed_mclk          = 0
    };

    i2s_pin_config_t pin_config = {
        .bck_io_num     = I2S_BCK_IO,
        .ws_io_num      = I2S_WS_IO,
        .data_out_num   = I2S_DO_IO,
        .data_in_num    = I2S_DI_IO
    };

    esp_err_t ret = i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_driver_install failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ret = i2s_set_pin(I2S_NUM_0, &pin_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_set_pin failed: %s", esp_err_to_name(ret));
        i2s_driver_uninstall(I2S_NUM_0);
        return ret;
    }

    // 2) Create the two queues
    s_empty_queue = xQueueCreate(NUM_AUDIO_BUFFERS, sizeof(int));
    s_ready_queue = xQueueCreate(NUM_AUDIO_BUFFERS, sizeof(int));
    if (!s_empty_queue || !s_ready_queue) {
        ESP_LOGE(TAG, "Failed to create audio queues");
        return ESP_FAIL;
    }

    // Put all buffer indices into the "empty" queue initially
    for (int i = 0; i < NUM_AUDIO_BUFFERS; i++) {
        xQueueSend(s_empty_queue, &i, 0);
    }

    // 3) Create tasks
    xTaskCreatePinnedToCore(audio_task, "audioTask", 4096, NULL, 5, NULL, 1);
    xTaskCreatePinnedToCore(audio_monitor_task, "audioMonitor", 2048, NULL, 4, NULL, 1);

    ESP_LOGI(TAG, "Audio player initialized.");
    return ESP_OK;
}

/**
 * The main audio task:
 *   - waits for a filled buffer index from s_ready_queue,
 *   - writes it to I2S,
 *   - returns that index to s_empty_queue.
 */
static void audio_task(void *param)
{
    ESP_LOGI(TAG, "Audio task started");
    while (1) {
        int buf_idx;
        // Wait indefinitely for a filled buffer index
        if (xQueueReceive(s_ready_queue, &buf_idx, portMAX_DELAY) == pdTRUE) {
            if (buf_idx < 0 || buf_idx >= NUM_AUDIO_BUFFERS) {
                ESP_LOGE(TAG, "Invalid buffer index: %d", buf_idx);
                continue;
            }
            audio_buffer_t *buf = &s_buffers[buf_idx];

            // Write out to I2S
            size_t written = 0;
            esp_err_t err = i2s_write(I2S_NUM_0, buf->data, buf->length,
                                      &written, portMAX_DELAY);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "I2S write error: %s", esp_err_to_name(err));
            }

            // Return this buffer to the empty queue
            xQueueSend(s_empty_queue, &buf_idx, portMAX_DELAY);
        }
    }
    vTaskDelete(NULL);
}

/**
 * A small optional monitor task that logs queue usage.
 */
static void audio_monitor_task(void *param)
{
    while (1) {
        UBaseType_t empty_count = uxQueueMessagesWaiting(s_empty_queue);
        UBaseType_t ready_count = uxQueueMessagesWaiting(s_ready_queue);
        ESP_LOGI(TAG, "[Audio Monitor] empty=%u, ready=%u",
                 (unsigned)empty_count, (unsigned)ready_count);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

void audio_player_shutdown(void)
{
    i2s_driver_uninstall(I2S_NUM_0);

    if (s_empty_queue) {
        vQueueDelete(s_empty_queue);
        s_empty_queue = NULL;
    }
    if (s_ready_queue) {
        vQueueDelete(s_ready_queue);
        s_ready_queue = NULL;
    }
    ESP_LOGI(TAG, "Audio player shut down.");
}

audio_buffer_t* audio_player_get_buffer_blocking(void)
{
    int idx;
    if (xQueueReceive(s_empty_queue, &idx, portMAX_DELAY) == pdTRUE) {
        if (idx >= 0 && idx < NUM_AUDIO_BUFFERS) {
            // Clear out length to 0 just in case
            s_buffers[idx].length = 0;
            return &s_buffers[idx];
        }
    }
    return NULL; // should not happen if portMAX_DELAY
}

void audio_player_submit_buffer(audio_buffer_t *buf)
{
    // Derive buffer index
    int idx = (int)(buf - &s_buffers[0]);
    if (idx < 0 || idx >= NUM_AUDIO_BUFFERS) {
        ESP_LOGE(TAG, "Invalid audio_buffer_t pointer in submit_buffer!");
        return;
    }
    xQueueSend(s_ready_queue, &idx, portMAX_DELAY);
}

void audio_player_set_sample_rate(uint32_t sample_rate, uint16_t num_channels)
{
    i2s_channel_t chan = (num_channels > 1) ? I2S_CHANNEL_STEREO : I2S_CHANNEL_MONO;
    i2s_set_clk(I2S_NUM_0, sample_rate, I2S_BITS_PER_SAMPLE_16BIT, chan);
    ESP_LOGI(TAG, "I2S sample rate set to %u Hz, %s",
             sample_rate,
             (chan == I2S_CHANNEL_STEREO) ? "stereo" : "mono");
}
