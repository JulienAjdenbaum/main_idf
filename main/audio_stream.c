// audio_stream.c

#include "audio_stream.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>
#include "audio_player.h"

static const char *TAG = "AUDIO_STREAM";

// WAV header structure
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

static bool     s_wav_header_parsed = false;
static size_t   s_header_bytes_collected = 0;
static uint8_t  s_header_buf[sizeof(wav_header_t)];

static uint32_t s_wav_sample_rate  = 44100;
static uint16_t s_wav_num_channels = 1;


// Forward declarations
static bool parse_wav_header(const uint8_t *hdr);
static void feed_pcm_data(const uint8_t *pcm_in, size_t in_len);


// Called if you want to restart WAV parsing from scratch
void audio_stream_reset_wav_header(void)
{
    s_wav_header_parsed = false;
    s_header_bytes_collected = 0;
    s_wav_sample_rate = 44100;
    s_wav_num_channels = 1;
}

/**
 * Handle incoming chunk of data that may contain:
 *   - partial or complete WAV header
 *   - or raw PCM data (once header is parsed)
 */
void audio_stream_handle_incoming(const uint8_t *data, size_t length)
{
    if (!s_wav_header_parsed) {
        // still collecting WAV header
        size_t needed = sizeof(wav_header_t) - s_header_bytes_collected;
        if (length < needed) {
            // partial header
            memcpy(&s_header_buf[s_header_bytes_collected], data, length);
            s_header_bytes_collected += length;
            return;
        } else {
            // complete the header
            memcpy(&s_header_buf[s_header_bytes_collected], data, needed);
            s_header_bytes_collected += needed;

            if (!parse_wav_header(s_header_buf)) {
                ESP_LOGE(TAG, "Invalid WAV header!");
                // decide how you want to handle error
                return;
            }
            s_wav_header_parsed = true;

            // the remainder is PCM data
            size_t pcm_len = length - needed;
            if (pcm_len > 0) {
                feed_pcm_data(data + needed, pcm_len);
            }
        }
    } else {
        // already parsed header => all data is PCM
        feed_pcm_data(data, length);
    }
}


//------------------ Private Helpers ---------------------//
static bool parse_wav_header(const uint8_t *hdr)
{
    const wav_header_t *h = (const wav_header_t *)hdr;

    if (memcmp(h->riff, "RIFF", 4) != 0 ||
        memcmp(h->wave, "WAVE", 4) != 0 ||
        memcmp(h->fmt,  "fmt ", 4) != 0)
    {
        return false;
    }
    if (h->audio_format != 1) {
        ESP_LOGE(TAG, "Unsupported WAV format (we only handle PCM=1)");
        return false;
    }

    s_wav_num_channels = h->num_channels;
    s_wav_sample_rate  = h->sample_rate;

    ESP_LOGI(TAG, "WAV header => sample_rate=%u, channels=%u, bits=%u",
             h->sample_rate, h->num_channels, h->bits_per_sample);

    // If you want dynamic sample-rate switching:
    audio_player_set_sample_rate(s_wav_sample_rate, s_wav_num_channels);

    return true;
}

static void feed_pcm_data(const uint8_t *pcm_in, size_t in_len)
{
    if (s_wav_num_channels == 1) {
        // Convert from mono (16-bit) to stereo
        size_t num_mono_samples = in_len / 2; // each sample is 2 bytes
        size_t stereo_bytes     = num_mono_samples * 2 * sizeof(uint16_t);

        // 1) Get an empty buffer from the player:
        audio_buffer_t *buf = audio_player_get_buffer_blocking();
        if (!buf) {
            ESP_LOGE(TAG, "No free buffer for stereo conversion!");
            return;
        }

        // 2) Fill that buffer
        // Possibly chunk if 'stereo_bytes' is bigger than AUDIO_BUFFER_SIZE
        if (stereo_bytes > AUDIO_BUFFER_SIZE) {
            ESP_LOGW(TAG, "Incoming PCM too large for one buffer => truncating");
            stereo_bytes = AUDIO_BUFFER_SIZE;
        }

        uint16_t *dst = (uint16_t *) buf->data;
        const uint16_t *src = (const uint16_t *) pcm_in;
        size_t max_stereo_samples = stereo_bytes / 2; // # of 16-bit slots

        // replicate each mono sample into L & R
        for (size_t i = 0, j = 0; i < num_mono_samples && (j+1) < max_stereo_samples; i++, j+=2) {
            dst[j]   = src[i]; // left
            dst[j+1] = src[i]; // right
        }

        buf->length = stereo_bytes;
        // 3) Submit for playback
        audio_player_submit_buffer(buf);
    }
    else {
        // Already stereo => pass directly
        audio_buffer_t *buf = audio_player_get_buffer_blocking();
        if (!buf) {
            ESP_LOGE(TAG, "No free buffer for stereo data!");
            return;
        }

        size_t copy_len = in_len;
        if (copy_len > AUDIO_BUFFER_SIZE) {
            ESP_LOGW(TAG, "Incoming PCM too large => truncating");
            copy_len = AUDIO_BUFFER_SIZE;
        }

        memcpy(buf->data, pcm_in, copy_len);
        buf->length = copy_len;
        audio_player_submit_buffer(buf);
    }
}
