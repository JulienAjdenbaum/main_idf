#pragma once
#include <stdint.h>
#include <stddef.h>         /* gives size_t                                   */

/* Decoder state – must be preserved between blocks */
typedef struct {
    int16_t predictor;      /* last reconstructed sample                      */
    int8_t  index;          /* 0‑88                                           */
} ima_state_t;

/* Decode `n_samples` (must be even – 2 per encoded byte)
   in  : pointer to 4‑bit ADPCM nibbles (no header)
   out : int16_t PCM samples (mono)
   st  : state (updated in‑place)
   returns n_samples                                                  */
size_t ima_decode_block(const uint8_t *in,
                        int16_t      *out,
                        ima_state_t  *st,
                        size_t        n_samples);
