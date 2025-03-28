// audio_player.c
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
// static void i2s_monitor_task(void *arg);
#define DEFAULT_VREF    1100    // For calibration (if needed)

static float s_volume = 1.0f;

void audio_player_set_volume(float vol)
{
    // Clamp vol between 0.0f and 1.0f if necessary
    if (vol < 0.0f) vol = 0.0f;
    if (vol > 1.0f) vol = 1.0f;
    s_volume = vol;
}

float audio_player_get_volume(void)
{
    return s_volume;
}

esp_err_t audio_player_init(void)
{
    i2s_config_t i2s_config = {
        .mode                 = I2S_MODE_MASTER | I2S_MODE_TX,
        .sample_rate          = SAMPLE_RATE,
        .bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format       = I2S_CHANNEL_FMT_RIGHT_LEFT,  // stereo
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .dma_buf_count        = DMA_BUF_COUNT,
        .dma_buf_len          = DMA_BUF_LEN,
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

    // Put all buffer indexes into empty queue
    for (int i = 0; i < NUM_AUDIO_BUFFERS; i++) {
        xQueueSend(s_empty_queue, &i, 0);
    }

    // Start tasks
    xTaskCreatePinnedToCore(audio_task, "audioTask", 4096, NULL, 7, NULL, 1);
    xTaskCreatePinnedToCore(audio_monitor_task, "audioMonitor", 4096, NULL, 4, NULL, 1);
    xTaskCreate(
        volume_task,     // Task code
        "volume_task",   // Name for debugging
        4096,            // Stack size (bytes)
        NULL,            // Task parameter
        5,               // Priority
        NULL             // Task handle (not used here)
    );
    // xTaskCreatePinnedToCore(i2s_monitor_task, "i2sMonitor", 2048, NULL, 4, NULL, 1);

    ESP_LOGI(TAG, "Audio player initialized. sample_rate=%d", SAMPLE_RATE);
    return ESP_OK;
}

static void volume_task(void *param)
{
    // If there is no potentiometer in use, just set the volume to 1.0 forever
    if (POT_PIN == -1) {
        ESP_LOGI(TAG, "No pot detected (POT_PIN == -1). Volume set to 1.0f");
        while (1) {
            audio_player_set_volume(1.0f);
            vTaskDelay(pdMS_TO_TICKS(200));
        }
        // vTaskDelete(NULL); // unreachable, but here for completeness
    }

    // Otherwise, configure ADC to read from the pot
    adc1_config_width(ADC_WIDTH_BIT_12);               // 0..4095
    adc1_config_channel_atten(POT_PIN, ADC_ATTEN_DB_11); // up to ~3.3V

    while (1) {
        // Read the raw ADC value
        int raw = adc1_get_raw(POT_PIN);

        // 0..4095 => 0.0..1.0
        float normalized = (float)raw / 4095.0f;

        // Option A: Simple square mapping (makes the pot “quieter” near 0)
        // float vol = normalized * normalized;

        // Option B: Slightly stronger curve
        // float vol = powf(normalized, 3.0f);

        // Option C: Straight linear (original)
        // float vol = normalized;

        float vol = normalized * normalized; // pick whichever feels best

        // Update the actual audio volume
        audio_player_set_volume(vol);

        // Optional debug:
        ESP_LOGI(TAG, "ADC raw: %d => normalized=%.3f => finalVolume=%.3f",
                 raw, normalized, vol);

        // Don’t spam the ADC or the log
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    // If it ever leaves the loop
    vTaskDelete(NULL);
}


static void audio_task(void *param)
{
    ESP_LOGI(TAG, "audio_task started");
    while (1) {
        int buf_idx;
        if (xQueueReceive(s_ready_queue, &buf_idx, portMAX_DELAY) == pdTRUE) {
            s_last_audio_time = esp_timer_get_time();
            
            if (buf_idx < 0 || buf_idx >= NUM_AUDIO_BUFFERS) {
                ESP_LOGE(TAG, "Invalid buffer index: %d", buf_idx);
                continue;
            }
            audio_buffer_t *buf = &s_buffers[buf_idx];

            size_t bytes_written = 0;
            esp_err_t err = i2s_write(I2S_NUM, buf->data, buf->length,
                                      &bytes_written, portMAX_DELAY);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "i2s_write failed: %s", esp_err_to_name(err));
            }

            // return buffer to empty queue
            xQueueSend(s_empty_queue, &buf_idx, portMAX_DELAY);
        }
    }
    vTaskDelete(NULL);
}



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

audio_buffer_t *audio_player_get_buffer_blocking(void)
{
    int idx;
    if (xQueueReceive(s_empty_queue, &idx, portMAX_DELAY) == pdTRUE) {
        s_buffers[idx].length = 0; // reset length
        return &s_buffers[idx];
    }
    // Should never get here if portMAX_DELAY
    return NULL;
}

void audio_player_submit_buffer(audio_buffer_t *buf)
{
    s_last_audio_time = esp_timer_get_time();
    int idx = (int)(buf - &s_buffers[0]);
    if (idx < 0 || idx >= NUM_AUDIO_BUFFERS) {
        ESP_LOGE(TAG, "Invalid buffer pointer in submit_buffer");
        return;
    }
    xQueueSend(s_ready_queue, &idx, portMAX_DELAY);
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
    // Convert microseconds => milliseconds
    int64_t now_ms       = esp_timer_get_time() / 1000;
    int64_t last_time_ms = s_last_audio_time / 1000;

    // If time since last audio submission is <= 100 ms
    if ( (now_ms - last_time_ms) <= 500 ) {
        return true;
    } else {
        return false;
    }
}



// static void i2s_monitor_task(void *arg)
// {
//     while (1) {
//         size_t bytes_in_i2s_tx_buffer = 0;
//         esp_err_t ret = i2s_write_expand(I2S_NUM_0, NULL, 0, 0, &bytes_in_i2s_tx_buffer);

//         if (ret == ESP_OK) {
//             ESP_LOGI("I2S_MONITOR", "Buffered data: %d bytes", bytes_in_i2s_tx_buffer);
//         } else {
//             ESP_LOGE("I2S_MONITOR", "Failed to get I2S buffer size: %s", esp_err_to_name(ret));
//         }

//         vTaskDelay(pdMS_TO_TICKS(1000));
//     }
// }