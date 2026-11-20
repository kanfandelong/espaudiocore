/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 kanfandelong. All rights reserved.
 *
 * pcm_compare -- compare two raw interleaved s16le PCM files.
 *
 * Used to sanity-check lossy decoders against an ffmpeg reference:
 * sample counts, RMS, max sample difference, SNR and correlation.
 *
 *   pcm_compare ours.pcm ref.pcm <channels> <rate>
 */

#define _GNU_SOURCE

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int16_t *read_pcm(const char *path, size_t *samples_out)
{
    FILE *f = fopen(path, "rb");
    if (!f)
    {
        fprintf(stderr, "cannot open %s\n", path);
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0)
    {
        fclose(f);
        return NULL;
    }
    long sz = ftell(f);
    if (sz < 0)
    {
        fclose(f);
        return NULL;
    }
    rewind(f);

    size_t samples = (size_t)sz / sizeof(int16_t);
    int16_t *buf = (int16_t *)malloc(samples * sizeof(int16_t));
    if (!buf)
    {
        fclose(f);
        return NULL;
    }
    size_t got = fread(buf, sizeof(int16_t), samples, f);
    fclose(f);
    *samples_out = got;
    return buf;
}

int main(int argc, char **argv)
{
    if (argc < 5)
    {
        fprintf(stderr, "usage: %s <ours.pcm> <ref.pcm> <channels> <rate>\n", argv[0]);
        return 2;
    }
    const int channels = atoi(argv[3]);
    const int rate = atoi(argv[4]);
    if (channels <= 0 || rate <= 0)
    {
        return 2;
    }

    size_t na = 0, nb = 0;
    int16_t *a = read_pcm(argv[1], &na);
    int16_t *b = read_pcm(argv[2], &nb);
    if (!a || !b)
    {
        free(a);
        free(b);
        return 2;
    }

    size_t n = na < nb ? na : nb;
    int64_t sum_a = 0, sum_b = 0, sum_d = 0, dot = 0;
    int max_diff = 0;
    for (size_t i = 0; i < n; i++)
    {
        int64_t va = a[i];
        int64_t vb = b[i];
        int64_t d = va - vb;
        sum_a += va * va;
        sum_b += vb * vb;
        sum_d += d * d;
        dot += va * vb;
        int ad = d < 0 ? (int)-d : (int)d;
        if (ad > max_diff)
        {
            max_diff = ad;
        }
    }

    double rms_a = n ? sqrt((double)sum_a / (double)n) : 0.0;
    double rms_b = n ? sqrt((double)sum_b / (double)n) : 0.0;
    double rms_d = n ? sqrt((double)sum_d / (double)n) : 0.0;
    double snr = (sum_d > 0 && sum_b > 0) ? 10.0 * log10((double)sum_b / (double)sum_d) : INFINITY;
    double corr = (sum_a > 0 && sum_b > 0)
                      ? (double)dot / (sqrt((double)sum_a) * sqrt((double)sum_b))
                      : 0.0;

    printf("frames: ours=%.3f s, ref=%.3f s (ch=%d rate=%d)\n",
           (double)na / (double)(channels * rate), (double)nb / (double)(channels * rate),
           channels, rate);
    printf("samples: ours=%zu, ref=%zu, compared=%zu\n", na, nb, n);
    printf("rms: ours=%.1f, ref=%.1f, diff=%.1f, max_diff=%d\n", rms_a, rms_b, rms_d, max_diff);
    printf("snr=%.2f dB, corr=%.6f\n", snr, corr);

    free(a);
    free(b);
    return (na == nb && max_diff == 0) ? 0 : 1;
}
