#include "audio_player.h"
#include "sd_card_utils.h"
#include <string.h>
#include <dirent.h>  // For directory operations
#include <esp_log.h> // For logging

// Define the TAG for logging
static const char *TAG = "main";

// WAV header structure (first 44 bytes)
typedef struct __attribute__((packed)) {
    char     riff[4];
    uint32_t overall_size;
    char     wave[4];
    char     fmt[4];
    uint32_t fmt_chunk_size;
    uint16_t audio_format;
    uint16_t num_channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;
    char     data[4];
    uint32_t data_size;
} wav_header_t;

// Add this function to play WAV files
void play_wav(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) {
        ESP_LOGE(TAG, "Failed to open file: %s", path);
        return;
    }

    wav_header_t *header = malloc(sizeof(wav_header_t));
    if (!header) {
        ESP_LOGE(TAG, "Failed to allocate memory for WAV header");
        fclose(file);
        return;
    }

    if (fread(header, sizeof(wav_header_t), 1, file) != 1) {
        ESP_LOGE(TAG, "Failed to read WAV header");
        free(header);
        fclose(file);
        return;
    }

    // Verify WAV format
    if (memcmp(header->riff, "RIFF", 4) != 0 || 
        memcmp(header->wave, "WAVE", 4) != 0 ||
        memcmp(header->fmt, "fmt ", 4) != 0) {
        ESP_LOGE(TAG, "Invalid WAV file");
        free(header);
        fclose(file);
        return;
    }

    audio_player_set_sample_rate(header->sample_rate, header->num_channels);

    // Stream audio data
    size_t bytes_read;
    while (1) {
        uint8_t *buffer = malloc(4096);  // Double buffer size
        bytes_read = fread(buffer, 1, 4096, file);
        
        if (bytes_read == 0) {
            free(buffer);
            break;
        }
        
        // Add delay to match audio throughput
        vTaskDelay(pdMS_TO_TICKS(10));
        
        // Ensure alignment for stereo (4 bytes per sample)
        bytes_read = bytes_read - (bytes_read % 4);
        
        audio_player_play(buffer, bytes_read);
    }

    free(header);
    fclose(file);
}

// Modified app_main to play all WAV files
void app_main() {
    // Initialize components
    ESP_ERROR_CHECK(audio_player_init());
    
    if (initialize_sd_card() != ESP_OK) {
        ESP_LOGE(TAG, "SD Card initialization failed!");
        return;
    }

    // List and play WAV files
    DIR *dir = opendir("/sdcard");
    if (!dir) {
        ESP_LOGE(TAG, "Failed to open SD card directory");
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        ESP_LOGI(TAG, "Found file: %s", entry->d_name);
        if (strstr(entry->d_name, ".WAV")) {
            char path[512];
            snprintf(path, sizeof(path), "/sdcard/%s", entry->d_name);
            ESP_LOGI(TAG, "Playing file: %s", path);
            play_wav(path);
        }
    }
    closedir(dir);

    // Cleanup
    unmount_sd_card();
    audio_player_shutdown();
}