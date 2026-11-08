/*
 * SPDX-License-Identifier: MIT
 * Simple stereo downmix coefficients shared by decoders whose libraries can
 * emit more than two channels.
 *
 * Weights are in 1/1000 units. Each output side is divided by the sum of its
 * weights, so a 5.1/7.1 stream is attenuated instead of clipping.
 */
#ifndef AUDIO_DOWNMIX_H
#define AUDIO_DOWNMIX_H

#include <stdint.h>

#if defined(__cplusplus)
extern "C" {
#endif

/* Standard channel order used by WAV/Vorbis/WavPack/FLAC:
 *   3: FL FR FC
 *   4: FL FR BL BR
 *   5: FL FR FC BL BR
 *   6: FL FR FC LFE BL BR
 *   7: FL FR FC LFE BC SL SR
 *   8: FL FR FC LFE BL BR SL SR
 * Returns false for unsupported channel counts.
 */
static inline bool audio_downmix_coeffs(unsigned channels, int wl[8], int wr[8])
{
    for (int i = 0; i < 8; ++i)
    {
        wl[i] = 0;
        wr[i] = 0;
    }

    switch (channels)
    {
    case 3:
        wl[0] = 1000; wl[2] = 707;
        wr[1] = 1000; wr[2] = 707;
        return true;
    case 4:
        wl[0] = 1000; wl[2] = 707;
        wr[1] = 1000; wr[3] = 707;
        return true;
    case 5:
        wl[0] = 1000; wl[2] = 707; wl[3] = 707;
        wr[1] = 1000; wr[2] = 707; wr[4] = 707;
        return true;
    case 6:
        wl[0] = 1000; wl[2] = 707; wl[3] = 500; wl[4] = 707;
        wr[1] = 1000; wr[2] = 707; wr[3] = 500; wr[5] = 707;
        return true;
    case 7:
        wl[0] = 1000; wl[2] = 707; wl[3] = 500; wl[4] = 707; wl[5] = 707;
        wr[1] = 1000; wr[2] = 707; wr[3] = 500; wr[4] = 707; wr[6] = 707;
        return true;
    case 8:
        wl[0] = 1000; wl[2] = 707; wl[3] = 500; wl[4] = 707; wl[6] = 707;
        wr[1] = 1000; wr[2] = 707; wr[3] = 500; wr[5] = 707; wr[7] = 707;
        return true;
    default:
        return false;
    }
}

static inline int32_t audio_downmix_clamp_i32(int64_t v)
{
    if (v > 2147483647LL)
        return 2147483647;
    if (v < -2147483648LL)
        return -2147483648;
    return (int32_t)v;
}

#if defined(__cplusplus)
}
#endif

#endif /* AUDIO_DOWNMIX_H */
