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

/* ---- playback‑state machine ------------------------------------------ */
typedef enum {
    AUD_STATE_IDLE,        // no data yet
    AUD_STATE_BUFFERING,   // filling queues
    AUD_STATE_PLAYING,     // DMA continuously feeding the codec
    AUD_STATE_UNDERRUN     // DMA starved – waiting for new data
} audio_state_t;

static volatile audio_state_t s_play_state = AUD_STATE_IDLE;

/* thresholds (bytes in your ready queue ≈ AUDIO_BUFFER_SIZE = 512)       */
#define START_PLAY_THRESHOLD   (AUDIO_BUFFER_SIZE * 3)   // ≈120 ms
#define PAUSE_PLAY_THRESHOLD   (AUDIO_BUFFER_SIZE * 1)   // ≈40 ms
#define PCM_TIMEOUT_MS         300                       // net‑hiccup guard

/* I2S event queue for underrun detection */
static QueueHandle_t  s_i2s_evt_q = NULL;
static TaskHandle_t   s_dma_evt_task = NULL;

/* ───────────────── dma_evt_task – watch I²S event queue ───────────────── */
/* ───────────────── dma_evt_task – watch I²S event queue ───────────────── */
static void dma_evt_task(void *param)
{
    i2s_event_t evt;

    while (1) {
        if (xQueueReceive(s_i2s_evt_q, &evt, portMAX_DELAY) == pdTRUE) {
            switch (evt.type) {

            case I2S_EVENT_DMA_ERROR:          /* DMA starved → UNDERRUN */
                s_play_state = AUD_STATE_UNDERRUN;
                ESP_EARLY_LOGW(TAG, "I²S DMA_ERROR → entering UNDERRUN");
                break;

            /* **DON’T** touch s_last_audio_time here – it masks real stalls */
            case I2S_EVENT_TX_DONE:
            default:
                break;
            }
        }
    }
}


void audio_player_set_volume(float vol)
{
    if (vol < 0.0f) vol = 0.0f;
    if (vol > 1.0f) vol = 1.0f;
    s_volume = (1.0f - vol) * 0.08f;
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

    esp_err_t ret = i2s_driver_install(I2S_NUM, &i2s_config,
                                   8,                // <- queue depth
                                   &s_i2s_evt_q);    // <- NEW: event queue
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
    xTaskCreatePinnedToCore(dma_evt_task, "dma_evt", 2048,
                        NULL, 4, &s_dma_evt_task, 1);
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
    s_play_state = AUD_STATE_IDLE;

    while (1) {

        /* ---------- ①  determine queue fill & time since last DMA ---------- */
        UBaseType_t ready_cnt   = uxQueueMessagesWaiting(s_ready_queue);
        size_t      ready_bytes = ready_cnt * AUDIO_BUFFER_SIZE;

        int64_t now_us         = esp_timer_get_time();
        int64_t since_last_ms  = (now_us - s_last_audio_time) / 1000;

        /* ---------- ②  state‑machine transitions -------------------------- */
        switch (s_play_state) {

            case AUD_STATE_IDLE:
            case AUD_STATE_BUFFERING:
            case AUD_STATE_UNDERRUN:
                if (ready_bytes >= START_PLAY_THRESHOLD) {
                    s_play_state = AUD_STATE_PLAYING;
                    ESP_LOGI(TAG, "[PLAY] start – buffer primed (%u B)", (unsigned)ready_bytes);
                }
                break;

            case AUD_STATE_PLAYING: {
                bool buffer_empty   = (ready_bytes == 0);           /* not ≤ 512   */
                bool writer_stalled = (since_last_ms > PCM_TIMEOUT_MS);

                if (buffer_empty && writer_stalled) {               /* need both   */
                    s_play_state = AUD_STATE_UNDERRUN;
                    ESP_LOGW(TAG, "[PLAY] real underrun (0 B, %lld ms)", since_last_ms);
                }
                break;
            }
        }
        /* ---------- ③  if not PLAYING just sleep a little ----------------- */
        if (s_play_state != AUD_STATE_PLAYING) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        /* ---------- ④  dequeue one buffer and push it to I²S -------------- */
        int buf_idx;
        if (xQueueReceive(s_ready_queue, &buf_idx, pdMS_TO_TICKS(10)) != pdTRUE) {
            /* shouldn’t happen often while PLAYING, but guard anyway */
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }

        if (buf_idx < 0 || buf_idx >= NUM_AUDIO_BUFFERS) {
            ESP_LOGE(TAG, "Invalid buffer index: %d", buf_idx);
            continue;
        }

        audio_buffer_t *buf = &s_buffers[buf_idx];
        uint8_t *ptr        = (uint8_t *)buf->data;
        size_t   remain     = buf->length;

        while (remain > 0) {
            size_t written     = 0;
            size_t chunk       = (remain > 1024) ? 1024 : remain;

            esp_err_t err = i2s_write(I2S_NUM, ptr, chunk,
                                      &written, pdMS_TO_TICKS(60));

            if (err != ESP_OK || written == 0) {
                ESP_LOGE(TAG, "i2s_write err=%s / wrote=%u – enter UNDERRUN",
                         esp_err_to_name(err), (unsigned)written);
                s_play_state = AUD_STATE_UNDERRUN;
                break;
            }

            ptr    += written;
            remain -= written;
            if (remain) vTaskDelay(pdMS_TO_TICKS(1));
        }

        /* Mark time of last successful DMA feed */
        s_last_audio_time = esp_timer_get_time();

        /* ---------- ⑤  recycle buffer ------------------------------------ */
        if (xQueueSend(s_empty_queue, &buf_idx, pdMS_TO_TICKS(20)) != pdTRUE) {
            ESP_LOGW(TAG, "Failed to return buffer %d to empty queue", buf_idx);
        }

        /* ---------- ⑥  tiny yield so other tasks get CPU ------------------ */
        taskYIELD();
    }
    /* never reached */
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
    return s_play_state == AUD_STATE_PLAYING;
}
