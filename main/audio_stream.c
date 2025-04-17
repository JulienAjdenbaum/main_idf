#include <stdint.h>
#include <stddef.h>
#include "audio_stream.h"
#include "audio_player.h"
#include "esp_log.h"
#include "adpcm_ima.h"

#ifndef MIN
#   define MIN(a,b)  (( (a) < (b) ) ? (a) : (b))
#endif

static const char *TAG = "AUDIO_STREAM";

/* forward‑declared legacy path */
static void legacy_pcm_handler(const uint8_t *data, size_t len);

/* persistent ADPCM state */
static ima_state_t g_state = {0, 0};

void audio_stream_handle_incoming(const uint8_t *data, size_t len)
{
    if (!len) return;

    /* ── legacy 0x02 → 8‑bit PCM ─────────────────────────── */
    if (data[0] == 0x02) {
        legacy_pcm_handler(data + 1, len - 1);
        return;
    }

    /* ── 0x12 → IMA‑ADPCM (132‑byte frame) ──────────────── */
    if (data[0] != 0x12 || len < 133) {
        ESP_LOGW(TAG, "Dropped unknown frame (marker 0x%02X)", data[0]);
        return;
    }

    g_state.predictor = (int16_t)(data[1] | (data[2] << 8)); // keep
    g_state.index     = data[3] & 0x7F;                      // was data[3]
    const uint8_t *adpcm = data + 4;                         // OK

    int16_t pcm16[256];
    ima_decode_block(adpcm, pcm16, &g_state, 256);

    audio_buffer_t *buf = audio_player_get_buffer_blocking();
    if (!buf) return;

    float vol = audio_player_get_volume();
    uint16_t *dst = (uint16_t *)buf->data;

    for (size_t i = 0; i < 256; ++i) {
        int16_t s = (int16_t)(pcm16[i] * vol);
        dst[2*i] = dst[2*i+1] = s;
    }
    buf->length = 256 * 2 * sizeof(uint16_t);
    audio_player_submit_buffer(buf);
}

/* ── legacy helper ──────────────────────────────────────── */
static void legacy_pcm_handler(const uint8_t *data, size_t len)
{
    while (len) {
        audio_buffer_t *buf = audio_player_get_buffer_blocking();
        if (!buf) return;

        size_t max_mono = AUDIO_BUFFER_SIZE / (2 * sizeof(uint16_t));
        size_t todo     = MIN(max_mono, len);

        float vol = audio_player_get_volume();
        uint16_t *dst = (uint16_t *)buf->data;

        for (size_t i = 0; i < todo; ++i) {
            int16_t s = ((int16_t)((int8_t)data[i])) << 8;
            s = (int16_t)(s * vol);
            dst[2*i] = dst[2*i+1] = s;
        }

        buf->length = todo * 2 * sizeof(uint16_t);
        audio_player_submit_buffer(buf);

        data += todo;
        len  -= todo;
    }
}
