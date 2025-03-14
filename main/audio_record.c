#include "audio_record.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/i2s.h"
#include "esp_log.h"
#include "esp_err.h"

#include "websocket_manager.h" // We'll use websocket_manager_send_bin(), websocket_manager_is_connected()

// -------------------- CONFIG / DEFINES --------------------
#define MIC_I2S_PORT            I2S_NUM_1
#define MIC_I2S_BCK_IO          14
#define MIC_I2S_WS_IO           13
#define MIC_I2S_DATA_IN_IO      12
#define MIC_I2S_DATA_OUT_IO     -1  // not used for mic-only
#define MIC_USE_APLL            false  // ← Add this

#define I2S_SAMPLE_RATE     16000
#define I2S_BITS_PER_SAMPLE I2S_BITS_PER_SAMPLE_32BIT
#define I2S_READ_BUF_SIZE   1024 // Number of bytes to read per i2s_read() call

// We will send audio prefixed by 0x02
#define AUDIO_PREFIX_BYTE   0x02

static const char *TAG = "AUDIO_RECORD";

// -------------------- INTERNAL FUNCTION DECLARATIONS --------------------
static void audio_record_task(void *arg);
// static void i2s_init_for_mic(void);

// -------------------- I2S INIT --------------------
static void i2s_init_for_mic(void)
{
    // Only uninstall if already installed
    // if (i2s_is_driver_installed(I2S_PORT)) {
    // i2s_driver_uninstall(MIC_I2S_PORT);
    // }

    i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
        .sample_rate = I2S_SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_I2S_MSB,
        .dma_buf_count = 8,
        .dma_buf_len = 512,
        .use_apll = MIC_USE_APLL,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .tx_desc_auto_clear = false,
        .fixed_mclk = 0
    };

    i2s_pin_config_t pin_config_mic = {
        .mck_io_num = I2S_PIN_NO_CHANGE,
        .bck_io_num = MIC_I2S_BCK_IO,
        .ws_io_num = MIC_I2S_WS_IO,
        .data_out_num = MIC_I2S_DATA_OUT_IO,     // not used for mic-only
        .data_in_num = MIC_I2S_DATA_IN_IO
    };

    ESP_ERROR_CHECK(i2s_driver_install(MIC_I2S_PORT, &i2s_config, 0, NULL));
    ESP_ERROR_CHECK(i2s_set_pin(MIC_I2S_PORT, &pin_config_mic));

    ESP_LOGI(TAG, "I2S initialized for mic input (sr=%d, bits=%d)",
             I2S_SAMPLE_RATE, I2S_BITS_PER_SAMPLE);
}

// -------------------- AUDIO RECORD TASK --------------------
static void audio_record_task(void *arg)
{
    // Initialize I2S once at startup
    i2s_init_for_mic();

    // Buffers for reading and converting
    static int32_t s_audio_buf[256]; // 256 x 32-bit = 1024 bytes
    uint8_t conv_buf[512];           // 256 x 16-bit = 512 bytes

    while (true) {
        // Only record + send if WebSocket is connected
        if (!websocket_manager_is_connected()) {
            vTaskDelay(pdMS_TO_TICKS(300));
            continue;
        }

        size_t bytes_read = 0;
        esp_err_t err = i2s_read(MIC_I2S_PORT,
                                 (void *)s_audio_buf,
                                 I2S_READ_BUF_SIZE,
                                 &bytes_read,
                                 portMAX_DELAY);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "i2s_read error: %s", esp_err_to_name(err));
            continue;
        }
        if (bytes_read == 0) {
            // No data; keep looping
            continue;
        }

        // Convert 32-bit samples (in s_audio_buf) down to 16-bit (into conv_buf)
        int num_samples_32 = bytes_read / 4; // each sample is 4 bytes
        int out_index      = 0;

        for (int i = 0; i < num_samples_32; i++) {
            int32_t s_32 = s_audio_buf[i];
            // shift down to get 16-bit, the >>13 is an example shift
            int16_t s_16 = (int16_t)(s_32 >> 13);

            // Write 16-bit sample into conv_buf (little-endian)
            conv_buf[out_index++] = (uint8_t)(s_16 & 0xFF);
            conv_buf[out_index++] = (uint8_t)((s_16 >> 8) & 0xFF);
        }

        // Now send it out via WebSocket, prefixing with 0x02
        // We'll build a small buffer that has 1 extra byte for the prefix
        static uint8_t send_buf[1 + sizeof(conv_buf)];
        send_buf[0] = AUDIO_PREFIX_BYTE;  // 0x02
        memcpy(send_buf + 1, conv_buf, out_index);

        int packet_size = out_index + 1;
        int ret = websocket_manager_send_bin((const char *)send_buf, packet_size);
        if (ret < 0) {
            ESP_LOGE(TAG, "WebSocket send_bin failed (%d)", ret);
        }

        // Just a small delay to avoid hogging CPU
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

// -------------------- PUBLIC API --------------------
void audio_record_init(void)
{
    // Create a dedicated task that will indefinitely record and stream
    xTaskCreate(audio_record_task, "audio_record_task", 4096, NULL, 5, NULL);
}
