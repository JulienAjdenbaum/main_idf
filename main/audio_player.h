#ifndef AUDIO_PLAYER_H
#define AUDIO_PLAYER_H

#include "driver/i2s.h"
#include <stdint.h>
#include <stdlib.h>
#include "esp_err.h"

#define NUM_AUDIO_BUFFERS   64             // how many buffers to keep
#define AUDIO_BUFFER_SIZE   (1024)      // each buffer size in bytes


// I2S Configuration
#define I2S_NUM           (I2S_NUM_0)
#define I2S_BCK_IO        (26)
#define I2S_WS_IO         (25)
#define I2S_DO_IO         (27)
#define I2S_DI_IO         (-1)  // Not used for output
#define SAMPLE_RATE       (24000)
#define DMA_BUF_COUNT     (8)
#define DMA_BUF_LEN       (1024)

typedef struct {
    uint8_t data[AUDIO_BUFFER_SIZE];
    size_t  length; // how many bytes actually used
} audio_buffer_t;

// Public functions
esp_err_t audio_player_init(void);
void audio_player_shutdown(void);
void audio_player_set_sample_rate(uint32_t sample_rate, uint16_t num_channels);
audio_buffer_t  *audio_player_get_buffer_blocking(void);
void             audio_player_submit_buffer(audio_buffer_t *buf);


#endif // AUDIO_PLAYER_H 