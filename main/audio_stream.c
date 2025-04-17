#include <stdint.h>   // <-- gives uint8_t, int8_t, …
#include <stddef.h>   // <-- gives size_t
#include "audio_stream.h"
#include "audio_player.h"
#include "esp_log.h"
#define SAMPLE_SIZE_BYTES 1  // incoming is now 8‑bit
#define PCM_SAMPLE_RATE   8000

static const char *TAG = "AUDIO_STREAM";

void audio_stream_handle_incoming(const uint8_t *data, size_t length)
{
    /* incoming ‘data’ is:
     *   – mono, 8‑bit, signed (two‑s complement)
     *   – prefixed with 0x02 already stripped by caller
     * We expand each 8‑bit sample to 16‑bit and duplicate to L/R.        */

    while (length > 0) {
        audio_buffer_t *buf = audio_player_get_buffer_blocking();
        if (!buf) {
            ESP_LOGW(TAG, "No free buffer => skipping %u bytes",
                     (unsigned)length);
            return;
        }

        size_t max_stereo_bytes  = AUDIO_BUFFER_SIZE;
        size_t max_mono_samples  = max_stereo_bytes / (2 * sizeof(uint16_t));
        size_t available_mono_samples = length;              /* 1 byte/sample */
        size_t convert_samples   = (available_mono_samples < max_mono_samples)
                                   ? available_mono_samples
                                   : max_mono_samples;

        const uint8_t  *src8 = data;                         /* 8‑bit source */
        uint16_t *dst       = (uint16_t *)buf->data;
        float volume        = audio_player_get_volume();

        for (size_t i = 0; i < convert_samples; i++) {
            /* centred signed 8‑bit → signed 16‑bit */
            int16_t s = ((int16_t)( (int8_t)src8[i] )) << 8;
            s = (int16_t)(s * volume);
            dst[2*i]   = s; /* L */
            dst[2*i+1] = s; /* R */
        }

        size_t bytes_converted = convert_samples * 2 * sizeof(uint16_t);
        buf->length            = bytes_converted;

        data   += convert_samples;  /* 1 byte / sample consumed */
        length -= convert_samples;

        if (!audio_player_submit_buffer(buf)) {
            ESP_LOGW(TAG, "submit_buffer failed => skipping remaining %u bytes",
                     (unsigned)length);
            return;
        }
    }
}
