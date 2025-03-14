// audio_stream.h

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Handle a chunk of raw PCM data (mono, 16-bit, 24000 Hz).
 *
 * @param data   Pointer to the raw audio bytes
 * @param length Number of bytes in @p data
 */
void audio_stream_handle_incoming(const uint8_t *data, size_t length);

#ifdef __cplusplus
}
#endif
