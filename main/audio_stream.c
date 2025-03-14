// audio_stream.c
#include "audio_stream.h"
#include "audio_player.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "AUDIO_STREAM";

#define SAMPLE_SIZE_BYTES 2  // 16-bit
#define PCM_SAMPLE_RATE   24000

/**
 * @brief Receives raw PCM (mono, 16-bit, 24000 Hz) and does
 *        mono->stereo expansion, then passes to audio_player.
 */
void audio_stream_handle_incoming(const uint8_t *data, size_t length)
{
    while (length > 0) {
        // Acquire one free buffer from the AudioPlayer
        audio_buffer_t *buf = audio_player_get_buffer_blocking();
        if (!buf) {
            ESP_LOGE(TAG, "No free buffer to store audio data!");
            return;
        }

        // Convert mono 16-bit to stereo => 2 x 16-bit
        // So if we read N mono samples, that's 2*N 16-bit samples out => 4*N bytes
        size_t max_stereo_bytes = AUDIO_BUFFER_SIZE; // capacity in the target buffer
        // The max # of mono samples we can convert into that buffer:
        // each mono sample becomes 4 bytes (2 channels * 2 bytes)
        size_t max_mono_samples = max_stereo_bytes / (2 * sizeof(uint16_t));

        // The # of mono samples actually available in the inbound data:
        size_t available_mono_samples = length / sizeof(uint16_t);

        // We'll convert the smaller of them:
        size_t convert_samples = (available_mono_samples < max_mono_samples)
                                 ? available_mono_samples
                                 : max_mono_samples;

        // Expand
        const uint16_t *src = (const uint16_t *)(data);
        uint16_t *dst       = (uint16_t *)buf->data;

        for (size_t i = 0; i < convert_samples; i++) {
            dst[2*i]   = src[i];  // Left
            dst[2*i+1] = src[i];  // Right
        }

        // The resulting stereo bytes is convert_samples * 2ch * 2 bytes:
        size_t bytes_converted = convert_samples * 2 * sizeof(uint16_t);
        buf->length = bytes_converted;

        size_t mono_bytes_consumed = convert_samples * sizeof(uint16_t);

        // Advance the offset in the incoming buffer
        data += mono_bytes_consumed;
        length -= mono_bytes_consumed;

        audio_player_submit_buffer(buf);
    }
    
}
