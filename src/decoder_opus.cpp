/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 kanfandelong. All rights reserved.
 */

/**
 * @file    decoder_opus.cpp
 * @brief   Opus 解码器（opusfile + libopus + libogg）。
 * @note opusfile 需要 seek 能力，**ringbuf 输入不可用**。
 */

#include "audio_port.h"
#include "audio_types.h"
#include "decoder.h"

#include <new>
#include <stdlib.h>
#include <string.h>

extern "C" {
#include "opusfile.h"
}

#if defined(CONFIG_ESPAUDIOCORE_ENABLE_OPUS)

/** 一次 op_read_stereo 的最大样本数（帧数） */
#define OPUS_READ_FRAMES 2048
/** Opus 解码输出固定 48 kHz */
#define OPUS_SAMPLE_RATE 48000

class DecoderOpus : public AudioDecoder {
public:
    ~DecoderOpus() override
    {
        close();
    }

    audio_err_t open(AudioInput *in, AudioSink *out, espaudiocore_meta_cb_t meta_cb, void *meta_user,
                     audio_format_t *fmt, int64_t *duration_ms, int *dec_err) override
    {
        in_  = in;
        out_ = out;
        meta_cb_   = meta_cb;
        meta_user_ = meta_user;
        if (dec_err) {
            *dec_err = AUDIO_DEC_ERR_NONE;
        }
        if (duration_ms) {
            *duration_ms = -1;
        }
        if (!in_ || !out_) {
            return AUDIO_ERR_INVALID_ARG;
        }
        if (!in_->can_seek()) {
            AUDIO_LOGE("Opus requires a seekable source (ringbuf input is not supported)");
            if (dec_err) {
                *dec_err = AUDIO_DEC_ERR_OPEN;
            }
            return AUDIO_ERR_NOT_SUPPORTED;
        }

        cb_.read  = read_cb;
        cb_.seek  = seek_cb;
        cb_.tell  = tell_cb;
        cb_.close = close_cb;

        int err = OP_EREAD;
        of_ = op_open_callbacks(this, &cb_, nullptr, 0, &err);
        if (!of_) {
            AUDIO_LOGE("op_open_callbacks failed: %d", err);
            if (dec_err) {
                *dec_err = AUDIO_DEC_ERR_OPEN;
            }
            return AUDIO_ERR_PARSE;
        }
        opened_ = true;

        /* op_read_stereo 输出交错立体声，因此对外固定 2 声道 */
        fmt_.rate     = OPUS_SAMPLE_RATE;
        fmt_.channels = 2;
        fmt_.bits     = 16;

        ogg_int64_t total = op_pcm_total(of_, -1);
        if (total > 0) {
            duration_hint_ms_ = (int64_t)((uint64_t)total * 1000ull / OPUS_SAMPLE_RATE);
            report_duration(duration_hint_ms_);
        }

        report_tags();

        pcm_ = (int16_t *)audio_alloc_big(OPUS_READ_FRAMES * 2 * sizeof(int16_t), 0);
        pcm32_ = (int32_t *)audio_alloc_big(OPUS_READ_FRAMES * 2 * sizeof(int32_t), 0);
        if (!pcm_ || !pcm32_) {
            if (dec_err) {
                *dec_err = AUDIO_DEC_ERR_OPEN;
            }
            close();
            return AUDIO_ERR_NO_MEM;
        }

        if (fmt) {
            *fmt = fmt_;
        }
        if (out_) {
            out_->set_rate(fmt_.rate);
            out_->set_format(fmt_.bits, fmt_.channels);
        }
        if (duration_ms) {
            *duration_ms = duration_hint_ms_;
        }

        state_ = AUDIO_DEC_STATE_ACTIVE;
        AUDIO_LOGI("Opus: %d ch source -> stereo 48 kHz, %lld ms", op_channel_count(of_, -1),
                   (long long)duration_hint_ms_);
        return AUDIO_OK;
    }

    audio_err_t decode() override
    {
        if (state_ != AUDIO_DEC_STATE_ACTIVE) {
            return AUDIO_ERR_EOF;
        }

        /* op_read_stereo 返回**每声道**帧数；负数为错误码 */
        int ret = op_read_stereo(of_, (opus_int16 *)pcm_, OPUS_READ_FRAMES * 2);
        if (ret == 0) {
            state_ = AUDIO_DEC_STATE_EOS;
            return AUDIO_ERR_EOF;
        }
        if (ret < 0) {
            if (ret == OP_HOLE) {
                return AUDIO_OK; /* 数据空洞：跳过（与原实现一致） */
            }
            AUDIO_LOGE("op_read_stereo failed: %d", ret);
            state_ = AUDIO_DEC_STATE_ERROR;
            return AUDIO_ERR_DECODE;
        }

        /* 统一 int32 域（见 D-23）：opusfile 输出交错立体声 int16 */
        const size_t nsamp = (size_t)ret * 2;
        for (size_t i = 0; i < nsamp; i++) {
            pcm32_[i] = (int32_t)pcm_[i] << 16;
        }
        int rc = out_->write_i32(pcm32_, (size_t)ret, 16, 0);
        if (rc == AUDIO_ERR_NOT_SUPPORTED) {
            rc = out_->write(pcm_, nsamp * sizeof(int16_t), 0);
        }
        if (rc == AUDIO_ERR_NOT_SUPPORTED) {
            state_ = AUDIO_DEC_STATE_EOS;
            return AUDIO_ERR_EOF;
        }
        if (rc != AUDIO_OK) {
            state_ = AUDIO_DEC_STATE_ERROR;
            return (audio_err_t)rc;
        }
        return AUDIO_OK;
    }

    audio_err_t seek_ms(int64_t ms) override
    {
        uint64_t sample = (uint64_t)ms * fmt_.rate / 1000ull;
        return op_pcm_seek(of_, sample) == 0 ? AUDIO_OK : AUDIO_FAIL;
    }

    void reset() override
    {
        /* seek 由库的api完成；这里不用做任何事） */
        // if (opened_ && of_) {
        //     (void)op_raw_seek(of_, 0);
        // }
        state_ = AUDIO_DEC_STATE_ACTIVE;
    }

    void close() override
    {
        if (of_) {
            if (opened_) {
                op_free(of_);
            }
            of_ = nullptr;
            opened_ = false;
        }
        audio_free(pcm_);
        pcm_ = nullptr;
        audio_free(pcm32_);
        pcm32_ = nullptr;
        state_ = AUDIO_DEC_STATE_IDLE;
    }

private:
    void report_duration(int64_t ms)
    {
        if (ms < 0 || !meta_cb_) {
            return;
        }
        char b[24];
        snprintf(b, sizeof(b), "%lld", (long long)ms);
        espaudiocore_meta_t m = {};
        m.type      = "tlen";
        m.is_binary = false;
        m.data      = b;
        m.len       = strlen(b);
        m.pic_type  = -1;
        meta_cb_(meta_user_, &m);
    }

    void report_tags()
    {
        if (!meta_cb_) {
            return;
        }
        const OpusTags *tags = op_tags(of_, -1);
        if (!tags) {
            AUDIO_LOGW("no Opus tags");
            return;
        }
        if (tags->vendor) {
            espaudiocore_meta_t m = {};
            m.type      = "VENDOR";
            m.is_binary = false;
            m.data      = tags->vendor;
            m.len       = strlen(tags->vendor);
            m.pic_type  = -1;
            meta_cb_(meta_user_, &m);
        }
        for (int i = 0; i < tags->comments; i++) {
            const char *e = tags->user_comments[i];
            int len = tags->comment_lengths[i];
            if (!e || len <= 0) {
                continue;
            }
            const char *eq = (const char *)memchr(e, '=', (size_t)len);
            if (!eq) {
                continue;
            }
            char key[32];
            size_t klen = (size_t)(eq - e);
            if (klen >= sizeof(key)) {
                klen = sizeof(key) - 1;
            }
            memcpy(key, e, klen);
            key[klen] = '\0';

            espaudiocore_meta_t m = {};
            m.type      = key;
            m.is_binary = false;
            m.data      = eq + 1;
            m.len       = (size_t)(len - klen - 1);
            m.pic_type  = -1;
            meta_cb_(meta_user_, &m);
        }
    }

    /* ---------------- opusfile 回调 ---------------- */

    static int read_cb(void *stream, unsigned char *ptr, int nbytes)
    {
        DecoderOpus *self = static_cast<DecoderOpus *>(stream);
        int n = self->in_->read(ptr, (size_t)nbytes);
        return (n < 0) ? -1 : n;
    }

    static int seek_cb(void *stream, opus_int64 offset, int whence)
    {
        DecoderOpus *self = static_cast<DecoderOpus *>(stream);
        return (self->in_->seek((int64_t)offset, whence) == AUDIO_OK) ? 0 : -1;
    }

    static opus_int64 tell_cb(void *stream)
    {
        DecoderOpus *self = static_cast<DecoderOpus *>(stream);
        return (opus_int64)self->in_->tell();
    }

    static int close_cb(void *stream)
    {
        (void)stream; /* 源由 player 释放 */
        return 0;
    }

    AudioInput        *in_ = nullptr;
    AudioSink         *out_ = nullptr;
    OggOpusFile       *of_ = nullptr;
    OpusFileCallbacks  cb_ = {};
    bool               opened_ = false;
    int16_t           *pcm_ = nullptr;
    int32_t           *pcm32_ = nullptr; /**< 统一 int32 域中间缓冲 */
    espaudiocore_meta_cb_t meta_cb_ = nullptr;
    void              *meta_user_ = nullptr;
    int64_t            duration_hint_ms_ = -1;
};

/** 魔数：OGG 页 "OggS" + 第一页 payload 以 "OpusHead" 开头 */
bool probe_opus(AudioInput *in)
{
    uint8_t h[64];
    if (!in) {
        return false;
    }
    int got = in->peek(h, sizeof(h), 40);
    if (got < 28 || memcmp(h, "OggS", 4) != 0) {
        return false;
    }
    int segs = h[26];
    int payload = 27 + segs;
    if (payload + 8 > got) {
        return false;
    }
    return memcmp(h + payload, "OpusHead", 8) == 0;
}

AudioDecoder *create_opus(void)
{
    return new (std::nothrow) DecoderOpus();
}

#else /* !CONFIG_ESPAUDIOCORE_ENABLE_OPUS */

bool probe_opus(AudioInput *in)
{
    (void)in;
    return false;
}

AudioDecoder *create_opus(void)
{
    return nullptr;
}

#endif
