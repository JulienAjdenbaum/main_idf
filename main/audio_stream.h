// audio_stream.h
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Called when we get new audio bytes from the websocket (minus prefix 0x02)
void audio_stream_handle_incoming(const uint8_t *data, size_t length);

// Reset the internal WAV header parse state
void audio_stream_reset_wav_header(void);

#ifdef __cplusplus
}
#endif
