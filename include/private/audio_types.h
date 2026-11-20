/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 kanfandelong. All rights reserved.
 */

/**
 * @file    audio_types.h
 * @brief   Internal shared types: PCM format, data source and data sink.
 *
 * This header contains C++ classes and may only be included from .cpp files.
 * Pure C translation units should include espaudiocore.h (public types) and
 * audio_port.h (platform layer) only.
 *
 * Layering:
 *   - public C API          espaudiocore.h (C compiler safe)
 *   - error codes           include/private/audio_err.h (C safe)
 *   - abstractions          this file and decoder.h (C++)
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <atomic>
#include <math.h>

#include "audio_err.h"
#include "audio_port.h"
#include "espaudiocore.h" /* espaudiocore_meta_t, espaudiocore_pcm_cb_t */

#ifdef __cplusplus

class AudioInput;
class AudioSink;
class AudioDecoder;

/* ===========================================================================
 * PCM format
 * =========================================================================*/

typedef struct
{
    uint32_t rate;    /**< sample rate in Hz */
    uint8_t channels; /**< channel count: 1 or 2 */
    uint8_t bits;     /**< bit depth: 16/24/32 */
} audio_format_t;

/* ===========================================================================
 * Data source
 * =========================================================================*/

/**
 * @brief Audio data source.
 *
 * Contract:
 *  - read() returns the number of bytes actually read; 0 means the stream
 *    really ended (not "temporarily no data").
 *  - Short reads are allowed; the caller loops.
 *  - peek() does not advance the logical position and tries to provide at
 *    least `need` bytes (ID3 parsing and magic probing rely on this). The
 *    ring-buffer source implements it with an internal look-ahead buffer.
 *  - When can_seek() is false, seek() returns AUDIO_ERR_NOT_SUPPORTED.
 *    Capabilities that depend on seeking (M4A container, Xing duration,
 *    FLAC seek) are unavailable in that mode.
 *  - wait_data() only makes sense for the ring-buffer source; the file
 *    source returns immediately.
 */
class AudioInput
{
public:
    virtual ~AudioInput() = default;

    const char *path = nullptr; /**< file path; NULL for non-file streams */

    virtual int read(void *dst, size_t len) = 0;
    virtual int peek(void *dst, size_t len, size_t need) = 0;
    virtual bool can_seek() const = 0;
    virtual int seek(int64_t pos, int whence) = 0; /**< whence: SEEK_SET/CUR/END */
    virtual int64_t tell() = 0;
    virtual int64_t size() const = 0; /**< <0 when unknown (stream source) */
    virtual int wait_data(uint32_t timeout_ms) = 0;

    /** Convenience helper: read exactly `len` bytes, returning the count read
     *  (a short count means EOF). */
    size_t read_full(void *dst, size_t len)
    {
        size_t got = 0;
        uint8_t *p = static_cast<uint8_t *>(dst);
        while (got < len)
        {
            int n = read(p + got, len - got);
            if (n <= 0)
            {
                break;
            }
            got += static_cast<size_t>(n);
        }
        return got;
    }
};

/* ===========================================================================
 * Data sink
 * =========================================================================*/

/**
 * @brief Audio output sink.
 *
 * Contract (important):
 *  - write() either writes all `bytes`, or gives up because a stop was
 *    requested. It must never return "0 written" while decoding continues,
 *    otherwise samples are silently dropped.
 *  - Blocking is normal back-pressure (ALSA buffer full / ring buffer full);
 *    it must be interruptible by the stop flag.
 *
 * Two entry points:
 *  1. write()     - raw bytes plus a source bit depth. Legacy path.
 *  2. write_i32() - the recommended unified path: decoders align PCM to int32
 *                   (16-bit << 16 / 24-bit << 8 / 32-bit raw), and the sink
 *                   applies volume then narrows to the output bit depth. This
 *                   keeps "decode -> scale -> output" responsibilities clean.
 */
class AudioSink
{
public:
    virtual ~AudioSink() = default;

    virtual int write(const void *frames, size_t bytes, uint32_t timeout_ms) = 0;

    /**
     * @brief Write PCM as int32 samples (each pair of samples = L,R).
     *
     * @param pcm        interleaved int32 samples, length = pairs*2
     * @param pairs      number of L/R pairs
     * @param src_bits   effective bit depth of these int32 values (16/24/32).
     *                   16 -> value already <<16; 24 -> already <<8; 32 -> raw.
     *                   The sink shifts right only when the output is narrower.
     * @param timeout_ms 0 = use the default
     *
     * Default implementation returns AUDIO_ERR_NOT_SUPPORTED; concrete sinks
     * override it.
     */
    virtual int write_i32(const int32_t *pcm, size_t pairs, uint8_t src_bits,
                          uint32_t timeout_ms)
    {
        (void)pcm;
        (void)pairs;
        (void)src_bits;
        (void)timeout_ms;
        return AUDIO_ERR_NOT_SUPPORTED;
    }

    virtual int set_rate(uint32_t hz) = 0;
    virtual int set_format(uint8_t bits, uint8_t channels) = 0;
    virtual int flush() = 0;

    /** Drop queued output without playing it (used by seek). */
    virtual int discard()
    {
        return AUDIO_OK;
    }

    /** Apply any deferred configuration (ALSA: hw_params). */
    virtual int prepare()
    {
        return AUDIO_OK;
    }

    /** Pause/resume the output device (no-op for non-device sinks). */
    virtual int pause(bool on)
    {
        (void)on;
        return AUDIO_OK;
    }

    /** True when the current configuration differs from the target format. */
    bool format_changed(const audio_format_t &fmt) const
    {
        return (fmt.rate != cur_rate) || (fmt.channels != cur_channels) || (fmt.bits != cur_bits);
    }

    uint32_t cur_rate = 0;
    uint8_t cur_channels = 0;
    uint8_t cur_bits = 0;

    /** Set volume in dB. Atomic so the control thread may change it while the
     *  decoder thread is writing samples. */
    virtual void set_volume_db(float db)
    {
        volume_db_.store(db, std::memory_order_relaxed);
        gain_q16_.store(db_to_gain_q16(db), std::memory_order_relaxed);
    }
    float volume_db() const { return volume_db_.load(std::memory_order_relaxed); }
    uint32_t gain_q16() const { return gain_q16_.load(std::memory_order_relaxed); }
    bool gain_is_unity() const { return gain_q16() == 65536; }

    /**
     * @brief Account for successfully written sample frames.
     * @param src_bits bit depth of the written samples
     */
    void account_write(size_t bytes, uint8_t src_bits)
    {
        uint8_t bps = src_bits ? (src_bits / 8) : (cur_bits ? cur_bits / 8 : 2);
        uint8_t ch = cur_channels ? cur_channels : 2;
        size_t frame_bytes = (size_t)bps * ch;
        if (frame_bytes)
        {
            played_frames_ += bytes / frame_bytes;
        }
    }

    /** Total frames handed to the output. */
    uint64_t played_frames() const { return played_frames_; }

    /** Reset the frame counter at the start of a session. */
    void reset_accounting() { played_frames_ = 0; }

protected:
    std::atomic<float> volume_db_{0.0f};
    std::atomic<uint32_t> gain_q16_{65536}; /**< Q16 linear gain, 65536 = 0 dB */
    uint64_t played_frames_ = 0;            /**< frames handed to the output */

public:
    /**
     * @brief Convert PCM between bit depths, left-aligned to the container.
     *
     * @note Input is interleaved, little-endian, signed, left-aligned.
     * @note Only 16/24/32-bit PCM is supported.
     *
     * @param src       source PCM (interleaved, src_bits)
     * @param samples   total number of samples (not frames)
     * @param dst       destination buffer
     * @param dst_cap   destination capacity in bytes
     * @return bytes written to dst
     */
    static size_t convert_bits(const void *src, size_t samples, uint8_t src_bits, void *dst,
                               size_t dst_cap, uint8_t dst_bits)
    {
        if (!src || !dst || samples == 0 || src_bits == 0 || dst_bits == 0)
        {
            return 0;
        }

        if (!((src_bits == 16 || src_bits == 24 || src_bits == 32) &&
              (dst_bits == 16 || dst_bits == 24 || dst_bits == 32)))
        {
            return 0;
        }

        const size_t src_bytes = src_bits / 8;
        const size_t dst_bytes = dst_bits / 8;
        const size_t dst_cap_samples = dst_cap / dst_bytes;
        const size_t n = (samples < dst_cap_samples) ? samples : dst_cap_samples;
        if (n == 0)
        {
            return 0;
        }

        if (src_bits == dst_bits)
        {
            memcpy(dst, src, n * src_bytes);
            return n * src_bytes;
        }

        const uint8_t *s = (const uint8_t *)src;
        uint8_t *d = (uint8_t *)dst;

        for (size_t i = 0; i < n; i++)
        {
            /* Normalise to int32, left aligned. */
            int32_t v32;
            if (src_bits == 16)
            {
                uint16_t u16 = (uint16_t)s[0] | ((uint16_t)s[1] << 8);
                int16_t s16 = (int16_t)u16;
                v32 = (int32_t)((uint32_t)(uint16_t)s16 << 16);
            }
            else if (src_bits == 24)
            {
                uint32_t u24 = (uint32_t)s[0] | ((uint32_t)s[1] << 8) | ((uint32_t)s[2] << 16);
                if (u24 & 0x800000)
                {
                    u24 |= 0xFF000000; /* sign extend 24 -> 32 */
                }
                v32 = (int32_t)(u24 << 8); /* left align */
            }
            else /* src_bits == 32 */
            {
                uint32_t u32 = (uint32_t)s[0] | ((uint32_t)s[1] << 8) |
                               ((uint32_t)s[2] << 16) | ((uint32_t)s[3] << 24);
                v32 = (int32_t)u32;
            }
            s += src_bytes;

            int32_t out;
            if (dst_bits == 32)
            {
                out = v32;
            }
            else
            {
                out = v32 >> (32 - dst_bits);
            }

            if (dst_bits == 16)
            {
                d[0] = (uint8_t)(out & 0xFF);
                d[1] = (uint8_t)((out >> 8) & 0xFF);
            }
            else if (dst_bits == 24)
            {
                d[0] = (uint8_t)(out & 0xFF);
                d[1] = (uint8_t)((out >> 8) & 0xFF);
                d[2] = (uint8_t)((out >> 16) & 0xFF);
            }
            else
            {
                d[0] = (uint8_t)(out & 0xFF);
                d[1] = (uint8_t)((out >> 8) & 0xFF);
                d[2] = (uint8_t)((out >> 16) & 0xFF);
                d[3] = (uint8_t)((out >> 24) & 0xFF);
            }
            d += dst_bytes;
        }

        return n * dst_bytes;
    }

    /**
     * @brief Integer volume scaling in place (Q16 gain, saturating).
     *
     * @param buf     32-bit samples (modified in place)
     * @param samples number of samples
     * @param gain    Q16 linear gain (65536 = 0 dB)
     */
    static void apply_gain(int32_t *buf, size_t samples, uint32_t gain)
    {
        if (!buf || gain == 65536)
        {
            return;
        }
        for (size_t i = 0; i < samples; i++)
        {
            int64_t v = ((int64_t)buf[i] * (int64_t)gain) >> 16;
            if (v > INT32_MAX)
            {
                v = INT32_MAX;
            }
            else if (v < INT32_MIN)
            {
                v = INT32_MIN;
            }
            buf[i] = (int32_t)v;
        }
    }

    /** dB -> Q16 linear gain. -60 dB and below is silence (returns 0). */
    static uint32_t db_to_gain_q16(float db)
    {
        if (db <= -60.0f)
        {
            return 0;
        }
        float linear = powf(10.0f, db / 20.0f);
        return (uint32_t)(linear * 65536.0f + 0.5f);
    }
};

#endif /* __cplusplus */
