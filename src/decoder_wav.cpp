/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 kanfandelong. All rights reserved.
 */

/**
 * @file    decoder_wav.cpp
 * @brief   WAV (RIFF/WAVE) 解码器：支持 PCM 8/16/24/32-bit 与 IEEE float。
 */

#include "audio_port.h"
#include "audio_types.h"
#include "decoder.h"

#include <new>
#include <stdlib.h>
#include <string.h>

#if defined(CONFIG_ESPAUDIOCORE_ENABLE_WAV)

#define WAV_HEADER_SCAN_MAX 4096
#define WAV_OUT_FRAMES      512 /**< 每次 decode 输出的立体声帧数 */
/** 统一 int32 域缓冲容量（采样点数，每帧 L/R） */
#define WAV_I32_CAP (WAV_OUT_FRAMES * 2)

class DecoderWav : public AudioDecoder {
public:
    ~DecoderWav() override
    {
        close();
    }

    audio_err_t open(AudioInput *in, AudioSink *out, espaudiocore_meta_cb_t meta_cb, void *meta_user,
                     audio_format_t *fmt, int64_t *duration_ms, int *dec_err) override
    {
        (void)meta_cb;
        (void)meta_user;
        in_  = in;
        out_ = out;
        if (dec_err) {
            *dec_err = AUDIO_DEC_ERR_NONE;
        }
        if (duration_ms) {
            *duration_ms = -1;
        }

        uint8_t hdr[WAV_HEADER_SCAN_MAX];
        int got = in_->peek(hdr, sizeof(hdr), 12);
        if (got < 12 || memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0) {
            if (dec_err) {
                *dec_err = AUDIO_DEC_ERR_PARSE;
            }
            return AUDIO_ERR_PARSE;
        }

        /* 逐个 chunk 扫描（在 peek 缓冲范围内），找到 fmt 与 data */
        size_t off = 12;
        bool have_fmt = false;
        while (off + 8 <= (size_t)got) {
            uint32_t csize = audio_rd_le32(hdr + off + 4);
            const uint8_t *id = hdr + off;
            if (memcmp(id, "fmt ", 4) == 0) {
                if (off + 8 + 16 > (size_t)got) {
                    break;
                }
                const uint8_t *f = hdr + off + 8;
                audio_fmt_      = audio_rd_le16(f + 0);
                channels_       = audio_rd_le16(f + 2);
                rate_           = audio_rd_le32(f + 4);
                bits_           = audio_rd_le16(f + 14);
                have_fmt        = true;
            } else if (memcmp(id, "data", 4) == 0) {
                if (!have_fmt) {
                    break; /* data 在 fmt 之前：异常文件 */
                }
                data_bytes_ = csize;
                /* 数据从当前逻辑位置往后 off+8 处开始（peek 未移动位置） */
                data_skip_ = off + 8;
                break;
            }
            off += 8 + csize + (csize & 1); /* chunk 按偶数字节对齐 */
        }

        if (!have_fmt || data_bytes_ == 0 || channels_ == 0 || rate_ == 0 || bits_ == 0) {
            if (dec_err) {
                *dec_err = AUDIO_DEC_ERR_PARSE;
            }
            AUDIO_LOGE("WAV header incomplete (fmt=%d ch=%u rate=%u bits=%u data=%u)", (int)have_fmt,
                       (unsigned)channels_, (unsigned)rate_, (unsigned)bits_, (unsigned)data_bytes_);
            return AUDIO_ERR_PARSE;
        }
        if (audio_fmt_ != 1 && audio_fmt_ != 3) {
            /* 1 = PCM, 3 = IEEE float；其余（ADPCM 等）暂不支持 */
            AUDIO_LOGE("unsupported WAV format tag 0x%04X", (unsigned)audio_fmt_);
            if (dec_err) {
                *dec_err = AUDIO_DEC_ERR_OPEN;
            }
            return AUDIO_ERR_NOT_SUPPORTED;
        }

        bytes_per_frame_in_ = (uint32_t)channels_ * (bits_ / 8u);
        if (bytes_per_frame_in_ == 0) {
            return AUDIO_ERR_PARSE;
        }
        data_left_ = data_bytes_; /* 必须在进入 decode 前初始化 */
        if (duration_ms && rate_) {
            *duration_ms = (int64_t)((uint64_t)data_bytes_ * 1000ull /
                                     ((uint64_t)bytes_per_frame_in_ * rate_));
        }

        /* 决定输出位深：**24/32-bit PCM 与 float32 走 int32 路径以保留精度**，
         * 不再一律压成 16-bit；8/16-bit 仍走 16-bit 路径。 */
        wide_out_ = (audio_fmt_ == 3) || (bits_ > 16);
        const uint8_t out_bits = wide_out_ ? ((bits_ == 24) ? 24 : 32) : 16;

        if (fmt) {
            fmt->rate     = rate_;
            fmt->channels = 2;
            fmt->bits     = out_bits;
        }
        fmt_.rate     = rate_;
        fmt_.channels = 2;
        fmt_.bits     = out_bits;
        if (out_) {
            out_->set_rate(rate_);
            out_->set_format(out_bits, 2);
        }

        buf_ = (uint8_t *)audio_alloc_big(bytes_per_frame_in_ * WAV_OUT_FRAMES, 0);
        /* int32 中间缓冲放堆：避免对象内再塞一份大数组 */
        pcm32_ = (int32_t *)audio_alloc_big(WAV_I32_CAP * sizeof(int32_t), 0);
        if (!buf_ || !pcm32_) {
            if (dec_err) {
                *dec_err = AUDIO_DEC_ERR_OPEN;
            }
            return AUDIO_ERR_NO_MEM;
        }

        state_ = AUDIO_DEC_STATE_ACTIVE;
        AUDIO_LOGI("WAV: %u Hz, %u ch, %u-bit, tag=%u, data=%u bytes -> out %u-bit%s",
                   (unsigned)rate_, (unsigned)channels_, (unsigned)bits_, (unsigned)audio_fmt_,
                   (unsigned)data_bytes_, (unsigned)out_bits,
                   wide_out_ ? " (high-res preserved)" : "");
        return AUDIO_OK;
    }

    audio_err_t decode() override
    {
        if (state_ != AUDIO_DEC_STATE_ACTIVE) {
            return AUDIO_ERR_EOF;
        }

        /* 首次调用时跳过 fmt 与 chunk 头，定位到 PCM 起始 */
        if (data_skip_ > 0) {
            uint8_t tmp[64];
            size_t left = data_skip_;
            while (left > 0) {
                size_t chunk = left < sizeof(tmp) ? left : sizeof(tmp);
                int n = in_->read(tmp, chunk);
                if (n <= 0) {
                    state_ = AUDIO_DEC_STATE_ERROR;
                    return AUDIO_ERR_IO;
                }
                left -= (size_t)n;
            }
            data_skip_ = 0;
        }

        if (data_left_ == 0) {
            state_ = AUDIO_DEC_STATE_EOS;
            return AUDIO_ERR_EOF;
        }

        size_t max_in = bytes_per_frame_in_ * WAV_OUT_FRAMES;
        if (max_in > data_left_) {
            max_in = data_left_;
        }
        /* 只读整帧 */
        max_in = (max_in / bytes_per_frame_in_) * bytes_per_frame_in_;
        if (max_in == 0) {
            data_left_ = 0;
            state_ = AUDIO_DEC_STATE_EOS;
            return AUDIO_ERR_EOF;
        }

        /* 若数据源提前结束，read_full 会短读，截断到整帧边界 */
        size_t got = in_->read_full(buf_, max_in);
        got = (got / bytes_per_frame_in_) * bytes_per_frame_in_;
        if (got == 0) {
            data_left_ = 0;
            state_ = AUDIO_DEC_STATE_EOS;
            return AUDIO_ERR_EOF;
        }
        data_left_ -= (uint32_t)got;

        size_t frames = got / bytes_per_frame_in_;
        const size_t nsamp = frames * 2;
        if (nsamp > WAV_I32_CAP) {
            return AUDIO_ERR_IO;
        }

        /* 交给 sink：契约要求"全写出或因 stop 放弃" */
        int rc;
        if (wide_out_) {
            /* 24/32-bit 源：以 int32 输出，精度完整保留（对齐 FLAC 的做法） */
            make_samples_i32(buf_, frames, pcm32_);
            rc = out_->write_i32(pcm32_, frames, fmt_.bits, 0);
            if (rc == AUDIO_ERR_NOT_SUPPORTED) {
                /* sink 不支持 int32：退化为 16-bit（有精度损失，仅兜底） */
                const int sh = (fmt_.bits >= 16) ? ((int)fmt_.bits - 16) : 0;
                for (size_t i = 0; i < nsamp; i++) {
                    out_pcm_[i] = (int16_t)(sh ? (pcm32_[i] >> sh) : pcm32_[i]);
                }
                rc = out_->write(out_pcm_, nsamp * sizeof(int16_t), 0);
            }
        } else {
            make_samples(buf_, frames, out_pcm_);
            for (size_t i = 0; i < nsamp; i++) {
                pcm32_[i] = (int32_t)out_pcm_[i] << 16;
            }
            rc = out_->write_i32(pcm32_, frames, 16, 0);
            if (rc == AUDIO_ERR_NOT_SUPPORTED) {
                rc = out_->write(out_pcm_, nsamp * sizeof(int16_t), 0);
            }
        }
        if (rc == AUDIO_ERR_NOT_SUPPORTED) {
            state_ = AUDIO_DEC_STATE_EOS; /* 被停止打断 */
            return AUDIO_ERR_EOF;
        }
        if (rc != AUDIO_OK) {
            state_ = AUDIO_DEC_STATE_ERROR;
            last_error_ = AUDIO_DEC_ERR_OPEN;
            return (audio_err_t)rc;
        }
        return AUDIO_OK;
    }

    void reset() override
    {
        /* data_skip_ 只在首次生效；seek 由调用者负责，这里只复位统计 */
        data_left_ = data_bytes_;
    }

    void close() override
    {
        if (buf_) {
            audio_free(buf_);
            buf_ = nullptr;
        }
        audio_free(pcm32_);
        pcm32_ = nullptr;
        state_ = AUDIO_DEC_STATE_IDLE;
    }

private:
    /** 把输入 PCM 归一化成 16-bit 交错立体声 */
    void make_samples(const uint8_t *src, size_t frames, int16_t *dst)
    {
        for (size_t i = 0; i < frames; i++) {
            int16_t l = 0;
            int16_t r = 0;
            const uint8_t *p = src + i * bytes_per_frame_in_;
            for (uint8_t c = 0; c < channels_ && c < 2; c++) {
                int16_t v = convert_one(p + (size_t)c * (bits_ / 8u));
                if (c == 0) {
                    l = v;
                } else {
                    r = v;
                }
            }
            if (channels_ == 1) {
                r = l;
            }
            dst[i * 2 + 0] = l;
            dst[i * 2 + 1] = r;
        }
    }

    int16_t convert_one(const uint8_t *p)
    {
        if (audio_fmt_ == 1) { /* PCM */
            switch (bits_) {
            case 8:
                return (int16_t)(((int16_t)(*p) - 128) << 8);
            case 16:
                return (int16_t)audio_rd_le16(p);
            case 24: {
                int32_t v = (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16));
                if (v & 0x800000) {
                    v |= (int32_t)0xFF000000;
                }
                return (int16_t)(v >> 8);
            }
            case 32: {
                int32_t v = (int32_t)audio_rd_le32(p);
                return (int16_t)(v >> 16);
            }
            default:
                return 0;
            }
        } else { /* IEEE float32 */
            float f;
            uint32_t u = audio_rd_le32(p);
            memcpy(&f, &u, sizeof(f));
            if (f > 1.0f) {
                f = 1.0f;
            }
            if (f < -1.0f) {
                f = -1.0f;
            }
            return (int16_t)(f * 32767.0f);
        }
    }

    /** 把输入 PCM 转成 int32（有效数据对齐到高位），保留 24/32-bit 精度 */
    void make_samples_i32(const uint8_t *src, size_t frames, int32_t *dst)
    {
        for (size_t i = 0; i < frames; i++) {
            int32_t l = 0;
            int32_t r = 0;
            const uint8_t *p = src + i * bytes_per_frame_in_;
            for (uint8_t c = 0; c < channels_ && c < 2; c++) {
                int32_t v = convert_one_i32(p + (size_t)c * (bits_ / 8u));
                if (c == 0) {
                    l = v;
                } else {
                    r = v;
                }
            }
            if (channels_ == 1) {
                r = l;
            }
            dst[i * 2 + 0] = l;
            dst[i * 2 + 1] = r;
        }
    }

    /**
     * @brief 单样本 -> int32（有效数据对齐到高位）。
     *
     * 对齐规则与 FLAC 解码器保持一致：16-bit -> `<<16`，24-bit -> `<<8`，
     * 32-bit -> 原值。这样 sink 在 int32 域做缩放/位宽下采样时不会丢精度。
     */
    int32_t convert_one_i32(const uint8_t *p) const
    {
        if (audio_fmt_ == 1) { /* PCM */
            switch (bits_) {
            case 16:
                return (int32_t)((int16_t)audio_rd_le16(p)) << 16;
            case 24: {
                int32_t v =
                    (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16));
                if (v & 0x800000) {
                    v |= (int32_t)0xFF000000;
                }
                return v << 8; /* 24 位有效数据放到高位 */
            }
            case 32:
                return (int32_t)audio_rd_le32(p);
            default:
                return 0;
            }
        } else { /* IEEE float32：归一化到 int32 满量程 */
            float f;
            uint32_t u = audio_rd_le32(p);
            memcpy(&f, &u, sizeof(f));
            if (f > 1.0f) {
                f = 1.0f;
            }
            if (f < -1.0f) {
                f = -1.0f;
            }
            return (int32_t)(f * 2147483647.0f);
        }
    }

    AudioInput *in_ = nullptr;
    AudioSink  *out_ = nullptr;
    uint8_t    *buf_ = nullptr;

    uint16_t audio_fmt_ = 0;      /**< 1 = PCM, 3 = float */
    bool     wide_out_ = false;   /**< true = 走 int32 输出路径（24/32-bit / float32） */
    uint16_t channels_ = 0;
    uint32_t rate_ = 0;
    uint16_t bits_ = 0;
    uint32_t bytes_per_frame_in_ = 0;

    uint32_t data_bytes_ = 0;     /**< data chunk 声明的字节数 */
    uint32_t data_left_ = 0;      /**< 尚需读取的字节数 */
    uint32_t data_skip_ = 0;      /**< 首次 decode 前要跳过的头长度 */

    int16_t out_pcm_[WAV_OUT_FRAMES * 2];
    int32_t *pcm32_ = nullptr; /**< 统一 int32 域缓冲（堆） */
};

/* ===========================================================================
 * 注册项
 * =========================================================================*/

bool probe_wav(AudioInput *in)
{
    uint8_t h[12];
    if (!in || in->peek(h, sizeof(h), 12) < 12) {
        return false;
    }
    return memcmp(h, "RIFF", 4) == 0 && memcmp(h + 8, "WAVE", 4) == 0;
}

AudioDecoder *create_wav(void)
{
    return new (std::nothrow) DecoderWav();
}

#else /* !CONFIG_ESPAUDIOCORE_ENABLE_WAV */

bool probe_wav(AudioInput *in)
{
    (void)in;
    return false;
}

AudioDecoder *create_wav(void)
{
    return nullptr;
}

#endif