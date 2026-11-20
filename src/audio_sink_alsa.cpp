/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 kanfandelong. All rights reserved.
 */

/**
 * @file    audio_sink_alsa.c
 * @brief   Native ALSA output sink.
 *
 * ALSA is loaded with dlopen("libasound.so.2") so building the library does
 * not require libasound development headers or an ARM sysroot with ALSA.
 * Runtime only needs the shared object that ships with the board rootfs.
 *
 * The PCM device is opened in non-blocking mode: writes return -EAGAIN when
 * the buffer is full and the sink waits in 100 ms slices, checking the session
 * stop flag in between. That keeps espaudiocore_stop() bounded even when the
 * sound card stalls.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "audio_port.h"
#include "audio_sink.h"
#include "audio_types.h"

#include <dlfcn.h>
#include <errno.h>
#include <math.h>
#include <new>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <atomic>
#include <string>

namespace
{

/* Minimal ALSA ABI subset. Values match alsa/pcm.h. */
typedef struct _snd_pcm snd_pcm_t;
typedef long snd_pcm_sframes_t;
typedef unsigned long snd_pcm_uframes_t;

constexpr int kStreamPlayback = 0;
constexpr int kAccessRwInterleaved = 3;
constexpr int kFormatS16Le = 2;
/*
 * ALSA snd_pcm_format_t 的实际枚举值：
 *   S16_LE=2, S24_LE=6, S32_LE=10, S24_3LE=32。
 *
 * 2026-11-20 修：这里原来写的是 6（= S24_LE，4 字节容器），但
 * convert_bits(dst_bits=24) 按 S24_3LE 的**3 字节**布局打包；于是设备把
 * 3 字节样本当 4 字节 S24_LE 读 → 每个样本都错位，RK3506/ES8388 上表现为
 * 持续爆音/失真（16-bit 路径正常）。正确的 S24_3LE 值是 32。
 */
constexpr int kFormatS24_3Le = 32;
constexpr int kFormatS32Le = 10;
constexpr int kNonblock = 1;

struct AlsaApi
{
    void *lib = nullptr;
    int (*open)(snd_pcm_t **, const char *, int, int) = nullptr;
    int (*set_params)(snd_pcm_t *, int, int, unsigned int, unsigned int, int, unsigned int) = nullptr;
    snd_pcm_sframes_t (*writei)(snd_pcm_t *, const void *, snd_pcm_uframes_t) = nullptr;
    int (*recover)(snd_pcm_t *, int, int) = nullptr;
    int (*prepare)(snd_pcm_t *) = nullptr;
    int (*drop)(snd_pcm_t *) = nullptr;
    int (*drain)(snd_pcm_t *) = nullptr;
    int (*close)(snd_pcm_t *) = nullptr;
    int (*wait)(snd_pcm_t *, int) = nullptr;
    int (*pause)(snd_pcm_t *, int) = nullptr;
    int (*nonblock)(snd_pcm_t *, int) = nullptr;
    int (*delay)(snd_pcm_t *, snd_pcm_sframes_t *) = nullptr;
    const char *(*strerror_fn)(int) = nullptr;
};

AlsaApi g_alsa;
pthread_once_t g_alsa_once = PTHREAD_ONCE_INIT;

void alsa_load_once()
{
    g_alsa.lib = dlopen("libasound.so.2", RTLD_NOW | RTLD_LOCAL);
    if (!g_alsa.lib)
    {
        AUDIO_LOGE("cannot load libasound.so.2: %s", dlerror());
        return;
    }

    g_alsa.open = (int (*)(snd_pcm_t **, const char *, int, int))dlsym(g_alsa.lib, "snd_pcm_open");
    g_alsa.set_params =
        (int (*)(snd_pcm_t *, int, int, unsigned int, unsigned int, int, unsigned int))dlsym(
            g_alsa.lib, "snd_pcm_set_params");
    g_alsa.writei =
        (snd_pcm_sframes_t(*)(snd_pcm_t *, const void *, snd_pcm_uframes_t))dlsym(g_alsa.lib,
                                                                                 "snd_pcm_writei");
    g_alsa.recover = (int (*)(snd_pcm_t *, int, int))dlsym(g_alsa.lib, "snd_pcm_recover");
    g_alsa.prepare = (int (*)(snd_pcm_t *))dlsym(g_alsa.lib, "snd_pcm_prepare");
    g_alsa.drop = (int (*)(snd_pcm_t *))dlsym(g_alsa.lib, "snd_pcm_drop");
    g_alsa.drain = (int (*)(snd_pcm_t *))dlsym(g_alsa.lib, "snd_pcm_drain");
    g_alsa.close = (int (*)(snd_pcm_t *))dlsym(g_alsa.lib, "snd_pcm_close");
    g_alsa.wait = (int (*)(snd_pcm_t *, int))dlsym(g_alsa.lib, "snd_pcm_wait");
    g_alsa.pause = (int (*)(snd_pcm_t *, int))dlsym(g_alsa.lib, "snd_pcm_pause");
    g_alsa.nonblock = (int (*)(snd_pcm_t *, int))dlsym(g_alsa.lib, "snd_pcm_nonblock");
    g_alsa.delay =
        (int (*)(snd_pcm_t *, snd_pcm_sframes_t *))dlsym(g_alsa.lib, "snd_pcm_delay");
    g_alsa.strerror_fn = (const char *(*)(int))dlsym(g_alsa.lib, "snd_strerror");

    if (!g_alsa.open || !g_alsa.set_params || !g_alsa.writei || !g_alsa.recover ||
        !g_alsa.prepare || !g_alsa.close || !g_alsa.wait)
    {
        AUDIO_LOGE("libasound.so.2 is missing required symbols");
        dlclose(g_alsa.lib);
        g_alsa = AlsaApi{};
    }
}

bool alsa_load()
{
    pthread_once(&g_alsa_once, alsa_load_once);
    return g_alsa.lib != nullptr;
}

const char *alsa_strerror(int err)
{
    if (g_alsa.strerror_fn)
    {
        return g_alsa.strerror_fn(err);
    }
    return strerror(err < 0 ? -err : err);
}

int bits_to_alsa_format(uint8_t bits)
{
    switch (bits)
    {
    case 16:
        return kFormatS16Le;
    case 24:
        return kFormatS24_3Le;
    case 32:
        return kFormatS32Le;
    default:
        return -1;
    }
}

} // namespace

class AudioSinkAlsa : public AudioSink
{
public:
    AudioSinkAlsa(const char *device, uint32_t io_timeout_ms, uint8_t target_bits,
                  uint32_t latency_ms)
        : io_timeout_ms_(io_timeout_ms), target_bits_(target_bits)
    {
        if (device && device[0])
        {
            device_ = device;
        }
        uint32_t latency = latency_ms ? latency_ms : 200;
        if (latency < 20)
        {
            latency = 20;
        }
        if (latency > 2000)
        {
            latency = 2000;
        }
        latency_us_ = latency * 1000;
    }

    ~AudioSinkAlsa() override
    {
        close();
    }

    bool init()
    {
        if (!alsa_load())
        {
            return false;
        }
        if (pthread_mutex_init(&lock_, nullptr) != 0)
        {
            return false;
        }
        lock_init_ = true;

        buf_a_ = (int32_t *)audio_alloc_big(kConvSamples * sizeof(int32_t), 0);
        buf_b_ = (uint8_t *)audio_alloc_big(kConvSamples * 4, 0);
        buf_c_ = (int32_t *)audio_alloc_big(kConvSamples * sizeof(int32_t), 0);
        if (!buf_a_ || !buf_b_ || !buf_c_)
        {
            AUDIO_LOGE("no memory for ALSA conversion buffers");
            return false;
        }

        int rc = g_alsa.open(&pcm_, device_.c_str(), kStreamPlayback, kNonblock);
        if (rc < 0)
        {
            AUDIO_LOGE("cannot open ALSA device '%s': %s", device_.c_str(), alsa_strerror(rc));
            pcm_ = nullptr;
            return false;
        }
        AUDIO_LOGI("ALSA device '%s' opened (latency target %u ms)", device_.c_str(),
                   (unsigned)(latency_us_ / 1000));
        return true;
    }

    void set_stop_flag(const std::atomic<bool> *flag)
    {
        stop_flag_ = flag;
    }

    void set_pcm_callback(espaudiocore_pcm_cb_t cb, void *user)
    {
        pcm_cb_ = cb;
        pcm_user_ = user;
    }

    int set_rate(uint32_t hz) override
    {
        if (hz == 0)
        {
            return AUDIO_ERR_INVALID_ARG;
        }
        pthread_mutex_lock(&lock_);
        if (hz != cur_rate)
        {
            cur_rate = hz;
            dirty_ = true;
        }
        pthread_mutex_unlock(&lock_);
        return AUDIO_OK;
    }

    int set_format(uint8_t bits, uint8_t channels) override
    {
        if (bits_to_alsa_format(bits) < 0 || (channels != 1 && channels != 2))
        {
            AUDIO_LOGE("unsupported output format: %u-bit %u ch", (unsigned)bits,
                       (unsigned)channels);
            return AUDIO_ERR_NOT_SUPPORTED;
        }
        pthread_mutex_lock(&lock_);
        if (bits != cur_bits || channels != cur_channels)
        {
            cur_bits = bits;
            cur_channels = channels;
            dirty_ = true;
        }
        pthread_mutex_unlock(&lock_);
        return AUDIO_OK;
    }

    int write_i32(const int32_t *pcm, size_t pairs, uint8_t src_bits, uint32_t timeout_ms) override
    {
        if (!pcm || pairs == 0)
        {
            return AUDIO_OK;
        }
        if (ensure_configured() != AUDIO_OK)
        {
            return AUDIO_ERR_IO;
        }

        const uint8_t sb = src_bits ? src_bits : (cur_bits ? cur_bits : 16);
        const uint8_t ob = output_bits();
        if (bits_to_alsa_format(ob) < 0)
        {
            return AUDIO_ERR_NOT_SUPPORTED;
        }

        tap_pcm(pcm, pairs, sb);

        const bool unity = gain_is_unity();
        const uint32_t gain = gain_q16();
        const size_t total = pairs * 2;
        size_t done = 0;

        while (done < total)
        {
            size_t n = total - done;
            if (n > kConvSamples)
            {
                n = kConvSamples;
            }

            const int32_t *src = pcm + done;
            if (!unity)
            {
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
                    buf_a_[i] = (int32_t)v;
                }
                src = buf_a_;
            }

            int rc;
            if (ob == 32)
            {
                rc = raw_write(src, n * sizeof(int32_t), timeout_ms);
            }
            else
            {
                size_t nb = convert_bits(src, n, 32, buf_b_, kConvSamples * 4, ob);
                if (nb == 0)
                {
                    return AUDIO_ERR_IO;
                }
                rc = raw_write(buf_b_, nb, timeout_ms);
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

    int write(const void *frames, size_t bytes, uint32_t timeout_ms) override
    {
        if (!frames || bytes == 0)
        {
            return AUDIO_OK;
        }

        const uint8_t sb = cur_bits ? cur_bits : 16;
        if (bits_to_alsa_format(sb) < 0)
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
            n &= ~(size_t)1; /* whole L/R pairs only */

            size_t nb = convert_bits((const uint8_t *)frames + done * src_bytes, n, sb, buf_c_,
                                     kConvSamples * 4, 32);
            if (nb == 0)
            {
                return AUDIO_ERR_IO;
            }
            int rc = write_i32(buf_c_, n / 2, 32, timeout_ms);
            if (rc != AUDIO_OK)
            {
                return rc;
            }
            done += n;
        }
        return AUDIO_OK;
    }

    int flush() override
    {
        if (!pcm_)
        {
            return AUDIO_OK;
        }
        pthread_mutex_lock(&lock_);
        int rc = drain_locked(1000);
        pthread_mutex_unlock(&lock_);
        return rc == 0 ? AUDIO_OK : AUDIO_ERR_IO;
    }

    int prepare() override
    {
        return ensure_configured();
    }

    int discard() override
    {
        if (!pcm_ || !g_alsa.drop)
        {
            return AUDIO_OK;
        }
        pthread_mutex_lock(&lock_);
        if (!dirty_)
        {
            (void)g_alsa.drop(pcm_);
            if (g_alsa.prepare)
            {
                (void)g_alsa.prepare(pcm_);
            }
        }
        pthread_mutex_unlock(&lock_);
        return AUDIO_OK;
    }

    int pause(bool on) override
    {
        if (!pcm_ || !g_alsa.pause)
        {
            return AUDIO_OK; /* player still stops feeding data */
        }
        pthread_mutex_lock(&lock_);
        int rc = g_alsa.pause(pcm_, on ? 1 : 0);
        pthread_mutex_unlock(&lock_);
        if (rc < 0)
        {
            /* Many PCM plugins do not implement pause; falling back to "stop
             * feeding" is perfectly valid. */
            AUDIO_LOGD("snd_pcm_pause(%d) unsupported: %s", on ? 1 : 0, alsa_strerror(rc));
            return AUDIO_OK;
        }
        return AUDIO_OK;
    }

    size_t pending_frames()
    {
        if (!pcm_ || !g_alsa.delay)
        {
            return 0;
        }
        pthread_mutex_lock(&lock_);
        snd_pcm_sframes_t delay = 0;
        int rc = g_alsa.delay(pcm_, &delay);
        pthread_mutex_unlock(&lock_);
        if (rc < 0 || delay <= 0)
        {
            return 0;
        }
        return (size_t)delay;
    }

    void close()
    {
        if (pcm_)
        {
            pthread_mutex_lock(&lock_);
            /* Manual stop should be immediate: drop queued frames. Normal EOS
             * was already drained by flush() on the decoder thread. */
            if (g_alsa.drop)
            {
                (void)g_alsa.drop(pcm_);
            }
            int rc = g_alsa.close(pcm_);
            if (rc < 0)
            {
                AUDIO_LOGW("snd_pcm_close failed: %s", alsa_strerror(rc));
            }
            pcm_ = nullptr;
            pthread_mutex_unlock(&lock_);
        }
        if (lock_init_)
        {
            pthread_mutex_destroy(&lock_);
            lock_init_ = false;
        }
        audio_free(buf_a_);
        audio_free(buf_b_);
        audio_free(buf_c_);
        buf_a_ = nullptr;
        buf_b_ = nullptr;
        buf_c_ = nullptr;
    }

private:
    static const size_t kConvSamples = 8192;

    uint8_t output_bits() const
    {
        if (target_bits_)
        {
            return target_bits_;
        }
        return cur_bits ? cur_bits : 16;
    }

    int ensure_configured()
    {
        pthread_mutex_lock(&lock_);
        int rc = ensure_configured_locked();
        pthread_mutex_unlock(&lock_);
        return rc;
    }

    int ensure_configured_locked()
    {
        if (!dirty_)
        {
            return AUDIO_OK;
        }
        if (!pcm_)
        {
            return AUDIO_ERR_IO;
        }
        if (cur_rate == 0 || cur_channels == 0 || cur_bits == 0)
        {
            AUDIO_LOGE("ALSA sink has no format yet (rate=%u ch=%u bits=%u)", (unsigned)cur_rate,
                       (unsigned)cur_channels, (unsigned)cur_bits);
            return AUDIO_ERR_INVALID_ARG;
        }

        const uint8_t ob = output_bits();
        const int format = bits_to_alsa_format(ob);
        if (format < 0)
        {
            return AUDIO_ERR_NOT_SUPPORTED;
        }

        /* A running stream must be dropped before hw_params can change. */
        if (g_alsa.drop)
        {
            (void)g_alsa.drop(pcm_);
        }
        int rc = g_alsa.set_params(pcm_, format, kAccessRwInterleaved, cur_channels, cur_rate, 1,
                                   latency_us_);
        if (rc < 0)
        {
            (void)g_alsa.prepare(pcm_);
            rc = g_alsa.set_params(pcm_, format, kAccessRwInterleaved, cur_channels, cur_rate, 1,
                                   latency_us_);
        }
        if (rc < 0)
        {
            AUDIO_LOGE("ALSA set_params(%u Hz, %u ch, %u-bit) failed: %s", (unsigned)cur_rate,
                       (unsigned)cur_channels, (unsigned)ob, alsa_strerror(rc));
            /* Remember the attempt so the decode loop does not retry forever. */
            dirty_ = false;
            return AUDIO_ERR_IO;
        }

        dirty_ = false;
        AUDIO_LOGI("ALSA format: %u Hz, %u ch, %u-bit (target %s)", (unsigned)cur_rate,
                   (unsigned)cur_channels, (unsigned)ob,
                   target_bits_ ? "fixed" : "follow source");
        return AUDIO_OK;
    }

    int drain_locked(int timeout_ms)
    {
        if (!pcm_ || !g_alsa.drain)
        {
            return 0;
        }
        if (g_alsa.nonblock)
        {
            (void)g_alsa.nonblock(pcm_, 1);
        }
        int waited = 0;
        for (;;)
        {
            int rc = g_alsa.drain(pcm_);
            if (rc != -EAGAIN)
            {
                return rc;
            }
            if (waited >= timeout_ms)
            {
                (void)g_alsa.drop(pcm_);
                return 0;
            }
            int slice = timeout_ms - waited;
            if (slice > 50)
            {
                slice = 50;
            }
            (void)g_alsa.wait(pcm_, slice);
            waited += slice;
        }
    }

    int raw_write(const void *data, size_t bytes, uint32_t timeout_ms)
    {
        if (!pcm_ || bytes == 0)
        {
            return AUDIO_OK;
        }
        if (ensure_configured() != AUDIO_OK)
        {
            return AUDIO_ERR_IO;
        }

        pthread_mutex_lock(&lock_);
        const uint8_t ob = output_bits();
        const uint8_t ch = cur_channels ? cur_channels : 2;
        pthread_mutex_unlock(&lock_);
        const size_t frame_bytes = (size_t)ch * (size_t)(ob / 8);
        if (frame_bytes == 0)
        {
            return AUDIO_ERR_INVALID_ARG;
        }

        const uint8_t *p = (const uint8_t *)data;
        const size_t frames = bytes / frame_bytes;
        size_t done = 0;
        uint32_t tmo = timeout_ms ? timeout_ms : io_timeout_ms_;
        int64_t elapsed_ms = 0;

        while (done < frames)
        {
            if (stop_flag_ && stop_flag_->load(std::memory_order_relaxed))
            {
                return AUDIO_ERR_NOT_SUPPORTED;
            }

            pthread_mutex_lock(&lock_);
            snd_pcm_sframes_t rc = g_alsa.writei(pcm_, p + done * frame_bytes, frames - done);
            int err = (rc < 0) ? (int)rc : 0;
            pthread_mutex_unlock(&lock_);

            if (rc > 0)
            {
                done += (size_t)rc;
                elapsed_ms = 0;
                continue;
            }
            if (err == -EAGAIN)
            {
                /* Non-blocking buffer full: wait in a slice so stop can win. */
                (void)g_alsa.wait(pcm_, 100);
                elapsed_ms += 100;
                if (tmo && elapsed_ms > (int64_t)tmo)
                {
                    AUDIO_LOGW("ALSA write timed out after %u ms", (unsigned)tmo);
                    return AUDIO_ERR_IO;
                }
                continue;
            }
            if (err == -EPIPE || err == -ESTRPIPE || err == -EINTR)
            {
                if (err == -EINTR)
                {
                    continue;
                }
                pthread_mutex_lock(&lock_);
                int rrc = g_alsa.recover(pcm_, err, 1);
                pthread_mutex_unlock(&lock_);
                AUDIO_LOGW("ALSA %s recovered (rc=%d)",
                           err == -EPIPE ? "underrun" : "suspend", rrc);
                continue;
            }

            AUDIO_LOGE("snd_pcm_writei failed: %s", alsa_strerror(err));
            return AUDIO_ERR_IO;
        }
        return AUDIO_OK;
    }

    void tap_pcm(const int32_t *pcm, size_t frames, uint8_t src_bits)
    {
        if (!pcm_cb_ || !pcm || frames == 0)
        {
            return;
        }
        espaudiocore_pcm_t info = {};
        info.samples = pcm;
        info.frames = frames;
        info.rate = cur_rate;
        info.channels = cur_channels ? cur_channels : 2;
        info.bits = src_bits ? src_bits : (cur_bits ? cur_bits : 16);
        pcm_cb_(pcm_user_, &info);
    }

    snd_pcm_t *pcm_ = nullptr;
    pthread_mutex_t lock_{};
    bool lock_init_ = false;
    std::string device_ = "default";
    uint32_t io_timeout_ms_ = 0;
    uint32_t latency_us_ = 200000;
    uint8_t target_bits_ = 0;
    bool dirty_ = true;

    const std::atomic<bool> *stop_flag_ = nullptr;
    espaudiocore_pcm_cb_t pcm_cb_ = nullptr;
    void *pcm_user_ = nullptr;

    int32_t *buf_a_ = nullptr; /**< volume-scaled int32 staging */
    uint8_t *buf_b_ = nullptr; /**< narrowed output (16/24/32) */
    int32_t *buf_c_ = nullptr; /**< write() -> int32 conversion staging */
};

AudioSink *audio_sink_alsa_create(const char *device, uint32_t io_timeout_ms, uint8_t target_bits,
                                  uint32_t latency_ms)
{
    AudioSinkAlsa *s = new (std::nothrow) AudioSinkAlsa(device, io_timeout_ms, target_bits,
                                                        latency_ms);
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

void audio_sink_alsa_bind_stop(AudioSink *sink, const std::atomic<bool> *flag)
{
    AudioSinkAlsa *alsa = static_cast<AudioSinkAlsa *>(sink);
    if (alsa)
    {
        alsa->set_stop_flag(flag);
    }
}

void audio_sink_alsa_set_pcm(AudioSink *sink, espaudiocore_pcm_cb_t cb, void *user)
{
    AudioSinkAlsa *alsa = static_cast<AudioSinkAlsa *>(sink);
    if (alsa)
    {
        alsa->set_pcm_callback(cb, user);
    }
}

size_t audio_sink_alsa_pending_frames(AudioSink *sink)
{
    AudioSinkAlsa *alsa = static_cast<AudioSinkAlsa *>(sink);
    return alsa ? alsa->pending_frames() : 0;
}

size_t audio_sink_pending_frames(AudioSink *sink)
{
    return audio_sink_alsa_pending_frames(sink);
}
