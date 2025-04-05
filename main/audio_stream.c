#include "audio_stream.h"
#include "audio_player.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "AUDIO_STREAM";

#define SAMPLE_SIZE_BYTES 2  // 16-bit
#define PCM_SAMPLE_RATE   8000

void audio_stream_handle_incoming(const uint8_t *data, size_t length)
{
    while (length > 0) {
        audio_buffer_t *buf = audio_player_get_buffer_blocking();
        if (!buf) {
            // No free buffer => skip the rest
            ESP_LOGW(TAG, "No free buffer => skipping %u bytes", (unsigned)length);
            return;
        }

        size_t max_stereo_bytes  = AUDIO_BUFFER_SIZE;
        size_t max_mono_samples  = max_stereo_bytes / (2 * sizeof(uint16_t));
        size_t available_mono_samples = length / sizeof(uint16_t);
        size_t convert_samples   = (available_mono_samples < max_mono_samples)
                                   ? available_mono_samples
                                   : max_mono_samples;

        const uint16_t *src = (const uint16_t *)(data);
        uint16_t *dst       = (uint16_t *)buf->data;
        float volume        = audio_player_get_volume();

        for (size_t i = 0; i < convert_samples; i++) {
            int16_t s = src[i];
            s = (int16_t)(s * volume);
            dst[2*i]   = s; // Left
            dst[2*i+1] = s; // Right
        }

        size_t bytes_converted = convert_samples * 2 * sizeof(uint16_t);
        buf->length            = bytes_converted;
        size_t mono_bytes_used = convert_samples * sizeof(uint16_t);

        data   += mono_bytes_used;
        length -= mono_bytes_used;

        if (!audio_player_submit_buffer(buf)) {
            // We tried to queue it but the ready_queue was full => skip
            ESP_LOGW(TAG, "submit_buffer failed => skipping remaining %u bytes", (unsigned)length);
            return;
        }
    }
}
