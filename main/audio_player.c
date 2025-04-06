// audio_player.c

#include "esp_task_wdt.h"
#include "audio_player.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "driver/i2s.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_adc_cal.h"
#include "driver/adc.h"
#include "pins.h"

static const char *TAG = "AudioPlayer";

static audio_buffer_t s_buffers[NUM_AUDIO_BUFFERS];
static QueueHandle_t  s_empty_queue = NULL;
static QueueHandle_t  s_ready_queue = NULL;

static int64_t s_last_audio_time = 0;

static void audio_task(void *param);
static void audio_monitor_task(void *param);
static void volume_task(void *param);

#define DEFAULT_VREF 1100

static float s_volume = 1.0f;

void audio_player_set_volume(float vol)
{
    if (vol < 0.0f) vol = 0.0f;
    if (vol > 1.0f) vol = 1.0f;
    s_volume = (1.0f - vol) * 0.2f;
}

float audio_player_get_volume(void)
{
    return s_volume;
}

esp_err_t audio_player_init(void)
{
    i2s_config_t i2s_config = {
        .mode = I2S_MODE_MASTER | I2S_MODE_TX,
        .sample_rate = SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .dma_buf_count = DMA_BUF_COUNT,
        .dma_buf_len = DMA_BUF_LEN,
        .use_apll = false,
        .intr_alloc_flags = 0,
        .tx_desc_auto_clear = true,
        .fixed_mclk = 0
    };

    i2s_pin_config_t pin_config = {
        .bck_io_num = I2S_BCK_IO,
        .ws_io_num = I2S_WS_IO,
        .data_out_num = I2S_DO_IO,
        .data_in_num = I2S_DI_IO
    };

    esp_err_t ret = i2s_driver_install(I2S_NUM, &i2s_config, 0, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_driver_install failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = i2s_set_pin(I2S_NUM, &pin_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_set_pin failed: %s", esp_err_to_name(ret));
        i2s_driver_uninstall(I2S_NUM);
        return ret;
    }

    // Create the empty/ready queues
    s_empty_queue = xQueueCreate(NUM_AUDIO_BUFFERS, sizeof(int));
    s_ready_queue = xQueueCreate(NUM_AUDIO_BUFFERS, sizeof(int));
    if (!s_empty_queue || !s_ready_queue) {
        ESP_LOGE(TAG, "Failed to create audio queues");
        return ESP_FAIL;
    }

    // Put all buffer indexes into the empty queue
    for (int i = 0; i < NUM_AUDIO_BUFFERS; i++) {
        xQueueSend(s_empty_queue, &i, 0);
    }

    // Start tasks
    xTaskCreatePinnedToCore(audio_task, "audioTask", 4096, NULL, 3, NULL, 1);
    xTaskCreatePinnedToCore(audio_monitor_task, "audioMonitor", 2048, NULL, 4, NULL, 1);
    xTaskCreatePinnedToCore(volume_task, "volume_task", 2048, NULL, 5, NULL, 1);

    ESP_LOGI(TAG, "Audio player initialized. sample_rate=%d", SAMPLE_RATE);
    return ESP_OK;
}

static void volume_task(void *param)
{
    if (POT_PIN == -1) {
        ESP_LOGI(TAG, "No pot. Volume fixed at 1.0f");
        while (1) {
            audio_player_set_volume(1.0f);
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }

    adc1_config_width(ADC_WIDTH_BIT_12);
    adc1_config_channel_atten(POT_PIN, ADC_ATTEN_DB_11);

    while (1) {
        int raw = adc1_get_raw(POT_PIN);
        float normalized = (float)raw / 4095.0f;
        float vol = (normalized * normalized); // Or another mapping
        audio_player_set_volume(vol);
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}
static void audio_task(void *param)
{
    ESP_LOGI(TAG, "audio_task started");
    while (1) {
        // esp_task_wdt_reset();
        int buf_idx;
        // Wait up to 50ms for the next ready buffer
        if (xQueueReceive(s_ready_queue, &buf_idx, pdMS_TO_TICKS(50)) == pdTRUE) {
            // esp_task_wdt_reset();
            s_last_audio_time = esp_timer_get_time();

            if (buf_idx < 0 || buf_idx >= NUM_AUDIO_BUFFERS) {
                ESP_LOGE(TAG, "Invalid buffer index: %d", buf_idx);
                continue;
            }
            audio_buffer_t *buf = &s_buffers[buf_idx];

            // Write the buffer to I2S in small chunks to avoid long blocking
            uint8_t *ptr   = (uint8_t *)buf->data;
            size_t   remain = buf->length;
            size_t chunks_processed = 0;
            while (remain > 0) {
                size_t written = 0;
                size_t chunk_size = (remain > 1024) ? 1024 : remain; // Process in 1KB chunks
                esp_err_t err = i2s_write(I2S_NUM, ptr, chunk_size, &written, pdMS_TO_TICKS(20));

                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "i2s_write error => %s", esp_err_to_name(err));
                    break;  // stop trying to write the rest
                }
                if (written == 0) {
                    // If we wrote zero, it means i2s_write() timed out
                    ESP_LOGW(TAG, "i2s_write wrote 0 => skipping remainder");
                    break;
                }
                ptr    += written;
                remain -= written;
                if (remain > 0) {
                    vTaskDelay(pdMS_TO_TICKS(1));
                }
                if ((chunks_processed++ % 2) == 0) {
                    taskYIELD();
                }
            }

            // Return the buffer to empty queue (short timeout again)
            if (xQueueSend(s_empty_queue, &buf_idx, pdMS_TO_TICKS(20)) != pdTRUE) {
                ESP_LOGW(TAG, "Failed to return buffer to empty queue!");
            }
        }
        // If xQueueReceive timed out, we just loop again.

        // **Important**: Let other tasks and the watchdog run
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    vTaskDelete(NULL);
}


static void audio_monitor_task(void *param)
{
    while (1) {
        UBaseType_t empty_count = uxQueueMessagesWaiting(s_empty_queue);
        UBaseType_t ready_count = uxQueueMessagesWaiting(s_ready_queue);
        // Debug info
        // ESP_LOGI(TAG, "[Audio Monitor] empty=%u, ready=%u",
        //          (unsigned)empty_count, (unsigned)ready_count);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

audio_buffer_t *audio_player_get_buffer_blocking(void)
{
    int idx;
    BaseType_t ret = xQueueReceive(s_empty_queue, &idx, pdMS_TO_TICKS(20)); // Reduce timeout
    
    if (ret == pdTRUE) {
        return &s_buffers[idx];
    }
    
    // Add more frequent yielding
    taskYIELD();
    vTaskDelay(pdMS_TO_TICKS(2));
    return NULL;
}

bool audio_player_submit_buffer(audio_buffer_t *buf)
{
    int idx = (int)(buf - &s_buffers[0]);
    if (idx < 0 || idx >= NUM_AUDIO_BUFFERS) {
        ESP_LOGE(TAG, "Invalid buffer pointer in submit_buffer");
        return false;
    }

    // e.g. short timeout. If full, skip. 
    if (xQueueSend(s_ready_queue, &idx, pdMS_TO_TICKS(5)) != pdTRUE) {
        ESP_LOGW(TAG, "ready_queue is full; skipping this buffer!");
        return false;
    }
    return true;
}

void audio_player_set_sample_rate(uint32_t sample_rate, uint16_t num_channels)
{
    i2s_channel_t chan = (num_channels > 1) ? I2S_CHANNEL_STEREO : I2S_CHANNEL_MONO;
    i2s_set_clk(I2S_NUM, sample_rate, I2S_BITS_PER_SAMPLE_16BIT, chan);
    ESP_LOGI(TAG, "Audio sample rate updated => %u Hz, %s",
             sample_rate, (chan == I2S_CHANNEL_STEREO) ? "stereo" : "mono");
}

void audio_player_shutdown(void)
{
    i2s_driver_uninstall(I2S_NUM);
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

bool audio_player_is_playing(void)
{
    int64_t now_ms       = esp_timer_get_time() / 1000;
    int64_t last_time_ms = s_last_audio_time / 1000;
    // If time since last audio is <= 500ms => considered “playing”
    return ((now_ms - last_time_ms) <= 500);
}
