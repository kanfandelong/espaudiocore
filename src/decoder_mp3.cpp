/*
 * SPDX-License-Identifier: RPSL-1.0 AND MIT
 * Copyright (c) 2026 kanfandelong. All rights reserved.
 *
 * This file contains code derived from the Helix AAC decoder,
 * which is licensed under the RealNetworks Public Source License
 * Version 1.0 (RPSL-1.0). Portions of this file are subject to
 * the terms of RPSL-1.0. All other code is licensed under MIT.
 *
 * The full text of RPSL-1.0 is available at:
 * https://opensource.org/licenses/RPSL-1.0
 * The full text of MIT is available at:
 * https://opensource.org/licenses/MIT
 */

/**
 * @file    decoder_mp3.cpp
 * @brief   MP3 解码器（libhelix-mp3）。
 */

#include "audio_port.h"
#include "audio_types.h"
#include "decoder.h"
#include "id3_parser.h"
#include "xing_parser.h"

#include <new>
#include <stdlib.h>
#include <string.h>

extern "C" {
#include "mp3dec.h"
}

#if defined(CONFIG_ESPAUDIOCORE_ENABLE_MP3)

/* 与原实现一致：1600 字节足以容纳最大 MPEG 帧（1440）+ 余量 */
#define MP3_IN_BUF 1600
#define MP3_OUT_SAMPLES (1152 * 2)
/** int32 中间缓冲容量：每帧最多 1152 个立体声帧 = 2304 个采样点 */
#define MP3_I32_CAP MP3_OUT_SAMPLES

class DecoderMp3 : public AudioDecoder {
public:
    ~DecoderMp3() override
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

        h_ = MP3InitDecoder();
        if (!h_) {
            AUDIO_LOGE("MP3InitDecoder failed (out of memory)");
            if (dec_err) {
                *dec_err = AUDIO_DEC_ERR_OPEN;
            }
            return AUDIO_ERR_NO_MEM;
        }

        buf_  = (uint8_t *)audio_alloc_big(MP3_IN_BUF, 0);
        pcm_  = (int16_t *)audio_alloc_big(MP3_OUT_SAMPLES * sizeof(int16_t), 0);
        pcm32_ = (int32_t *)audio_alloc_big(MP3_OUT_SAMPLES * sizeof(int32_t), 0);
        xing_ = (uint8_t *)audio_alloc_big(2880, 0);
        if (!buf_ || !pcm_ || !pcm32_ || !xing_) {
            if (dec_err) {
                *dec_err = AUDIO_DEC_ERR_OPEN;
            }
            close();
            return AUDIO_ERR_NO_MEM;
        }
        memset(buf_, 0, MP3_IN_BUF);

        /* 1) ID3v2 标签（文本 + 图片）解析并上报；返回跳过的字节数 */
        id3_bytes_ = id3_parse_and_report(in_, meta_cb_, meta_user_);

        /* 2) Xing/LAME 头 -> 精确时长（依赖可 seek 的源） */
        first_frame_pos_ = (uint32_t)in_->tell();
        if (in_->can_seek()) {
            int n = in_->read(xing_, 2880);
            if (n > 4) {
                XingHeaderInfo xi = parseXingHeader(xing_, (size_t)n);
                if (xi.valid) {
                    duration_hint_ms_ = (int64_t)(xi.duration * 1000.0f);
                    AUDIO_LOGI("MP3 Xing: frames=%u bitrate=%u duration=%.2fs rate=%d ch=%d",
                               (unsigned)xi.frames, (unsigned)xi.bitrate, (double)xi.duration,
                               xi.sampleRate, xi.channels);
                    report_duration(duration_hint_ms_);
                }
            }
            /* 回退到帧起点重新解码 */
            (void)in_->seek((int64_t)first_frame_pos_, SEEK_SET);
        }

        /* 3) 先解一帧，拿到真实格式 */
        if (!fill_valid_frame()) {
            AUDIO_LOGW("MP3: no valid frame found");
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
        if (duration_ms) {
            *duration_ms = duration_hint_ms_;
        }

        state_ = AUDIO_DEC_STATE_ACTIVE;
        AUDIO_LOGI("MP3: %u Hz, %u ch, %u-bit (id3=%u bytes)", (unsigned)fmt_.rate,
                   (unsigned)fmt_.channels, (unsigned)fmt_.bits, (unsigned)id3_bytes_);
        return AUDIO_OK;
    }

    audio_err_t decode() override
    {
        if (state_ != AUDIO_DEC_STATE_ACTIVE) {
            return AUDIO_ERR_EOF;
        }

        /* 尚有未写出的样本：整帧一次交给 sink（统一 int32 域，见 D-23） */
        if (valid_samples_ > 0) {
            const size_t nsamp = (size_t)valid_samples_ * fmt_.channels;
            if (nsamp > MP3_I32_CAP) {
                return AUDIO_ERR_IO;
            }
            for (size_t i = 0; i < nsamp; i++) {
                pcm32_[i] = (int32_t)pcm_[i] << 16; /* int16 -> int32 高位对齐 */
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

        /* 解下一帧 */
        if (!fill_valid_frame()) {
            state_ = AUDIO_DEC_STATE_EOS;
            if (duration_hint_ms_ < 0 && !est_reported_) {
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
            /* 单帧坏数据：跳过，不终止整条流（与原实现一致） */
            return AUDIO_OK;
        }
        return AUDIO_OK;
    }

    void reset() override
    {
        /* seek 之后：丢弃缓冲，下次重新找同步字 */
        buff_valid_ = last_frame_end_ = 0;
        valid_samples_ = 0;
        eof_ = false;
        first_sync_done_ = false;
        state_ = AUDIO_DEC_STATE_ACTIVE;
    }

    void close() override
    {
        if (h_) {
            MP3FreeDecoder(h_);
            h_ = nullptr;
        }
        audio_free(buf_);
        buf_ = nullptr;
        audio_free(pcm_);
        pcm_ = nullptr;
        audio_free(pcm32_);
        pcm32_ = nullptr;
        audio_free(xing_);
        xing_ = nullptr;
        buff_valid_ = last_frame_end_ = valid_samples_ = 0;
        state_ = AUDIO_DEC_STATE_IDLE;
    }

private:
    void report_duration(int64_t ms)
    {
        if (ms < 0 || !meta_cb_) {
            return;
        }
        char buf[24];
        snprintf(buf, sizeof(buf), "%lld", (long long)ms);
        espaudiocore_meta_t m = {};
        m.type      = "tlen"; /* 与原实现一致的长度元数据 key */
        m.is_binary = false;
        m.data      = buf;
        m.len       = strlen(buf);
        m.pic_type  = -1;
        meta_cb_(meta_user_, &m);
    }

    /** 用已解码帧数估算总时长（无 Xing 头时的兜底，逻辑同原实现） */
    int64_t estimate_duration_ms()
    {
        if (bitrate_count_ < 50 || avg_bitrate_ == 0 || file_size_ == 0) {
            return -1;
        }
        uint64_t total_bytes = (file_size_ > first_frame_pos_) ? (file_size_ - first_frame_pos_) : 0;
        uint64_t total_ms = (total_bytes * 8ull * 1000ull) / avg_bitrate_;
        return (int64_t)total_ms;
    }

    /**
     * @brief 读到至少一个完整帧并把同步字移到缓冲头部。
     *        逻辑移植自 AudioGeneratorMP3a::FillBufferWithValidFrame()。
     */
    bool fill_valid_frame()
    {
        buf_[0] = 0; /* 破坏上次残留的同步字，避免误判 */
        int next_sync;
        do {
            next_sync = MP3FindSyncWord(buf_ + last_frame_end_, (int)buff_valid_ - last_frame_end_);
            if (next_sync >= 0) {
                next_sync += last_frame_end_;
            }
            last_frame_end_ = 0;
            if (next_sync == -1) {
                if (buff_valid_ > 0 && buf_[buff_valid_ - 1] == 0xff) {
                    /* 可能是同步字的前半，保留它 */
                    buf_[0] = 0xff;
                    int n = in_->read(buf_ + 1, MP3_IN_BUF - 1);
                    buff_valid_ = (n > 0) ? (int16_t)(n + 1) : 0;
                    if (buff_valid_ <= 1) {
                        return false; /* EOF */
                    }
                } else {
                    int n = in_->read(buf_, MP3_IN_BUF);
                    buff_valid_ = (n > 0) ? (int16_t)n : 0;
                    if (buff_valid_ == 0) {
                        return false; /* EOF */
                    }
                }
            }
        } while (next_sync == -1);

        /* 丢掉同步字之前的数据 */
        buff_valid_ = (int16_t)(buff_valid_ - next_sync);
        memmove(buf_, buf_ + next_sync, (size_t)buff_valid_);

        /* 尽量把缓冲补满 */
        if (buff_valid_ < MP3_IN_BUF) {
            int n = in_->read(buf_ + buff_valid_, MP3_IN_BUF - buff_valid_);
            if (n > 0) {
                buff_valid_ = (int16_t)(buff_valid_ + n);
            }
        }
        return true;
    }

    /** 解一帧到 pcm_，结果样本数在 valid_samples_ */
    audio_err_t decode_frame()
    {
        unsigned char *in_buff = buf_;
        int bytes_left = buff_valid_;
        int ret = MP3Decode(h_, &in_buff, &bytes_left, pcm_, 0);

        if (ret) {
            AUDIO_LOGD("MP3 decode error %d (skipped)", ret);
            return AUDIO_ERR_DECODE;
        }

        last_frame_end_ = (int16_t)(buff_valid_ - bytes_left);

        MP3FrameInfo fi;
        MP3GetLastFrameInfo(h_, &fi);
        if (fi.outputSamps <= 0 || fi.nChans <= 0 || fi.samprate <= 0) {
            return AUDIO_ERR_DECODE;
        }

        if ((uint32_t)fi.samprate != fmt_.rate) {
            fmt_.rate = (uint32_t)fi.samprate;
            fmt_.bits = 16;
            fmt_.channels = (uint8_t)fi.nChans;
            if (out_) {
                out_->set_rate(fmt_.rate);
                out_->set_format(fmt_.bits, fmt_.channels);
            }
        } else if ((uint8_t)fi.nChans != fmt_.channels) {
            fmt_.channels = (uint8_t)fi.nChans;
            if (out_) {
                out_->set_format(fmt_.bits, fmt_.channels);
            }
        }

        valid_samples_ = (int16_t)(fi.outputSamps / fmt_.channels);

        /* 比特率累加 -> 时长估算（逻辑同原实现，但只上报一次，避免刷屏） */
        if (duration_hint_ms_ < 0 && !est_reported_ && fi.bitrate > 0) {
            bitrate_sum_ += (uint64_t)fi.bitrate * 1000ull;
            bitrate_count_++;
            if (bitrate_count_ >= 50) {
                avg_bitrate_ = bitrate_sum_ / bitrate_count_;
                est_reported_ = true;
                report_duration(estimate_duration_ms());
            }
        }
        return AUDIO_OK;
    }

    AudioInput   *in_ = nullptr;
    AudioSink    *out_ = nullptr;
    HMP3Decoder   h_ = nullptr;
    uint8_t      *buf_ = nullptr;
    int16_t      *pcm_ = nullptr;
    int32_t      *pcm32_ = nullptr; /**< 统一 int32 域中间缓冲 */
    uint8_t      *xing_ = nullptr;

    espaudiocore_meta_cb_t meta_cb_ = nullptr;
    void                  *meta_user_ = nullptr;

    int16_t  buff_valid_ = 0;
    int16_t  last_frame_end_ = 0;
    int16_t  valid_samples_ = 0;
    bool     eof_ = false;
    bool     first_sync_done_ = false;

    size_t   id3_bytes_ = 0;
    uint32_t first_frame_pos_ = 0;
    uint32_t file_size_ = 0; /* 由 player 在 open 前设置不了，估时长用 tell/size */
    int64_t  duration_hint_ms_ = -1;

    uint64_t bitrate_sum_ = 0;
    uint32_t bitrate_count_ = 0;
    uint64_t avg_bitrate_ = 0;
    bool     est_reported_ = false; /**< 时长估算是否已上报 */
};

/** 魔数探测：MP3 无固定魔数，靠帧同步字 + 字段合法性交叉验证 */
bool probe_mp3(AudioInput *in)
{
    uint8_t h[16];
    if (!in) {
        return false;
    }
    int got = in->peek(h, sizeof(h), 10);
    if (got < 10) {
        return false;
    }
    if (memcmp(h, "ID3", 3) == 0) {
        return true;
    }
    for (int i = 0; i + 4 <= got; i++) {
        if (h[i] != 0xFF || (h[i + 1] & 0xE0) != 0xE0) {
            continue;
        }
        uint8_t ver = (h[i + 1] >> 3) & 0x03;
        uint8_t layer = (h[i + 1] >> 1) & 0x03;
        uint8_t br = (h[i + 2] >> 4) & 0x0F;
        uint8_t sr = (h[i + 2] >> 2) & 0x03;
        if (ver != 1 && layer != 0 && br != 0 && br != 15 && sr != 3) {
            return true;
        }
    }
    return false;
}

AudioDecoder *create_mp3(void)
{
    return new (std::nothrow) DecoderMp3();
}

#else /* !CONFIG_ESPAUDIOCORE_ENABLE_MP3 */

bool probe_mp3(AudioInput *in)
{
    (void)in;
    return false;
}

AudioDecoder *create_mp3(void)
{
    return nullptr;
}

#endif
