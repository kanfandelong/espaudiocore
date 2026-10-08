/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 kanfandelong. All rights reserved.
 */

/**
 * @file    decoder_vorbis.cpp
 * @brief   OGG Vorbis 解码器（Tremor 版 libvorbisfile + libogg）。
 * @note vorbisfile 需要 seek 能力，因此 **ringbuf 输入不可用**
 * （ringbuf 源的 can_seek() 为 false，ov_open_callbacks 会失败）。
 */

#include "audio_port.h"
#include "audio_types.h"
#include "decoder.h"

#include <new>
#include <stdlib.h>
#include <string.h>

extern "C" {
#include "ivorbisfile.h"
}

#if defined(CONFIG_ESPAUDIOCORE_ENABLE_VORBIS)

#define OGG_READ_BYTES 4096
/** int32 中间缓冲容量（采样点）：OGG_READ_BYTES 最多产生 2048 个 int16 采样点 */
#define VORBIS_I32_CAP (OGG_READ_BYTES / 2)
#define VORBIS_MAX_CHANNELS 8

class DecoderVorbis : public AudioDecoder {
public:
    ~DecoderVorbis() override
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
            /* vorbisfile 必须能 seek；ringbuf 源不支持（见设计 D-4） */
            AUDIO_LOGE("Vorbis requires a seekable source (ringbuf input is not supported)");
            if (dec_err) {
                *dec_err = AUDIO_DEC_ERR_OPEN;
            }
            return AUDIO_ERR_NOT_SUPPORTED;
        }

        vf_ = (OggVorbis_File *)calloc(1, sizeof(OggVorbis_File));
        if (!vf_) {
            if (dec_err) {
                *dec_err = AUDIO_DEC_ERR_OPEN;
            }
            return AUDIO_ERR_NO_MEM;
        }

        ov_callbacks cb;
        cb.read_func  = read_cb;
        cb.seek_func  = seek_cb;
        cb.close_func = close_cb;
        cb.tell_func  = tell_cb;

        int err = ov_open_callbacks(this, vf_, nullptr, 0, cb);
        if (err < 0) {
            AUDIO_LOGE("ov_open_callbacks failed: %d", err);
            if (dec_err) {
                *dec_err = AUDIO_DEC_ERR_OPEN;
            }
            close();
            return AUDIO_ERR_PARSE;
        }
        opened_ = true;

        vorbis_info *vi = ov_info(vf_, -1);
        if (!vi || vi->channels <= 0 || vi->rate <= 0) {
            AUDIO_LOGE("ov_info failed");
            if (dec_err) {
                *dec_err = AUDIO_DEC_ERR_PARSE;
            }
            close();
            return AUDIO_ERR_PARSE;
        }

        fmt_.rate     = (uint32_t)vi->rate;
        fmt_.channels = (uint8_t)vi->channels;
        fmt_.bits     = 16;

        /* 时长：优先用 ov_time_total（毫秒） */
        ogg_int64_t total_ms = ov_time_total(vf_, -1);
        if (total_ms > 0) {
            duration_hint_ms_ = (int64_t)total_ms;
            report_duration(duration_hint_ms_);
        }

        /* Vorbis comment 标签上报 */
        report_tags();

        pcm_ = (int16_t *)audio_alloc_big(OGG_READ_BYTES, 0);
        pcm32_ = (int32_t *)audio_alloc_big(VORBIS_I32_CAP * sizeof(int32_t), 0);
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
        AUDIO_LOGI("Vorbis: %u Hz, %u ch, %lld ms", (unsigned)fmt_.rate, (unsigned)fmt_.channels,
                   (long long)duration_hint_ms_);
        return AUDIO_OK;
    }

    audio_err_t decode() override
    {
        if (state_ != AUDIO_DEC_STATE_ACTIVE) {
            return AUDIO_ERR_EOF;
        }

        int section = 0;
        long ret = ov_read(vf_, (char *)pcm_, OGG_READ_BYTES, &section);
        if (ret == 0) {
            state_ = AUDIO_DEC_STATE_EOS;
            return AUDIO_ERR_EOF;
        }
        if (ret < 0) {
            if (ret == OV_HOLE) {
                return AUDIO_OK; /* 数据空洞：跳过继续（与原实现一致） */
            }
            AUDIO_LOGE("ov_read failed: %ld", ret);
            state_ = AUDIO_DEC_STATE_ERROR;
            return AUDIO_ERR_DECODE;
        }

        /* ret 是字节数；声道数可能随链接变化，按当前值切整帧 */
        size_t frame_bytes = (size_t)fmt_.channels * sizeof(int16_t);
        if (frame_bytes == 0) {
            return AUDIO_ERR_DECODE;
        }
        size_t bytes = ((size_t)ret / frame_bytes) * frame_bytes;
        if (bytes == 0) {
            return AUDIO_OK;
        }

        /* 统一 int32 域：Tremor 输出 int16，这里对齐成 int32（<<16），
         * 之后由 sink 在 int32 域做音量缩放与位宽输出（见 D-23）。 */
        const size_t nsamp = bytes / sizeof(int16_t);
        if (nsamp > VORBIS_I32_CAP) {
            /* 容量保护：本不该发生（ov_read 受 OGG_READ_BYTES 限制） */
            return AUDIO_ERR_IO;
        }
        for (size_t i = 0; i < nsamp; i++) {
            pcm32_[i] = (int32_t)pcm_[i] << 16;
        }
        const size_t pairs = nsamp / (fmt_.channels ? fmt_.channels : 2);

        int rc = out_->write_i32(pcm32_, pairs, 16, 0);
        if (rc == AUDIO_ERR_NOT_SUPPORTED) {
            /* sink 不支持统一路径：退回原字节接口 */
            rc = out_->write(pcm_, bytes, 0);
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

    void reset() override
    {
        if (opened_ && vf_) {
            (void)ov_raw_seek(vf_, 0);
        }
        state_ = AUDIO_DEC_STATE_ACTIVE;
    }

    void close() override
    {
        if (vf_) {
            if (opened_) {
                ov_clear(vf_);
            }
            free(vf_);
            vf_ = nullptr;
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

    /** 上报 Vorbis comment（与原实现一致：VENDOR 单独一个 key） */
    void report_tags()
    {
        if (!meta_cb_) {
            return;
        }
        vorbis_comment *vc = ov_comment(vf_, -1);
        if (!vc) {
            AUDIO_LOGW("no Vorbis comments");
            return;
        }
        if (vc->vendor) {
            espaudiocore_meta_t m = {};
            m.type      = "VENDOR";
            m.is_binary = false;
            m.data      = vc->vendor;
            m.len       = strlen(vc->vendor);
            m.pic_type  = -1;
            meta_cb_(meta_user_, &m);
        }
        for (int i = 0; i < vc->comments; i++) {
            const char *e = vc->user_comments[i];
            int len = vc->comment_lengths[i];
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
            m.is_binary = false; /* METADATA_BLOCK_PICTURE 是 base64，此处按文本透传 */
            m.data      = eq + 1;
            m.len       = (size_t)(len - klen - 1);
            m.pic_type  = -1;
            meta_cb_(meta_user_, &m);
        }
    }

    /* ---------------- ov_callbacks ---------------- */

    static size_t read_cb(void *ptr, size_t size, size_t nmemb, void *datasource)
    {
        DecoderVorbis *self = static_cast<DecoderVorbis *>(datasource);
        size_t want = size * nmemb;
        int n = self->in_->read(ptr, want);
        if (n <= 0) {
            return 0;
        }
        return (size_t)n / size;
    }

    static int seek_cb(void *datasource, ogg_int64_t offset, int whence)
    {
        DecoderVorbis *self = static_cast<DecoderVorbis *>(datasource);
        return (self->in_->seek((int64_t)offset, whence) == AUDIO_OK) ? 0 : -1;
    }

    static int close_cb(void *datasource)
    {
        (void)datasource; /* 源由 player 释放，这里不动 */
        return 0;
    }

    static long tell_cb(void *datasource)
    {
        DecoderVorbis *self = static_cast<DecoderVorbis *>(datasource);
        int64_t p = self->in_->tell();
        return (p < 0) ? -1 : (long)p;
    }

    AudioInput            *in_ = nullptr;
    AudioSink             *out_ = nullptr;
    OggVorbis_File        *vf_ = nullptr;
    bool                   opened_ = false;
    int16_t               *pcm_ = nullptr;
    int32_t               *pcm32_ = nullptr; /**< 统一 int32 域中间缓冲 */
    espaudiocore_meta_cb_t meta_cb_ = nullptr;
    void                  *meta_user_ = nullptr;
    int64_t                duration_hint_ms_ = -1;
};

/** 魔数：OGG 页以 "OggS" 开头。注意也可能是 Opus 流，需进一步区分。 */
bool probe_vorbis(AudioInput *in)
{
    uint8_t h[40];
    if (!in) {
        return false;
    }
    int got = in->peek(h, sizeof(h), 28);
    if (got < 28 || memcmp(h, "OggS", 4) != 0) {
        return false;
    }
    /* 第一个页面的 payload 起始处若是 0x01"vorbis" 则为 Vorbis；"OpusHead" 则为 Opus。
     * 页头固定 27 字节 + segment table（长度由 h[26] 给定）。 */
    int segs = h[26];
    int payload = 27 + segs;
    if (payload + 7 > got) {
        return true; /* 信息不足时不排除，交给打开阶段判定 */
    }
    if (h[payload] == 0x01 && memcmp(h + payload + 1, "vorbis", 6) == 0) {
        return true;
    }
    /* 不是 vorbis（可能是 opus），让 probe_opus 处理 */
    return false;
}

AudioDecoder *create_vorbis(void)
{
    return new (std::nothrow) DecoderVorbis();
}

#else /* !CONFIG_ESPAUDIOCORE_ENABLE_VORBIS */

bool probe_vorbis(AudioInput *in)
{
    (void)in;
    return false;
}

AudioDecoder *create_vorbis(void)
{
    return nullptr;
}

#endif
