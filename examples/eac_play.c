/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 kanfandelong. All rights reserved.
 *
 * eac_play -- minimal native Linux example / smoke-test tool.
 *
 *   eac_play song.flac                          # play through "default"
 *   eac_play song.mp3 --device plughw:0,0       # RK3506 speaker
 *   eac_play song.ogg --raw-out /tmp/out.pcm    # decode to a ring buffer
 *   eac_play song.wav --seconds 3 --bits 16
 *
 * The tool only uses the public header, which doubles as a compile check for
 * the installed API.
 */

#define _GNU_SOURCE

#include "espaudiocore.h"

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static volatile sig_atomic_t g_interrupted = 0;
static volatile sig_atomic_t g_error = 0;

static void on_signal(int sig)
{
    (void)sig;
    g_interrupted = 1;
}

static void on_event(void *user, espaudiocore_event_t ev, const char *msg)
{
    (void)user;
    const char *name = "?";
    switch (ev)
    {
    case ESPAUDIOCORE_EVT_STARTED: name = "STARTED"; break;
    case ESPAUDIOCORE_EVT_PAUSED:  name = "PAUSED";  break;
    case ESPAUDIOCORE_EVT_RESUMED: name = "RESUMED"; break;
    case ESPAUDIOCORE_EVT_SEEKED:  name = "SEEKED";  break;
    case ESPAUDIOCORE_EVT_EOS:     name = "EOS";     break;
    case ESPAUDIOCORE_EVT_STOPPED: name = "STOPPED"; break;
    case ESPAUDIOCORE_EVT_ERROR:
        name = "ERROR";
        g_error = 1;
        break;
    }
    fprintf(stderr, "[event] %-8s %s\n", name, msg ? msg : "");
}

static void on_meta(void *user, const espaudiocore_meta_t *meta)
{
    (void)user;
    if (!meta || !meta->type || meta->is_binary)
    {
        return;
    }
    if (strcmp(meta->type, "Title") == 0 || strcmp(meta->type, "Artist") == 0 ||
        strcmp(meta->type, "Album") == 0)
    {
        fprintf(stderr, "[meta ] %s=%s\n", meta->type, meta->data ? meta->data : "");
    }
}

static void usage(const char *argv0)
{
    fprintf(stderr,
            "usage: %s <file> [options]\n"
            "  -d, --device <alsa>   ALSA device (default: default; board: plughw:0,0)\n"
            "  -b, --bits <16|24|32> fixed output bit depth (default: follow source)\n"
            "  -v, --volume <db>     volume in dB (default: 0)\n"
            "  -t, --seconds <n>     stop after n seconds (default: play to EOS)\n"
            "  -s, --seek <ms>       seek to this position before playing\n"
            "  -r, --raw-out <path>  decode to a ring buffer and write raw PCM\n"
            "  -q, --quiet           only errors\n"
            "  -h, --help            this text\n",
            argv0);
}

static int parse_int(const char *s, long *out)
{
    char *end = NULL;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno || !end || *end != '\0')
    {
        return -1;
    }
    *out = v;
    return 0;
}

int main(int argc, char **argv)
{
    const char *path = NULL;
    const char *device = NULL;
    const char *raw_out = NULL;
    long bits = 0;
    double volume = 0.0;
    long seconds = 0;
    long seek_ms = 0;
    int quiet = 0;

    for (int i = 1; i < argc; i++)
    {
        const char *a = argv[i];
        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0)
        {
            usage(argv[0]);
            return 0;
        }
        else if ((strcmp(a, "-d") == 0 || strcmp(a, "--device") == 0) && i + 1 < argc)
        {
            device = argv[++i];
        }
        else if ((strcmp(a, "-b") == 0 || strcmp(a, "--bits") == 0) && i + 1 < argc)
        {
            if (parse_int(argv[++i], &bits) != 0 || (bits != 0 && bits != 16 && bits != 24 &&
                                                      bits != 32))
            {
                fprintf(stderr, "invalid --bits\n");
                return 2;
            }
        }
        else if ((strcmp(a, "-v") == 0 || strcmp(a, "--volume") == 0) && i + 1 < argc)
        {
            char *end = NULL;
            volume = strtod(argv[++i], &end);
            if (!end || *end != '\0')
            {
                fprintf(stderr, "invalid --volume\n");
                return 2;
            }
        }
        else if ((strcmp(a, "-t") == 0 || strcmp(a, "--seconds") == 0) && i + 1 < argc)
        {
            if (parse_int(argv[++i], &seconds) != 0 || seconds < 0)
            {
                fprintf(stderr, "invalid --seconds\n");
                return 2;
            }
        }
        else if ((strcmp(a, "-s") == 0 || strcmp(a, "--seek") == 0) && i + 1 < argc)
        {
            if (parse_int(argv[++i], &seek_ms) != 0 || seek_ms < 0)
            {
                fprintf(stderr, "invalid --seek\n");
                return 2;
            }
        }
        else if ((strcmp(a, "-r") == 0 || strcmp(a, "--raw-out") == 0) && i + 1 < argc)
        {
            raw_out = argv[++i];
        }
        else if (strcmp(a, "-q") == 0 || strcmp(a, "--quiet") == 0)
        {
            quiet = 1;
        }
        else if (a[0] == '-')
        {
            fprintf(stderr, "unknown option: %s\n", a);
            usage(argv[0]);
            return 2;
        }
        else if (!path)
        {
            path = a;
        }
        else
        {
            fprintf(stderr, "more than one input file\n");
            return 2;
        }
    }

    if (!path)
    {
        usage(argv[0]);
        return 2;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    espaudiocore_set_log_level(quiet ? ESPAUDIOCORE_LOG_WARN : ESPAUDIOCORE_LOG_INFO);

    espaudiocore_cfg_t cfg = {0};
    cfg.alsa_device = device; /* NULL => "default" */
    cfg.output_bits = (uint8_t)bits;
    cfg.volume_db = (float)volume;
    cfg.on_event = on_event;
    cfg.on_meta = on_meta;
    cfg.io_timeout_ms = 0;

    int dec_err = 0;
    int rc;
    const int64_t stop_at_ms = (seek_ms > 0 ? (int64_t)seek_ms : 0) + (int64_t)seconds * 1000;

    if (raw_out)
    {
        FILE *out = fopen(raw_out, "wb");
        if (!out)
        {
            fprintf(stderr, "cannot open %s: %s\n", raw_out, strerror(errno));
            return 1;
        }

        espaudiocore_ringbuf_t *rb = espaudiocore_ringbuf_create(1u << 20);
        if (!rb)
        {
            fprintf(stderr, "cannot create ring buffer\n");
            fclose(out);
            return 1;
        }

        rc = espaudiocore_begin_rb(path, rb, &cfg, &dec_err);
        if (rc != 0)
        {
            fprintf(stderr, "begin failed: %s (dec_err=%d)\n", espaudiocore_strerror(rc), dec_err);
            espaudiocore_ringbuf_destroy(rb);
            fclose(out);
            return 1;
        }

        if (seek_ms > 0)
        {
            rc = espaudiocore_seek_ms((int64_t)seek_ms);
            if (rc != 0)
            {
                fprintf(stderr, "seek failed: %s\n", espaudiocore_strerror(rc));
                espaudiocore_stop();
                espaudiocore_ringbuf_destroy(rb);
                fclose(out);
                return 1;
            }
        }

        uint32_t rate = 0;
        uint8_t channels = 0, out_bits = 0;
        (void)espaudiocore_get_format(&rate, &channels, &out_bits);
        if (!quiet)
        {
            fprintf(stderr, "format: %u Hz, %u ch, %u-bit, duration=%lld ms -> %s\n",
                    (unsigned)rate, (unsigned)channels, (unsigned)out_bits,
                    (long long)espaudiocore_get_duration_ms(), raw_out);
        }

        uint8_t buf[65536];
        while (!g_interrupted && !g_error)
        {
            if (seconds > 0 && espaudiocore_get_position_ms() >= stop_at_ms)
            {
                break;
            }
            size_t n = espaudiocore_ringbuf_read(rb, buf, sizeof(buf), 200);
            if (n > 0)
            {
                if (fwrite(buf, 1, n, out) != n)
                {
                    fprintf(stderr, "write failed: %s\n", strerror(errno));
                    g_error = 1;
                    break;
                }
                continue;
            }
            if (!espaudiocore_is_running())
            {
                /* Drain whatever is still queued, then finish. */
                n = espaudiocore_ringbuf_read(rb, buf, sizeof(buf), 0);
                if (n == 0)
                {
                    break;
                }
                if (fwrite(buf, 1, n, out) != n)
                {
                    g_error = 1;
                    break;
                }
            }
        }

        espaudiocore_stop();
        espaudiocore_ringbuf_destroy(rb);
        fclose(out);
        if (!quiet)
        {
            fprintf(stderr, "position=%lld ms\n", (long long)espaudiocore_get_position_ms());
        }
        return (g_error || g_interrupted) ? 1 : 0;
    }

    rc = espaudiocore_begin(path, &cfg, &dec_err);
    if (rc != 0)
    {
        fprintf(stderr, "begin failed: %s (dec_err=%d)\n", espaudiocore_strerror(rc), dec_err);
        return 1;
    }

    if (seek_ms > 0)
    {
        rc = espaudiocore_seek_ms((int64_t)seek_ms);
        if (rc != 0)
        {
            fprintf(stderr, "seek failed: %s\n", espaudiocore_strerror(rc));
            espaudiocore_stop();
            return 1;
        }
    }

    uint32_t rate = 0;
    uint8_t channels = 0, out_bits = 0;
    (void)espaudiocore_get_format(&rate, &channels, &out_bits);
    if (!quiet)
    {
        fprintf(stderr, "format: %u Hz, %u ch, %u-bit, duration=%lld ms, device=%s\n",
                (unsigned)rate, (unsigned)channels, (unsigned)out_bits,
                (long long)espaudiocore_get_duration_ms(), device ? device : "default");
    }

    int64_t last_report = 0;
    while (espaudiocore_is_running() && !g_interrupted && !g_error)
    {
        usleep(50000);
        if (seconds > 0 && espaudiocore_get_position_ms() >= stop_at_ms)
        {
            break;
        }
        int64_t pos = espaudiocore_get_position_ms();
        if (!quiet && pos / 1000 != last_report)
        {
            last_report = pos / 1000;
            fprintf(stderr, "position=%lld ms\n", (long long)pos);
        }
    }

    int64_t final_pos = espaudiocore_get_position_ms();
    espaudiocore_stop();
    if (!quiet)
    {
        fprintf(stderr, "stopped at %lld ms\n", (long long)final_pos);
    }
    return (g_error || g_interrupted) ? 1 : 0;
}
