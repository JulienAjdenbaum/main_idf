#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s.h"
#include "esp_log.h"
#include "esp_err.h"
#include "sd_card_utils.h"

#define I2S_PORT I2S_NUM_0
#define I2S_SAMPLE_RATE 22050
#define I2S_BITS_PER_SAMPLE I2S_BITS_PER_SAMPLE_16BIT
#define RECORDING_DURATION_SEC 10
#define I2S_READ_BUF_SIZE 1024

#define I2S_BCK_IO      26
#define I2S_WS_IO       25
#define I2S_DATA_IN_IO  32
#define I2S_PORT        I2S_NUM_0
#define I2S_SAMPLE_RATE 22050
#define I2S_BITS_PER_SAMPLE I2S_BITS_PER_SAMPLE_16BIT
#define I2S_READ_BUF_SIZE 1024

static const char *TAG = "AudioRecorder";

typedef struct {
    char chunkID[4];   // "RIFF"
    uint32_t chunkSize;
    char format[4];    // "WAVE"
    char subchunk1ID[4]; // "fmt "
    uint32_t subchunk1Size;
    uint16_t audioFormat;
    uint16_t numChannels;
    uint32_t sampleRate;
    uint32_t byteRate;
    uint16_t blockAlign;
    uint16_t bitsPerSample;
    char subchunk2ID[4]; // "data"
    uint32_t subchunk2Size;
} WavHeader;

// Function to write WAV header
void write_wav_header(FILE *file, uint32_t num_samples) {
    WavHeader header;
    memcpy(header.chunkID, "RIFF", 4);
    memcpy(header.format, "WAVE", 4);
    memcpy(header.subchunk1ID, "fmt ", 4);
    memcpy(header.subchunk2ID, "data", 4);

    header.chunkSize = 36 + num_samples * 4; // 2 channels * 2 bytes/sample
    header.subchunk1Size = 16;
    header.audioFormat = 1; // PCM
    header.numChannels = 2; // Stereo
    header.sampleRate = I2S_SAMPLE_RATE;
    header.bitsPerSample = I2S_BITS_PER_SAMPLE;
    header.byteRate = header.sampleRate * header.numChannels * (header.bitsPerSample / 8);
    header.blockAlign = header.numChannels * (header.bitsPerSample / 8);
    header.subchunk2Size = num_samples * 4; // 2 bytes per channel, 2 channels

    fwrite(&header, sizeof(WavHeader), 1, file);
}

void record_audio_to_sd_card(const char* filename) {
    char path[128];
    snprintf(path, sizeof(path), "%s/%s", mount_point, filename);

    FILE* f = fopen(path, "wb");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open file for writing: %s", filename);
        return;
    }

    uint32_t total_samples = I2S_SAMPLE_RATE * RECORDING_DURATION_SEC;
    write_wav_header(f, total_samples);

    int16_t sample_buffer[I2S_READ_BUF_SIZE / 2]; // Buffer for stereo data
    size_t bytes_read;
    uint32_t samples_written = 0;
    TickType_t start_time = xTaskGetTickCount();
    
    ESP_LOGI(TAG, "Recording started...");

    while (samples_written < total_samples) {
        esp_err_t err = i2s_read(I2S_PORT, sample_buffer, I2S_READ_BUF_SIZE, &bytes_read, portMAX_DELAY);
        if (err == ESP_OK && bytes_read > 0) {
            fwrite(sample_buffer, 1, bytes_read, f);
            samples_written += bytes_read / 4; // Since each stereo sample is 4 bytes (2 bytes per channel)
        }
        if ((xTaskGetTickCount() - start_time) * portTICK_PERIOD_MS >= RECORDING_DURATION_SEC * 1000) {
            break;
        }
    }

    ESP_LOGI(TAG, "Recording finished. Saved as: %s", filename);
    fclose(f);
}

void i2s_init_for_mic(void) {
    i2s_config_t i2s_config = {
        .mode = I2S_MODE_MASTER | I2S_MODE_RX,
        .sample_rate = I2S_SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = I2S_COMM_FORMAT_I2S,
        .dma_buf_count = 8,
        .dma_buf_len = 512,
        .use_apll = false,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1
    };

    i2s_pin_config_t pin_config = {
        .bck_io_num = I2S_BCK_IO,
        .ws_io_num = I2S_WS_IO,
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num = I2S_DATA_IN_IO
    };

    ESP_ERROR_CHECK(i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL));
    ESP_ERROR_CHECK(i2s_set_pin(I2S_PORT, &pin_config));

    ESP_LOGI(TAG, "I2S initialized for stereo microphone input");
}
