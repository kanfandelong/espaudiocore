/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 kanfandelong. All rights reserved.
 */

/**
 * @file    audio_player.c
 * @brief   Player state machine and decoder thread (pthread implementation).
 *
 * Responsibilities:
 *  - own every resource of the single playback session
 *  - decoder auto-routing (extension first, magic fallback)
 *  - serialize pause / seek / stop against the decode loop
 *  - reconfigure the ALSA device when the sample rate changes
 *
 * Threading:
 *  - the application calls begin/stop/pause/resume/seek from its control
 *    thread; those calls only set atomics and wait
 *  - the decoder thread owns the decoder and the sink write path; seek and
 *    reset happen there
 *  - every blocking point observes stop_req, so stop() is a bounded join
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "audio_decoder_route.h"
#include "audio_player.h"
#include "audio_port.h"
#include "audio_sink.h"
#include "audio_source.h"
#include "audio_types.h"
#include "decoder.h"

#include <errno.h>
#include <new>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <atomic>

/* ===========================================================================
 * Session context
 * =========================================================================*/

struct PlayerCtx
{
    AudioInput   *in = nullptr;
    AudioSink    *sink = nullptr;
    AudioDecoder *dec = nullptr;

    espaudiocore_cfg_t cfg = {};

    std::atomic<int64_t> duration_ms{-1};
    bool device_output = false;
    int  dec_err = AUDIO_DEC_ERR_NONE;

    pthread_t thread{};
    bool      thread_started = false;
    std::atomic<bool> thread_done{false};

    std::atomic<bool>    stop_req{false};
    std::atomic<bool>    pause_req{false};
    std::atomic<bool>    seek_req{false};
    std::atomic<int64_t> seek_target_ms{0};

    /* Guards the snapshot below and the seek/pause anchors. All of these are
     * 64-bit and read from the control thread. */
    pthread_mutex_t state_lock{};
    bool            state_lock_init = false;

    uint64_t seek_base_frames = 0;
    int64_t  seek_base_ms = 0;

    uint32_t snap_rate = 0;
    uint8_t  snap_channels = 0;
    uint8_t  snap_bits = 0;
    uint64_t snap_frames = 0;

    int64_t pause_position_ms = 0;

    /**
     * @brief Metadata trampoline: intercept "tlen" duration updates before
     *        forwarding to the user callback.
     *
     * MP3/AAC report an estimated duration through metadata while decoding.
     * Keeping it in sync avoids get_duration_ms() disagreeing with the meta
     * callback (the ESP-IDF build had exactly that bug).
     */
    static void meta_trampoline(void *user, const espaudiocore_meta_t *meta)
    {
        PlayerCtx *c = static_cast<PlayerCtx *>(user);
        if (c && meta && meta->type && meta->data && strcmp(meta->type, "tlen") == 0)
        {
            int64_t v = (int64_t)atoll(meta->data);
            if (v > 0)
            {
                c->duration_ms.store(v, std::memory_order_relaxed);
            }
        }
        if (c && c->cfg.on_meta)
        {
            c->cfg.on_meta(c->cfg.user, meta);
        }
    }
};

static PlayerCtx *s_ctx = nullptr;

/* ===========================================================================
 * Resource lifetime (only after the decoder thread has exited)
 * =========================================================================*/

static void ctx_destroy(PlayerCtx *c)
{
    if (!c)
    {
        return;
    }
    if (c->dec)
    {
        c->dec->close();
        delete c->dec;
        c->dec = nullptr;
    }
    if (c->sink)
    {
        delete c->sink;
        c->sink = nullptr;
    }
    if (c->in)
    {
        delete c->in;
        c->in = nullptr;
    }
    if (c->state_lock_init)
    {
        pthread_mutex_destroy(&c->state_lock);
        c->state_lock_init = false;
    }
    if (s_ctx == c)
    {
        s_ctx = nullptr;
    }
    delete c;
}

/** @brief Request stop and join the decoder thread. Returns 0 or -EDEADLK. */
static int ctx_stop_and_join(PlayerCtx *c)
{
    if (!c)
    {
        return 0;
    }
    if (c->thread_started && pthread_equal(pthread_self(), c->thread))
    {
        AUDIO_LOGE("espaudiocore_stop() called from the decoder thread; refusing to join");
        return -EDEADLK;
    }

    c->stop_req.store(true, std::memory_order_relaxed);
    c->pause_req.store(false, std::memory_order_relaxed);

    if (c->thread_started)
    {
        pthread_join(c->thread, nullptr);
        c->thread_started = false;
    }
    ctx_destroy(c);
    return 0;
}

/* ===========================================================================
 * Position helper (state_lock held)
 * =========================================================================*/

static int64_t position_locked(PlayerCtx *c)
{
    if (!c->snap_rate)
    {
        return 0;
    }
    uint64_t since = (c->snap_frames >= c->seek_base_frames)
                         ? (c->snap_frames - c->seek_base_frames)
                         : 0;
    if (c->device_output && c->sink)
    {
        size_t pending = audio_sink_pending_frames(c->sink);
        if (pending < since)
        {
            since -= pending;
        }
        else
        {
            since = 0;
        }
    }
    return c->seek_base_ms + (int64_t)((since * 1000ULL) / c->snap_rate);
}

static void snapshot_update(PlayerCtx *c)
{
    if (!c->dec)
    {
        return;
    }
    const audio_format_t &f = c->dec->format();
    pthread_mutex_lock(&c->state_lock);
    c->snap_rate = f.rate;
    c->snap_channels = f.channels;
    c->snap_bits = f.bits;
    c->snap_frames = c->sink ? c->sink->played_frames() : 0;
    pthread_mutex_unlock(&c->state_lock);
}

/* ===========================================================================
 * Decoder thread
 * =========================================================================*/

static void *decode_task(void *arg)
{
    PlayerCtx *c = static_cast<PlayerCtx *>(arg);

#if defined(__linux__)
    (void)pthread_setname_np(pthread_self(), "eac-decode");
#endif

    AUDIO_LOGI("decode thread started");

    bool eos = false;
    audio_err_t err = AUDIO_OK;

    while (!c->stop_req.load(std::memory_order_relaxed))
    {
        if (c->pause_req.load(std::memory_order_relaxed))
        {
            usleep(10000);
            continue;
        }

        if (c->seek_req.load(std::memory_order_relaxed))
        {
            int64_t target_ms = c->seek_target_ms.load(std::memory_order_relaxed);

            if (!c->in->can_seek())
            {
                AUDIO_LOGW("seek not supported by this source");
                if (c->cfg.on_event)
                {
                    c->cfg.on_event(c->cfg.user, ESPAUDIOCORE_EVT_ERROR, "seek not supported");
                }
            }
            else
            {
                int seek_result = (int)c->dec->seek_ms(target_ms);
                if (seek_result == AUDIO_OK)
                {
                    /* Discard PCM that was queued before the seek so the
                     * listener does not hear the old position's tail. */
                    if (c->sink)
                    {
                        (void)c->sink->discard();
                    }
                    pthread_mutex_lock(&c->state_lock);
                    c->seek_base_frames = c->sink->played_frames();
                    c->seek_base_ms = target_ms;
                    pthread_mutex_unlock(&c->state_lock);
                    c->dec->reset();
                    if (c->cfg.on_event)
                    {
                        c->cfg.on_event(c->cfg.user, ESPAUDIOCORE_EVT_SEEKED, "seeked");
                    }
                }
                else
                {
                    if (c->cfg.on_event)
                    {
                        c->cfg.on_event(c->cfg.user, ESPAUDIOCORE_EVT_ERROR, "seek error");
                    }
                    AUDIO_LOGW("seek failed: %lld ms (rc=%d)", (long long)target_ms, seek_result);
                }
            }
            c->seek_req.store(false, std::memory_order_relaxed);
        }

        audio_err_t r = c->dec->decode();

        /* Format changes: ALSA output is reconfigured here; the ring-buffer
         * output only records the values for get_format(). */
        const audio_format_t &f = c->dec->format();
        if (c->sink && f.rate && c->sink->format_changed(f))
        {
            c->sink->set_rate(f.rate);
            c->sink->set_format(f.bits, f.channels);
        }

        snapshot_update(c);

        if (r == AUDIO_ERR_EOF)
        {
            eos = true;
            break;
        }
        if (r != AUDIO_OK)
        {
            err = r;
            break;
        }
    }

    if (c->stop_req.load(std::memory_order_relaxed))
    {
        AUDIO_LOGI("decode loop stopped by request");
    }
    else if (eos)
    {
        AUDIO_LOGI("decode finished: end of stream");
        /* Play out whatever is still buffered before reporting EOS. */
        if (c->sink)
        {
            (void)c->sink->flush();
        }
        if (c->cfg.on_event)
        {
            c->cfg.on_event(c->cfg.user, ESPAUDIOCORE_EVT_EOS, "end of stream");
        }
    }
    else
    {
        AUDIO_LOGE("decode failed: %s", audio_err_name(err));
        if (c->cfg.on_event)
        {
            char msg[64];
            snprintf(msg, sizeof(msg), "decode failed: %s", audio_err_name(err));
            c->cfg.on_event(c->cfg.user, ESPAUDIOCORE_EVT_ERROR, msg);
        }
    }

    /* Always report STOPPED so the application can clean up in one branch. */
    if (c->cfg.on_event)
    {
        c->cfg.on_event(c->cfg.user, ESPAUDIOCORE_EVT_STOPPED, "stopped");
    }

    c->thread_done.store(true, std::memory_order_release);
    AUDIO_LOGI("decode thread exiting");
    return nullptr;
}

/* ===========================================================================
 * Common begin implementation
 * =========================================================================*/

static int player_begin(AudioInput *in, const audio_decoder_entry_t *entry, const char *alsa_device,
                        espaudiocore_ringbuf_t *out_rb, const espaudiocore_cfg_t *cfg, int *dec_err,
                        bool device_output)
{
    if (!in)
    {
        return -EINVAL;
    }
    if (dec_err)
    {
        *dec_err = AUDIO_DEC_ERR_NONE;
    }
    if (!entry || !entry->create)
    {
        delete in;
        if (dec_err)
        {
            *dec_err = AUDIO_DEC_ERR_OPEN;
        }
        return -ENOTSUP;
    }

    if (s_ctx)
    {
        int rc = ctx_stop_and_join(s_ctx);
        if (rc != 0)
        {
            delete in;
            return rc;
        }
    }

    PlayerCtx *c = new (std::nothrow) PlayerCtx();
    if (!c)
    {
        delete in;
        return -ENOMEM;
    }
    if (pthread_mutex_init(&c->state_lock, nullptr) != 0)
    {
        delete c;
        delete in;
        return -ENOMEM;
    }
    c->state_lock_init = true;

    if (cfg)
    {
        c->cfg = *cfg;
    }
    c->in = in;
    c->device_output = device_output;

    if (device_output)
    {
        const char *dev = (alsa_device && alsa_device[0]) ? alsa_device : "default";
        c->sink = audio_sink_alsa_create(dev, c->cfg.io_timeout_ms, c->cfg.output_bits,
                                         c->cfg.alsa_latency_ms);
        if (!c->sink)
        {
            AUDIO_LOGE("cannot create ALSA output sink");
            ctx_destroy(c);
            return -EIO;
        }
    }
    else
    {
        if (!out_rb)
        {
            ctx_destroy(c);
            return -EINVAL;
        }
        c->sink = audio_sink_ringbuf_create(out_rb, c->cfg.output_bits);
        if (!c->sink)
        {
            ctx_destroy(c);
            return -ENOMEM;
        }
    }

    c->sink->set_volume_db(c->cfg.volume_db);

    c->dec = entry->create();
    if (!c->dec)
    {
        ctx_destroy(c);
        if (dec_err)
        {
            *dec_err = AUDIO_DEC_ERR_OPEN;
        }
        return -ENOTSUP;
    }

    /* Open: parse headers, emit metadata, fill in format/duration. */
    audio_format_t fmt = {};
    int64_t duration_ms = -1;
    audio_err_t r = c->dec->open(c->in, c->sink, PlayerCtx::meta_trampoline, c, &fmt,
                                 &duration_ms, &c->dec_err);
    if (r != AUDIO_OK)
    {
        int rc = audio_err_to_code(r);
        if (dec_err)
        {
            *dec_err = c->dec_err;
        }
        ctx_destroy(c);
        return rc;
    }
    /* Keep a duration that the meta callback ("tlen") may already have
     * reported during open(); otherwise use the decoder's return value. */
    int64_t reported_duration = duration_ms > 0 ? duration_ms : c->duration_ms.load();
    if (reported_duration == 0)
    {
        reported_duration = -1;
    }
    c->duration_ms.store(reported_duration, std::memory_order_relaxed);

    /* Initial output configuration. */
    c->sink->set_rate(c->dec->format().rate);
    c->sink->set_format(c->dec->format().bits, c->dec->format().channels);
    if (device_output)
    {
        int prc = c->sink->prepare();
        if (prc != AUDIO_OK)
        {
            AUDIO_LOGE("cannot configure ALSA output");
            ctx_destroy(c);
            return -EIO;
        }
    }

    if (device_output && c->cfg.on_pcm)
    {
        audio_sink_alsa_set_pcm(c->sink, c->cfg.on_pcm, c->cfg.user);
    }

    if (device_output)
    {
        audio_sink_alsa_bind_stop(c->sink, &c->stop_req);
    }
    else
    {
        audio_sink_ringbuf_bind_stop(c->sink, &c->stop_req);
    }

    snapshot_update(c);

    s_ctx = c;

    if (pthread_create(&c->thread, nullptr, decode_task, c) != 0)
    {
        AUDIO_LOGE("failed to create decode thread");
        s_ctx = nullptr;
        ctx_destroy(c);
        return -ENOMEM;
    }
    c->thread_started = true;

    const audio_format_t &f = c->dec->format();
    AUDIO_LOGI("playing %s: %u Hz, %u ch, %u-bit, duration=%lld ms", entry->name,
               (unsigned)f.rate, (unsigned)f.channels, (unsigned)f.bits,
               (long long)c->duration_ms.load());

    if (c->cfg.on_event)
    {
        c->cfg.on_event(c->cfg.user, ESPAUDIOCORE_EVT_STARTED, "started");
    }
    return 0;
}

/* ===========================================================================
 * Internal interface implementation
 * =========================================================================*/

int audio_player_start_file(const char *path, const char *alsa_device,
                            espaudiocore_ringbuf_t *out_rb, const espaudiocore_cfg_t *cfg,
                            int *dec_err, bool device_output)
{
    if (dec_err)
    {
        *dec_err = AUDIO_DEC_ERR_NONE;
    }
    if (!path || !path[0])
    {
        AUDIO_LOGE("empty path");
        return -EINVAL;
    }

    AudioInput *in = audio_source_fs_create(path);
    if (!in)
    {
        if (dec_err)
        {
            *dec_err = AUDIO_DEC_ERR_OPEN;
        }
        return -ENOENT;
    }

    const audio_decoder_entry_t *entry = audio_decoder_route(in, path);
    return player_begin(in, entry, alsa_device, out_rb, cfg, dec_err, device_output);
}

int audio_player_start_stream(espaudiocore_ringbuf_t *in_rb, espaudiocore_format_t fmt,
                              const char *alsa_device, espaudiocore_ringbuf_t *out_rb,
                              const espaudiocore_cfg_t *cfg, int *dec_err, bool device_output)
{
    if (dec_err)
    {
        *dec_err = AUDIO_DEC_ERR_NONE;
    }
    if (!in_rb)
    {
        return -EINVAL;
    }

    AudioInput *in = audio_source_ringbuf_create(in_rb);
    if (!in)
    {
        return -ENOMEM;
    }

    /* Sequential stream: no probing, the caller must supply the format. */
    const audio_decoder_entry_t *entry = audio_decoder_for_format(fmt);
    if (!entry)
    {
        delete in;
        if (dec_err)
        {
            *dec_err = AUDIO_DEC_ERR_OPEN;
        }
        return -ENOTSUP;
    }

    return player_begin(in, entry, alsa_device, out_rb, cfg, dec_err, device_output);
}

int audio_player_stop()
{
    if (!s_ctx)
    {
        return -EPERM;
    }
    return ctx_stop_and_join(s_ctx) == 0 ? 0 : -EPERM;
}

int audio_player_pause()
{
    if (!s_ctx)
    {
        return -EPERM;
    }
    if (s_ctx->pause_req.load(std::memory_order_relaxed))
    {
        return 0;
    }

    pthread_mutex_lock(&s_ctx->state_lock);
    s_ctx->pause_position_ms = position_locked(s_ctx);
    pthread_mutex_unlock(&s_ctx->state_lock);

    s_ctx->pause_req.store(true, std::memory_order_relaxed);
    if (s_ctx->sink)
    {
        (void)s_ctx->sink->pause(true);
    }

    if (s_ctx->cfg.on_event)
    {
        s_ctx->cfg.on_event(s_ctx->cfg.user, ESPAUDIOCORE_EVT_PAUSED, "paused");
    }
    return 0;
}

int audio_player_resume()
{
    if (!s_ctx)
    {
        return -EPERM;
    }
    if (!s_ctx->pause_req.load(std::memory_order_relaxed))
    {
        return 0;
    }

    /* Continue counting from where the pause froze the clock. */
    pthread_mutex_lock(&s_ctx->state_lock);
    s_ctx->seek_base_frames = s_ctx->snap_frames;
    s_ctx->seek_base_ms = s_ctx->pause_position_ms;
    pthread_mutex_unlock(&s_ctx->state_lock);

    s_ctx->pause_req.store(false, std::memory_order_relaxed);
    if (s_ctx->sink)
    {
        (void)s_ctx->sink->pause(false);
    }

    if (s_ctx->cfg.on_event)
    {
        s_ctx->cfg.on_event(s_ctx->cfg.user, ESPAUDIOCORE_EVT_RESUMED, "resumed");
    }
    return 0;
}

int audio_player_seek_ms(int64_t ms)
{
    if (!s_ctx)
    {
        return -EPERM;
    }
    if (ms < 0)
    {
        return -EINVAL;
    }
    if (!s_ctx->in->can_seek())
    {
        return -ENOTSUP;
    }

    /* Only register the request; the decoder thread executes it serially. */
    s_ctx->seek_target_ms.store(ms, std::memory_order_relaxed);
    s_ctx->seek_req.store(true, std::memory_order_relaxed);

    /* Bounded wait until the request is consumed, so the caller observes the
     * new position when this returns. */
    for (int i = 0; i < 200 && s_ctx->seek_req.load(std::memory_order_relaxed); i++)
    {
        usleep(1000);
    }
    if (s_ctx->seek_req.load(std::memory_order_relaxed))
    {
        return -ETIMEDOUT;
    }
    return 0;
}

int64_t audio_player_get_position_ms()
{
    if (!s_ctx)
    {
        return 0;
    }
    pthread_mutex_lock(&s_ctx->state_lock);
    int64_t pos;
    if (s_ctx->pause_req.load(std::memory_order_relaxed))
    {
        pos = s_ctx->pause_position_ms;
    }
    else
    {
        pos = position_locked(s_ctx);
    }
    pthread_mutex_unlock(&s_ctx->state_lock);
    return pos;
}

int audio_player_set_volume_db(float db)
{
    if (!s_ctx || !s_ctx->sink)
    {
        return -EPERM;
    }
    s_ctx->sink->set_volume_db(db);
    s_ctx->cfg.volume_db = db;
    return 0;
}

float audio_player_get_volume_db()
{
    return (s_ctx && s_ctx->sink) ? s_ctx->sink->volume_db() : 0.0f;
}

int64_t audio_player_get_duration_ms()
{
    return s_ctx ? s_ctx->duration_ms.load(std::memory_order_relaxed) : -1;
}

int audio_player_get_format(uint32_t *rate, uint8_t *channels, uint8_t *bits)
{
    if (!s_ctx)
    {
        return -EPERM;
    }
    pthread_mutex_lock(&s_ctx->state_lock);
    uint32_t r = s_ctx->snap_rate;
    uint8_t ch = s_ctx->snap_channels;
    uint8_t b = s_ctx->snap_bits;
    pthread_mutex_unlock(&s_ctx->state_lock);

    if (!r)
    {
        return -EPERM;
    }
    if (rate)
    {
        *rate = r;
    }
    if (channels)
    {
        *channels = ch;
    }
    if (bits)
    {
        *bits = b;
    }
    return 0;
}

bool audio_player_is_running()
{
    return s_ctx && !s_ctx->stop_req.load(std::memory_order_relaxed) &&
           !s_ctx->thread_done.load(std::memory_order_acquire);
}

bool audio_player_is_device_output()
{
    return s_ctx && s_ctx->device_output;
}

int audio_player_last_decoder_error()
{
    return s_ctx ? s_ctx->dec_err : AUDIO_DEC_ERR_NONE;
}

AudioSink *audio_player_sink()
{
    return s_ctx ? s_ctx->sink : nullptr;
}
