// audio_player.h
#ifndef AUDIO_PLAYER_H
#define AUDIO_PLAYER_H

#include <stdint.h>
#include <stdlib.h>
#include "esp_err.h"
#include "driver/i2s.h"

// 24 kHz by default
#define SAMPLE_RATE     (8000)

#define DMA_BUF_COUNT   (16)
#define DMA_BUF_LEN     (512)

#define AUDIO_BUFFER_SIZE  512  // bytes
#define NUM_AUDIO_BUFFERS  16

typedef struct {
    uint8_t data[AUDIO_BUFFER_SIZE];
    size_t  length;
} audio_buffer_t;

esp_err_t audio_player_init(void);
void      audio_player_shutdown(void);
audio_buffer_t *audio_player_get_buffer_blocking(void);
bool audio_player_submit_buffer(audio_buffer_t *buf);
void      audio_player_set_sample_rate(uint32_t sample_rate, uint16_t num_channels);
bool audio_player_is_playing(void);

void  audio_player_set_volume(float vol);
float audio_player_get_volume(void);

#endif // AUDIO_PLAYER_H
