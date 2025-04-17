#include "adpcm_ima.h"
#include <stddef.h>

/* ========================= tables ========================= */
static const int step_table[89] = {
      7,  8,  9, 10, 11, 12, 13, 14,
     16, 17, 19, 21, 23, 25, 28, 31,
     34, 37, 41, 45, 50, 55, 60, 66,
     73, 80, 88, 97,107,118,130,143,
    157,173,190,209,230,253,279,307,
    337,371,408,449,494,544,598,658,
    724,796,876,963,1060,1166,1282,1411,
    1552,1707,1878,2066,2272,2499,2749,3024,
    3327,3660,4026,4428,4871,5358,5894,6484,
    7132,7845,8630,9493,10442,11487,12635,13899,
    15289,16818,18500,20350,22385,24623,27086,29794,
    32767
};

static const int8_t index_table[16] = {
   -1,-1,-1,-1, 2, 4, 6, 8,
   -1,-1,-1,-1, 2, 4, 6, 8
};

/* ========================= decoder ======================== */
size_t ima_decode_block(const uint8_t *in,
                        int16_t      *out,
                        ima_state_t  *st,
                        size_t        n_samples)
{
    int predictor = st->predictor;
    int index     = st->index;

    for (size_t s = 0; s < n_samples; ++s) {
        uint8_t code = (s & 1) ? (in[s >> 1] >> 4) : (in[s >> 1] & 0x0F);

        int step = step_table[index];
        int diff = step >> 3;
        if (code & 1) diff += step >> 2;
        if (code & 2) diff += step >> 1;
        if (code & 4) diff += step;
        if (code & 8) diff = -diff;

        predictor += diff;
        if (predictor >  32767) predictor =  32767;
        if (predictor < -32768) predictor = -32768;

        index += index_table[code];
        if (index < 0)   index = 0;
        if (index > 88)  index = 88;

        out[s] = (int16_t) predictor;
    }

    st->predictor = (int16_t) predictor;
    st->index     = (int8_t)  index;
    return n_samples;
}
