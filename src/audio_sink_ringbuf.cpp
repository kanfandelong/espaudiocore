/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 kanfandelong. All rights reserved.
 */

/**
 * @file    audio_sink_ringbuf.c
 * @brief   Ring-buffer output sink (PCM frames, no hardware involved).
 *
 * Rate changes are not applied to anything; they are reported to the
 * application through espaudiocore_get_format(). Writes go through the
 * public ring buffer, which is thread safe and interruptible by the session
 * stop flag.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "audio_port.h"
#include "audio_ringbuf.h"
#include "audio_sink.h"
#include "audio_types.h"

#include <atomic>
#include <new>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/** Chunk size for one ring write. Must be far below the ring capacity so a
 *  write can make progress even when the ring is nearly full. */
#define RAW_SEND_CHUNK 4096

class AudioSinkRingbuf : public AudioSink
{
public:
    AudioSinkRingbuf(espaudiocore_ringbuf_t *rb, uint8_t target_bits)
        : rb_(rb), target_bits_(target_bits)
    {
    }

    ~AudioSinkRingbuf() override
    {
        if (rb_)
        {
            audio_ringbuf_set_stop(rb_, nullptr);
        }
        audio_free(tmp32_);
        tmp32_ = nullptr;
        tmp32_cap_ = 0;
    }

    bool init()
    {
        return rb_ != nullptr;
    }

    void set_stop_flag(const std::atomic<bool> *flag)
    {
        stop_flag_ = flag;
        if (rb_)
        {
            audio_ringbuf_set_stop(rb_, flag);
        }
    }

    int write(const void *frames, size_t bytes, uint32_t timeout_ms) override
    {
        (void)timeout_ms;
        if (!rb_ || !frames || bytes == 0)
        {
            return AUDIO_OK;
        }

        const uint8_t sb = cur_bits ? cur_bits : 16;
        if (sb != 16 && sb != 24 && sb != 32)
        {
            return AUDIO_ERR_NOT_SUPPORTED;
        }
        const size_t src_bytes = sb / 8;
        const size_t samples = bytes / src_bytes;
        if (samples < 2)
        {
            return AUDIO_OK;
        }

        size_t done = 0;
        while (done + 1 < samples)
        {
            size_t n = samples - done;
            if (n > kConvSamples)
            {
                n = kConvSamples;
            }
            n &= ~(size_t)1;

            int32_t *stage = ensure32(n);
            if (!stage)
            {
                return AUDIO_ERR_NO_MEM;
            }
            size_t nb = convert_bits((const uint8_t *)frames + done * src_bytes, n, sb, stage,
                                     n * sizeof(int32_t), 32);
            if (nb == 0)
            {
                return AUDIO_ERR_IO;
            }
            int rc = write_i32(stage, n / 2, 32, 0);
            if (rc != AUDIO_OK)
            {
                return rc;
            }
            done += n;
        }
        return AUDIO_OK;
    }

    int write_i32(const int32_t *pcm, size_t pairs, uint8_t src_bits,
                  uint32_t timeout_ms) override
    {
        (void)timeout_ms;
        if (!rb_ || !pcm || pairs == 0)
        {
            return AUDIO_OK;
        }

        const uint8_t ob = target_bits_ ? target_bits_
                                        : (cur_bits ? cur_bits : (src_bits ? src_bits : 16));
        if (ob != 16 && ob != 24 && ob != 32)
        {
            return AUDIO_ERR_NOT_SUPPORTED;
        }

        const size_t total = pairs * 2;
        const bool unity = gain_is_unity();
        const uint32_t gain = gain_q16();
        size_t done = 0;

        while (done < total)
        {
            size_t n = total - done;
            if (n > kConvSamples)
            {
                n = kConvSamples;
            }
            const int32_t *src = pcm + done;

            int rc;
            if (unity)
            {
                if (ob == 32)
                {
                    rc = raw_send(src, n * sizeof(int32_t));
                }
                else
                {
                    size_t nb = convert_bits(src, n, 32, conv_, sizeof(conv_), ob);
                    rc = nb ? raw_send(conv_, nb) : AUDIO_ERR_IO;
                }
            }
            else
            {
                int32_t *scaled = ensure32(n);
                if (!scaled)
                {
                    return AUDIO_ERR_NO_MEM;
                }
                for (size_t i = 0; i < n; i++)
                {
                    int64_t v = ((int64_t)src[i] * (int64_t)gain) >> 16;
                    if (v > INT32_MAX)
                    {
                        v = INT32_MAX;
                    }
                    else if (v < INT32_MIN)
                    {
                        v = INT32_MIN;
                    }
                    scaled[i] = (int32_t)v;
                }
                if (ob == 32)
                {
                    rc = raw_send(scaled, n * sizeof(int32_t));
                }
                else
                {
                    size_t nb = convert_bits(scaled, n, 32, conv_, sizeof(conv_), ob);
                    rc = nb ? raw_send(conv_, nb) : AUDIO_ERR_IO;
                }
            }
            if (rc != AUDIO_OK)
            {
                return rc;
            }
            done += n;
        }

        account_write(pairs * 2 * (size_t)(ob / 8), ob);
        return AUDIO_OK;
    }

    int set_rate(uint32_t hz) override
    {
        if (hz && hz != cur_rate)
        {
            cur_rate = hz;
            AUDIO_LOGI("ring output: sample rate -> %u Hz (application must handle)", (unsigned)hz);
        }
        return AUDIO_OK;
    }

    int set_format(uint8_t bits, uint8_t channels) override
    {
        if (bits != cur_bits || channels != cur_channels)
        {
            cur_bits = bits;
            cur_channels = channels;
        }
        return AUDIO_OK;
    }

    int flush() override
    {
        return AUDIO_OK;
    }

    int pause(bool on) override
    {
        (void)on;
        return AUDIO_OK;
    }

private:
    static const size_t kConvSamples = 8192;

    int32_t *ensure32(size_t nsamp)
    {
        if (!tmp32_ || tmp32_cap_ < nsamp)
        {
            audio_free(tmp32_);
            tmp32_ = (int32_t *)audio_alloc_big(nsamp * sizeof(int32_t), 0);
            tmp32_cap_ = tmp32_ ? nsamp : 0;
        }
        return tmp32_;
    }

    int raw_send(const void *data, size_t bytes)
    {
        if (!rb_ || !data || bytes == 0)
        {
            return AUDIO_OK;
        }
        const uint8_t *p = (const uint8_t *)data;
        size_t left = bytes;

        while (left > 0)
        {
            size_t chunk = left > RAW_SEND_CHUNK ? RAW_SEND_CHUNK : left;
            size_t n = espaudiocore_ringbuf_write(rb_, p, chunk, 100);
            if (n > 0)
            {
                p += n;
                left -= n;
                continue;
            }
            if (stop_flag_ && stop_flag_->load(std::memory_order_relaxed))
            {
                return AUDIO_ERR_NOT_SUPPORTED;
            }
            /* Timeout: the consumer is behind. Retry until data is accepted,
             * the session stops, or the ring buffer itself is destroyed. */
        }
        return AUDIO_OK;
    }

    espaudiocore_ringbuf_t *rb_ = nullptr;
    uint8_t target_bits_ = 0;
    const std::atomic<bool> *stop_flag_ = nullptr;

    int32_t *tmp32_ = nullptr;
    size_t tmp32_cap_ = 0;

    /** Bit-depth conversion scratch (stack, inside the object).
     *  8192 samples * 4 bytes = 32768, enough for 24-bit packed output. */
    uint8_t conv_[32768];
};

AudioSink *audio_sink_ringbuf_create(espaudiocore_ringbuf_t *rb, uint8_t target_bits)
{
    AudioSinkRingbuf *s = new (std::nothrow) AudioSinkRingbuf(rb, target_bits);
    if (!s)
    {
        return nullptr;
    }
    if (!s->init())
    {
        delete s;
        return nullptr;
    }
    return s;
}

void audio_sink_ringbuf_bind_stop(AudioSink *sink, const std::atomic<bool> *flag)
{
    AudioSinkRingbuf *rb = static_cast<AudioSinkRingbuf *>(sink);
    if (rb)
    {
        rb->set_stop_flag(flag);
    }
}
