/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 kanfandelong. All rights reserved.
 *
 * eac_selftest -- API state-machine test without an audio device.
 *
 * Decodes a file into a ring buffer (the consumer paces the producer), then
 * exercises pause / resume / seek / position / duration / EOS.
 *
 *   eac_selftest tests/assets/t.wav
 */

#define _GNU_SOURCE

#include "espaudiocore.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int g_failures = 0;
static int g_eos = 0;
static int g_stopped = 0;

#define CHECK(cond, fmt, ...)                                                                    \
    do                                                                                           \
    {                                                                                            \
        if (!(cond))                                                                             \
        {                                                                                        \
            fprintf(stderr, "FAIL: " fmt "\n", ##__VA_ARGS__);                                   \
            g_failures++;                                                                        \
        }                                                                                        \
    } while (0)

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void on_event(void *user, espaudiocore_event_t ev, const char *msg)
{
    (void)user;
    (void)msg;
    if (ev == ESPAUDIOCORE_EVT_EOS)
    {
        g_eos = 1;
    }
    else if (ev == ESPAUDIOCORE_EVT_STOPPED)
    {
        g_stopped = 1;
    }
}

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        fprintf(stderr, "usage: %s <audio-file>\n", argv[0]);
        return 2;
    }

    /* Small ring: the consumer paces the producer, so pause/seek happen while
     * the decode thread is still running. */
    const size_t capacity = 64u * 1024;
    espaudiocore_ringbuf_t *rb = espaudiocore_ringbuf_create(capacity);
    if (!rb)
    {
        fprintf(stderr, "ring buffer allocation failed\n");
        return 2;
    }

    espaudiocore_cfg_t cfg = {0};
    cfg.output_bits = 16;
    cfg.on_event = on_event;

    int dec_err = 0;
    int rc = espaudiocore_begin_rb(argv[1], rb, &cfg, &dec_err);
    if (rc != 0)
    {
        fprintf(stderr, "begin failed: %s (dec_err=%d)\n", espaudiocore_strerror(rc), dec_err);
        espaudiocore_ringbuf_destroy(rb);
        return 1;
    }

    uint32_t rate = 0;
    uint8_t channels = 0, bits = 0;
    CHECK(espaudiocore_get_format(&rate, &channels, &bits) == 0, "get_format failed");
    CHECK(rate == 44100, "rate=%u, expected 44100", (unsigned)rate);
    CHECK(channels == 2, "channels=%u, expected 2", (unsigned)channels);
    CHECK(bits == 16, "bits=%u, expected 16", (unsigned)bits);

    int64_t duration = espaudiocore_get_duration_ms();
    CHECK(duration >= 2900 && duration <= 3100, "duration=%lld, expected ~3000",
          (long long)duration);

    uint8_t buf[4096];
    const int64_t t0 = now_ms();
    int pause_done = 0, resume_done = 0, seek_done = 0;
    int64_t pause_pos = -1;
    int64_t pause_at = 0;
    int64_t after_resume_pos = -1;
    int64_t seek_pos = -1;

    for (;;)
    {
        (void)espaudiocore_ringbuf_read(rb, buf, sizeof(buf), 10);
        int64_t t = now_ms() - t0;
        if (!pause_done && t >= 400)
        {
            int64_t before = espaudiocore_get_position_ms();
            CHECK(espaudiocore_pause() == 0, "pause failed");
            pause_pos = espaudiocore_get_position_ms();
            CHECK(pause_pos == before, "pause position changed: %lld -> %lld",
                  (long long)before, (long long)pause_pos);
            pause_done = 1;
            pause_at = now_ms();
        }
        else if (pause_done && !resume_done)
        {
            int64_t p = espaudiocore_get_position_ms();
            CHECK(p == pause_pos, "position moved while paused: %lld -> %lld",
                  (long long)pause_pos, (long long)p);
            if (now_ms() - pause_at >= 300)
            {
                CHECK(espaudiocore_resume() == 0, "resume failed");
                resume_done = 1;
                after_resume_pos = espaudiocore_get_position_ms();
                CHECK(after_resume_pos >= pause_pos, "position went backwards after resume");
            }
        }
        else if (resume_done && !seek_done && t >= 900)
        {
            CHECK(espaudiocore_seek_ms(1500) == 0, "seek failed");
            seek_pos = espaudiocore_get_position_ms();
            CHECK(seek_pos >= 1400 && seek_pos <= 1800, "seek position=%lld, expected ~1500",
                  (long long)seek_pos);
            seek_done = 1;
        }

        if (!espaudiocore_is_running() && espaudiocore_ringbuf_used(rb) == 0)
        {
            break;
        }
        if (t > 20000)
        {
            CHECK(0, "test timed out");
            break;
        }
        /* 4 KiB at 44.1 kHz stereo = 23 ms of audio; sleeping 20 ms keeps the
         * consumer close to real time so the producer blocks on the ring. */
        usleep(20000);
    }

    CHECK(seek_done, "seek was never exercised");
    CHECK(g_eos, "EOS event was not delivered");
    CHECK(g_stopped, "STOPPED event was not delivered");

    int64_t final_pos = espaudiocore_get_position_ms();
    CHECK(final_pos >= 2900, "final position=%lld, expected >= 2900", (long long)final_pos);
    CHECK(espaudiocore_stop() == 0, "stop failed");

    espaudiocore_ringbuf_destroy(rb);

    if (g_failures)
    {
        fprintf(stderr, "%d self-test failure(s)\n", g_failures);
        return 1;
    }
    printf("selftest OK (rate=%u, channels=%u, bits=%u, duration=%lld ms)\n", (unsigned)rate,
           (unsigned)channels, (unsigned)bits, (long long)duration);
    return 0;
}
