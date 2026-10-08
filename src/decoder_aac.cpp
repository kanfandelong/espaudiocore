/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 kanfandelong. All rights reserved.
 */

/**
 * @file    decoder_aac.cpp
 * @brief   AAC 解码器（libhelix-aac，含 SBR）。
 */

#include "audio_port.h"
#include "audio_types.h"
#include "decoder.h"
#include "id3_parser.h"

#include <new>
#include <stdlib.h>
#include <string.h>

extern "C" {
#include "aacdec.h"
}

#if defined(CONFIG_ESPAUDIOCORE_ENABLE_AAC)

/** 与原实现一致：1600 字节足够容纳一个 AAC 帧 */
#define AAC_IN_BUF 1600
#define AAC_OUT_SAMPLES (1024 * 2)
/** int32 中间缓冲容量：SBR 下每帧最多 2048 个立体声帧 = 4096 个采样点 */
#define AAC_I32_CAP (AAC_OUT_SAMPLES * 2)

class DecoderAac : public AudioDecoder {
public:
    ~DecoderAac() override
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

        h_ = AACInitDecoder();
        if (!h_) {
            AUDIO_LOGE("AACInitDecoder failed (out of memory)");
            if (dec_err) {
                *dec_err = AUDIO_DEC_ERR_OPEN;
            }
            return AUDIO_ERR_NO_MEM;
        }

        buf_ = (uint8_t *)audio_alloc_big(AAC_IN_BUF, 0);
        pcm_ = (int16_t *)audio_alloc_big(AAC_OUT_SAMPLES * sizeof(int16_t), 0);
        pcm32_ = (int32_t *)audio_alloc_big(AAC_I32_CAP * sizeof(int32_t), 0);
        if (!buf_ || !pcm_ || !pcm32_) {
            if (dec_err) {
                *dec_err = AUDIO_DEC_ERR_OPEN;
            }
            close();
            return AUDIO_ERR_NO_MEM;
        }
        memset(buf_, 0, AAC_IN_BUF);

        /* 跳过 ID3v2 标签：ADTS 流的开头常常带 ID3（实测用户的 .aac 就是如此，
         * 头 10 字节为 "ID3"+v2.3+size=10）。不做这一步会直接从标签里找 0xFFF，
         * 必然找不到同步字并报 PARSE 失败。
         * 返回值是跳过的字节数；非 ID3 时它会自行 seek 回 0。 */
        id3_bytes_ = id3_parse_and_report(in_, meta_cb_, meta_user_);
        AUDIO_LOGD("AAC: skipped %u bytes of ID3", (unsigned)id3_bytes_);

        /* 先解一帧拿到真实格式 */
        if (!fill_valid_frame()) {
            AUDIO_LOGW("AAC: no valid frame found (after %u bytes of ID3)",
                       (unsigned)id3_bytes_);
            if (dec_err) {
                *dec_err = AUDIO_DEC_ERR_PARSE;
            }
            close();
            return AUDIO_ERR_PARSE;
        }
        audio_err_t r = decode_frame();
        if (r != AUDIO_OK) {
            if (dec_err) {
                *dec_err = AUDIO_DEC_ERR_PARSE;
            }
            close();
            return AUDIO_ERR_PARSE;
        }

        if (fmt) {
            *fmt = fmt_;
        }
        if (out_) {
            out_->set_rate(fmt_.rate);
            out_->set_format(fmt_.bits, fmt_.channels);
        }

        state_ = AUDIO_DEC_STATE_ACTIVE;
        AUDIO_LOGI("AAC: %u Hz, %u ch (SBR out), first frame %d samples", (unsigned)fmt_.rate,
                   (unsigned)fmt_.channels, (int)valid_samples_);
        return AUDIO_OK;
    }

    audio_err_t decode() override
    {
        if (state_ != AUDIO_DEC_STATE_ACTIVE) {
            return AUDIO_ERR_EOF;
        }

        if (valid_samples_ > 0) {
            /* 统一 int32 域（见 D-23） */
            const size_t nsamp = (size_t)valid_samples_ * fmt_.channels;
            if (nsamp > AAC_I32_CAP) {
                return AUDIO_ERR_IO;
            }
            for (size_t i = 0; i < nsamp; i++) {
                pcm32_[i] = (int32_t)pcm_[i] << 16;
            }
            int rc = out_->write_i32(pcm32_, valid_samples_, 16, 0);
            valid_samples_ = 0;
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

        if (!fill_valid_frame()) {
            state_ = AUDIO_DEC_STATE_EOS;
            if (duration_ms_ < 0 && !est_reported_) {
                est_reported_ = true;
                report_duration(estimate_duration_ms());
            }
            return AUDIO_ERR_EOF;
        }
        audio_err_t r = decode_frame();
        if (r == AUDIO_ERR_EOF) {
            state_ = AUDIO_DEC_STATE_EOS;
            return r;
        }
        if (r != AUDIO_OK) {
            return AUDIO_OK; /* 单帧坏数据：跳过（与原实现一致） */
        }
        return AUDIO_OK;
    }

    void reset() override
    {
        buff_valid_ = last_frame_end_ = 0;
        valid_samples_ = 0;
        eof_ = false;
        if (h_) {
            AACFlushCodec(h_);
        }
        state_ = AUDIO_DEC_STATE_ACTIVE;
    }

    void close() override
    {
        if (h_) {
            AACFreeDecoder(h_);
            h_ = nullptr;
        }
        audio_free(buf_);
        buf_ = nullptr;
        audio_free(pcm_);
        pcm_ = nullptr;
        audio_free(pcm32_);
        pcm32_ = nullptr;
        buff_valid_ = last_frame_end_ = valid_samples_ = 0;
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

    int64_t estimate_duration_ms()
    {
        if (bitrate_count_ < 50 || avg_bitrate_ == 0) {
            return -1;
        }
        int64_t sz = in_->size();
        if (sz <= 0) {
            return -1;
        }
        /* 只按**音频数据**算：avg_bitrate_ 来自音频帧，文件大小里还含 ID3 标签，
         * 不扣掉会让估算偏大（标签越大偏得越多）。 */
        uint64_t audio_bytes = (uint64_t)sz;
        if (id3_bytes_ > 0 && (uint64_t)id3_bytes_ < audio_bytes) {
            audio_bytes -= (uint64_t)id3_bytes_;
        }
        uint64_t total_ms = (audio_bytes * 8ull * 1000ull) / avg_bitrate_;
        return (int64_t)total_ms;
    }

    /** 移植自 AudioGeneratorAAC::FillBufferWithValidFrame() */
    bool fill_valid_frame()
    {
        buf_[0] = 0; /* 破坏残留同步字 */
        int next_sync;
        do {
            next_sync = AACFindSyncWord(buf_ + last_frame_end_, (int)buff_valid_ - last_frame_end_);
            if (next_sync >= 0) {
                next_sync += last_frame_end_;
            }
            last_frame_end_ = 0;
            if (next_sync == -1) {
                if (buff_valid_ > 0 && buf_[buff_valid_ - 1] == 0xff) {
                    /* 可能是同步字前半，保留 */
                    buf_[0] = 0xff;
                    int n = in_->read(buf_ + 1, AAC_IN_BUF - 1);
                    buff_valid_ = (n > 0) ? (int16_t)(n + 1) : 0;
                    if (buff_valid_ <= 1) {
                        return false; /* EOF */
                    }
                } else {
                    int n = in_->read(buf_, AAC_IN_BUF - 1);
                    buff_valid_ = (n > 0) ? (int16_t)n : 0;
                    if (buff_valid_ == 0) {
                        return false; /* EOF */
                    }
                }
            }
        } while (next_sync == -1);

        buff_valid_ = (int16_t)(buff_valid_ - next_sync);
        memmove(buf_, buf_ + next_sync, (size_t)buff_valid_);

        if (buff_valid_ < AAC_IN_BUF) {
            int n = in_->read(buf_ + buff_valid_, AAC_IN_BUF - buff_valid_);
            if (n > 0) {
                buff_valid_ = (int16_t)(buff_valid_ + n);
            }
        }
        return true;
    }

    audio_err_t decode_frame()
    {
        unsigned char *in_buff = buf_;
        int bytes_left = buff_valid_;
        int ret = AACDecode(h_, &in_buff, &bytes_left, pcm_);
        if (ret) {
            AUDIO_LOGD("AAC decode error %d (skipped)", ret);
            return AUDIO_ERR_DECODE;
        }

        last_frame_end_ = (int16_t)(buff_valid_ - bytes_left);

        AACFrameInfo fi;
        AACGetLastFrameInfo(h_, &fi);
        if (fi.sampRateOut <= 0 || fi.nChans <= 0 || fi.outputSamps <= 0) {
            return AUDIO_ERR_DECODE;
        }

        /* 只在格式**真的变化**时通知 sink：本函数逐帧执行，无条件调用会做两次
         * 虚函数调用；更糟的是曾因 sink 侧缺少变化判断而把 UART 刷爆——
         * 实测 94 秒的 AAC 打了 4000+ 行 "sample rate -> ..."，日志输出本身
         * 把解码任务卡住，测试停在第一个文件不再前进。 */
        if (fmt_.rate != (uint32_t)fi.sampRateOut || fmt_.channels != (uint8_t)fi.nChans) {
            fmt_.rate     = (uint32_t)fi.sampRateOut;
            fmt_.bits     = 16;
            fmt_.channels = (uint8_t)fi.nChans;
            if (out_) {
                out_->set_rate(fmt_.rate);
                out_->set_format(fmt_.bits, fmt_.channels);
            }
        }

        /* outputSamps 是总样本数（含各声道），除以声道数得到帧数 */
        valid_samples_ = (int16_t)(fi.outputSamps / fi.nChans);

        /* 码率累加 -> 时长估算。只上报一次（早期版本每 50 帧重报，实测刷了 80 多行）。
         * 帧时长必须用**实际每帧样本数**：SBR 流的输出样本数不是固定 1024，
         * 硬编码会让帧时长偏小、估算时长偏大（实测 131630ms vs 实际 94400ms）。 */
        if (duration_ms_ < 0 && !est_reported_ && last_frame_end_ > 0) {
            const int spf = (fi.nChans > 0) ? (fi.outputSamps / fi.nChans) : 1024;
            if (spf > 0) {
                float frame_dur_sec = (float)spf / (float)fi.sampRateOut;
                if (frame_dur_sec > 0.0f) {
                    uint32_t br = (uint32_t)(((float)last_frame_end_ * 8.0f) / frame_dur_sec);
                    bitrate_sum_ += br;
                    bitrate_count_++;
                    /* 攒够 50 帧再报一次，之后不再重复（解码继续，但保持安静） */
                    if (bitrate_count_ >= 50) {
                        avg_bitrate_ = bitrate_sum_ / bitrate_count_;
                        est_reported_ = true;
                        report_duration(estimate_duration_ms());
                    }
                }
            }
        }
        return AUDIO_OK;
    }

    AudioInput   *in_ = nullptr;
    AudioSink    *out_ = nullptr;
    HAACDecoder   h_ = nullptr;
    uint8_t      *buf_ = nullptr;
    int16_t      *pcm_ = nullptr;
    int32_t      *pcm32_ = nullptr; /**< 统一 int32 域中间缓冲 */

    espaudiocore_meta_cb_t meta_cb_ = nullptr;
    void                  *meta_user_ = nullptr;

    int16_t  buff_valid_ = 0;
    int16_t  last_frame_end_ = 0;
    int16_t  valid_samples_ = 0;
    bool     eof_ = false;

    uint32_t id3_bytes_ = 0; /**< 跳过的 ID3 字节数 */

    int64_t  duration_ms_ = -1;
    bool     est_reported_ = false; /**< 时长估算是否已上报（避免重复刷屏） */
    uint64_t bitrate_sum_ = 0;
    uint32_t bitrate_count_ = 0;
    uint64_t avg_bitrate_ = 0;
};

/** 魔数探测：ADTS 同步字 0xFFF + layer 必须为 0 */
bool probe_aac(AudioInput *in)
{
    uint8_t h[8];
    if (!in) {
        return false;
    }
    int got = in->peek(h, sizeof(h), 4);
    if (got < 4) {
        return false;
    }
    if (h[0] != 0xFF || (h[1] & 0xF0) != 0xF0) {
        return false;
    }
    /* ADTS：layer 位必须为 00，sampling_frequency_index 不能是 15 */
    if (((h[1] >> 1) & 0x03) != 0) {
        return false;
    }
    if (((h[2] >> 2) & 0x0F) == 0x0F) {
        return false;
    }
    return true;
}

AudioDecoder *create_aac(void)
{
    return new (std::nothrow) DecoderAac();
}

#else /* !CONFIG_ESPAUDIOCORE_ENABLE_AAC */

bool probe_aac(AudioInput *in)
{
    (void)in;
    return false;
}

AudioDecoder *create_aac(void)
{
    return nullptr;
}

#endif
