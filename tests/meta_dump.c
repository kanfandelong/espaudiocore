/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 kanfandelong. All rights reserved.
 *
 * meta_dump -- print every metadata item a decoder reports.
 *
 * Cover art is binary, so eac_play (which only prints Title/Artist/Album)
 * cannot verify it.  This tool decodes into a ring buffer, discards the PCM
 * and prints text tags, picture MIME type, picture size and the first bytes
 * of the image signature, which is enough to check WAV/FLAC/Vorbis/Opus
 * cover parsing without an audio device.
 *
 *   meta_dump tests/assets/t_cover.flac
 */

#define _GNU_SOURCE

#include "espaudiocore.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int g_text = 0;
static int g_pictures = 0;

static void on_meta(void *user, const espaudiocore_meta_t *meta)
{
    (void)user;
    if (!meta || !meta->type)
    {
        return;
    }

    if (meta->is_binary)
    {
        const unsigned char *p = (const unsigned char *)meta->data;
        g_pictures++;
        printf("[meta] type=%s binary=1 len=%zu mime=%s pic=%d sig=%02x%02x%02x%02x\n",
               meta->type, meta->len, meta->mime ? meta->mime : "(null)", meta->pic_type,
               meta->len > 0 ? p[0] : 0, meta->len > 1 ? p[1] : 0,
               meta->len > 2 ? p[2] : 0, meta->len > 3 ? p[3] : 0);
        return;
    }

    g_text++;
    printf("[meta] type=%s binary=0 len=%zu mime=%s pic=%d data=\"%.*s\"\n", meta->type,
           meta->len, meta->mime ? meta->mime : "(null)", meta->pic_type,
           meta->data ? (int)meta->len : 0, meta->data ? meta->data : "");
}

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        fprintf(stderr, "usage: %s <audio-file>\n", argv[0]);
        return 2;
    }

    espaudiocore_ringbuf_t *rb = espaudiocore_ringbuf_create(256u * 1024);
    if (!rb)
    {
        fprintf(stderr, "ring buffer allocation failed\n");
        return 2;
    }

    espaudiocore_cfg_t cfg = {0};
    cfg.output_bits = 16;
    cfg.on_meta = on_meta;

    int dec_err = 0;
    int rc = espaudiocore_begin_rb(argv[1], rb, &cfg, &dec_err);
    if (rc != 0)
    {
        fprintf(stderr, "begin failed: %s (dec_err=%d)\n", espaudiocore_strerror(rc), dec_err);
        espaudiocore_ringbuf_destroy(rb);
        return 1;
    }

    uint8_t buf[16384];
    struct timespec t0, now;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;)
    {
        (void)espaudiocore_ringbuf_read(rb, buf, sizeof(buf), 20);
        if (!espaudiocore_is_running() && espaudiocore_ringbuf_used(rb) == 0)
        {
            break;
        }
        clock_gettime(CLOCK_MONOTONIC, &now);
        double elapsed = (double)(now.tv_sec - t0.tv_sec) +
                         (double)(now.tv_nsec - t0.tv_nsec) / 1e9;
        if (elapsed > 120.0)
        {
            fprintf(stderr, "decode timed out\n");
            espaudiocore_stop();
            espaudiocore_ringbuf_destroy(rb);
            return 1;
        }
    }
    espaudiocore_stop();
    espaudiocore_ringbuf_destroy(rb);

    printf("[meta] summary text=%d pictures=%d\n", g_text, g_pictures);
    return 0;
}
