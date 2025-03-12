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

// Internal state
static bool     s_wav_header_parsed = false;
static size_t   s_header_bytes_collected = 0;
static uint8_t  s_header_buf[sizeof(wav_header_t)];

static uint32_t s_wav_sample_rate = 44100;
static uint16_t s_wav_num_channels = 1;

static bool parse_wav_header(const uint8_t *hdr);
static void feed_pcm_data(const uint8_t *pcm_in, size_t in_len);

// Public functions
void audio_stream_reset_wav_header(void)
{
    s_wav_header_parsed = false;
    s_header_bytes_collected = 0;
    s_wav_sample_rate = 44100;
    s_wav_num_channels = 1;
}

void audio_stream_handle_incoming(const uint8_t *data, size_t length)
{
    // If header not yet parsed, accumulate
    if (!s_wav_header_parsed) {
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
                // You might decide to reset or ignore further data
                return;
            }
            s_wav_header_parsed = true;

            // The rest is PCM
            size_t pcm_len = length - needed;
            if (pcm_len > 0) {
                feed_pcm_data(data + needed, pcm_len);
            }
        }
    } else {
        // Already have header => all is PCM
        feed_pcm_data(data, length);
    }
}

// Private helpers
static bool parse_wav_header(const uint8_t *hdr)
{
    const wav_header_t *h = (const wav_header_t *)hdr;
    if (memcmp(h->riff, "RIFF", 4) != 0 ||
        memcmp(h->wave, "WAVE", 4) != 0 ||
        memcmp(h->fmt,  "fmt ", 4) != 0) {
        return false;
    }
    if (h->audio_format != 1) {
        ESP_LOGE(TAG, "Unsupported WAV (not PCM=1)");
        return false;
    }

    s_wav_sample_rate  = h->sample_rate;
    s_wav_num_channels = h->num_channels;

    ESP_LOGI(TAG, "WAV header => %u Hz, %u channels, %u bits",
             h->sample_rate, h->num_channels, h->bits_per_sample);

    // For simplicity, always set I2S to stereo + 44100 if you want. 
    // Or use h->sample_rate, h->num_channels directly:
    audio_player_set_sample_rate(s_wav_sample_rate,
                                 (s_wav_num_channels == 1) ? 2 : 2);
    return true;
}

static void feed_pcm_data(const uint8_t *pcm_in, size_t in_len)
{
    // If WAV is mono but I2S is stereo, we need to replicate channels
    if (s_wav_num_channels == 1) {
        // 16-bit assumption
        size_t num_samples = in_len / 2; // mono samples
        const uint16_t *in_samples = (const uint16_t *)pcm_in;

        // 2 * for stereo
        uint16_t *stereo_buf = malloc(num_samples * 2 * sizeof(uint16_t));
        if (!stereo_buf) {
            ESP_LOGE(TAG, "Failed to alloc stereo_buf");
            return;
        }
        for (size_t i = 0; i < num_samples; i++) {
            stereo_buf[2*i + 0] = in_samples[i]; // left
            stereo_buf[2*i + 1] = in_samples[i]; // right
        }
        audio_player_play((uint8_t *)stereo_buf,
                          num_samples * 2 * sizeof(uint16_t));
    } else {
        // 2 channels => pass directly
        audio_player_play((uint8_t *)pcm_in, in_len);
    }
}
