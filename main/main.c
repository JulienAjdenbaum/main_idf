#include <stdio.h>
#include "sd_card_utils.h"
#include "audio_recorder.h"

void app_main(void) {
    esp_err_t ret = initialize_sd_card();
    if (ret != ESP_OK) {
        printf("SD card initialization failed\n");
        return;
    }

    // Initialize I2S
    i2s_init_for_mic();
    write_file_to_sd_card("test.txt", "Hello, World!");
    // Record and save audio
    // mkdir("/sdcard/recordings", 0777);  // Create recordings directory
    record_audio_to_sd_card("rec.wav");

    // List files to verify
    list_files_in_sd_card();

    // Unmount the SD card
    unmount_sd_card();
}
