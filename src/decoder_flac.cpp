/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 kanfandelong. All rights reserved.
 */

/**
 * @file    decoder_flac.cpp
 * @brief   FLAC 解码器（libflac 流式回调接口）。
 *
 * 元数据：VORBIS_COMMENT 走文本回调；PICTURE 走二进制回调（data 指向
 * libflac 的 metadata 对象，仅在回调期间有效，应用需在回调期间处理好图片，例如直接解码至屏幕或copy一份）。
 */

#include "audio_port.h"
#include "audio_types.h"
#include "decoder.h"

#include <new>
#include <stdlib.h>
#include <string.h>

extern "C"
{
#include "FLAC/stream_decoder.h"
}

#if defined(CONFIG_ESPAUDIOCORE_ENABLE_FLAC)

/** 下混到立体声时支持的最大源声道数 */
#define FLAC_MAX_CH 8

class DecoderFlac : public AudioDecoder
{
public:
    ~DecoderFlac() override
    {
        close();
    }

    audio_err_t open(AudioInput *in, AudioSink *out, espaudiocore_meta_cb_t meta_cb, void *meta_user,
                     audio_format_t *fmt, int64_t *duration_ms, int *dec_err) override
    {
        in_ = in;
        out_ = out;
        meta_cb_ = meta_cb;
        meta_user_ = meta_user;
        if (dec_err)
        {
            *dec_err = AUDIO_DEC_ERR_NONE;
        }
        if (duration_ms)
        {
            *duration_ms = -1;
        }
        if (!in_ || !out_)
        {
            return AUDIO_ERR_INVALID_ARG;
        }

        dec_ = FLAC__stream_decoder_new();
        if (!dec_)
        {
            if (dec_err)
            {
                *dec_err = AUDIO_DEC_ERR_OPEN;
            }
            return AUDIO_ERR_NO_MEM;
        }
        (void)FLAC__stream_decoder_set_md5_checking(dec_, false);
        // Request only the metadata types we need (stream info and Vorbis comments).
        // Skipping picture metadata avoids large memory allocations on constrained devices.
        // FLAC__stream_decoder_set_metadata_respond(dec_, FLAC__METADATA_TYPE_STREAMINFO);
        // FLAC__stream_decoder_set_metadata_respond(dec_, FLAC__METADATA_TYPE_SEEKTABLE);
        // FLAC__stream_decoder_set_metadata_respond(dec_, FLAC__METADATA_TYPE_VORBIS_COMMENT);
        FLAC__stream_decoder_set_metadata_respond_all(dec_);

        FLAC__StreamDecoderInitStatus st = FLAC__stream_decoder_init_stream(
            dec_, read_cb, seek_cb, tell_cb, length_cb, eof_cb, write_cb, metadata_cb, error_cb, this);
        if (st != FLAC__STREAM_DECODER_INIT_STATUS_OK)
        {
            AUDIO_LOGE("FLAC init_stream failed: %s", FLAC__StreamDecoderInitStatusString[st]);
            if (dec_err)
            {
                *dec_err = AUDIO_DEC_ERR_OPEN;
            }
            close();
            return AUDIO_ERR_PARSE;
        }

        /* 解析元数据：STREAMINFO / VORBIS_COMMENT / PICTURE 都在回调里到达 */
        if (!FLAC__stream_decoder_process_until_end_of_metadata(dec_))
        {
            AUDIO_LOGE("FLAC metadata parse failed, state=%s",
                       FLAC__StreamDecoderStateString[FLAC__stream_decoder_get_state(dec_)]);
            if (dec_err)
            {
                *dec_err = AUDIO_DEC_ERR_PARSE;
            }
            close();
            return AUDIO_ERR_PARSE;
        }

        if (fmt_.rate == 0 || fmt_.channels == 0)
        {
            AUDIO_LOGE("FLAC STREAMINFO missing");
            if (dec_err)
            {
                *dec_err = AUDIO_DEC_ERR_PARSE;
            }
            close();
            return AUDIO_ERR_PARSE;
        }

        /* **按源位深声明**，绝不下采样成 16-bit：
         *   16-bit 源 -> fmt.bits = 16（sink 左移 16 位）
         *   24-bit 源 -> fmt.bits = 24（sink 左移 8 位，精度全保留）
         *   32-bit 源 -> fmt.bits = 32
         * 只有目标位宽比源更窄时，sink 才会右移截断（那是调用者的明确选择）。 */
        if (src_bits_ == 0)
        {
            src_bits_ = 16;
        }
        fmt_.bits = (src_bits_ <= 16) ? 16 : (uint8_t)src_bits_;

        if (fmt)
        {
            *fmt = fmt_;
        }
        if (out_)
        {
            out_->set_rate(fmt_.rate);
            out_->set_format(fmt_.bits, fmt_.channels);
        }
        if (duration_ms)
        {
            *duration_ms = duration_hint_ms_;
        }

        state_ = AUDIO_DEC_STATE_ACTIVE;
        AUDIO_LOGI("FLAC: %u Hz, %u ch, src %u-bit -> reported %u-bit%s", (unsigned)fmt_.rate,
                   (unsigned)fmt_.channels, (unsigned)src_bits_, (unsigned)fmt_.bits,
                   (fmt_.bits > 16) ? " (high-res preserved)" : "");
        return AUDIO_OK;
    }

    audio_err_t decode() override
    {
        if (state_ != AUDIO_DEC_STATE_ACTIVE)
        {
            return AUDIO_ERR_EOF;
        }

        FLAC__StreamDecoderState st = FLAC__stream_decoder_get_state(dec_);
        if (st == FLAC__STREAM_DECODER_END_OF_STREAM)
        {
            state_ = AUDIO_DEC_STATE_EOS;
            return AUDIO_ERR_EOF;
        }
        if (st == FLAC__STREAM_DECODER_ABORTED)
        {
            state_ = AUDIO_DEC_STATE_ERROR;
            return AUDIO_ERR_IO;
        }

        /* 处理一帧；PCM 在 write_cb 里直接交给 sink */
        if (!FLAC__stream_decoder_process_single(dec_))
        {
            st = FLAC__stream_decoder_get_state(dec_);
            if (st == FLAC__STREAM_DECODER_END_OF_STREAM)
            {
                state_ = AUDIO_DEC_STATE_EOS;
                return AUDIO_ERR_EOF;
            }
            AUDIO_LOGE("FLAC process_single failed, state=%s", FLAC__StreamDecoderStateString[st]);
            state_ = AUDIO_DEC_STATE_ERROR;
            last_error_ = AUDIO_DEC_ERR_PARSE;
            return AUDIO_ERR_DECODE;
        }

        st = FLAC__stream_decoder_get_state(dec_);
        if (st == FLAC__STREAM_DECODER_END_OF_STREAM)
        {
            state_ = AUDIO_DEC_STATE_EOS;
            return AUDIO_ERR_EOF;
        }
        if (aborted_)
        {
            state_ = AUDIO_DEC_STATE_EOS; /* 被停止打断 */
            return AUDIO_ERR_EOF;
        }
        return AUDIO_OK;
    }

    void reset() override
    {
        /* seek 由调用者完成；这里复位解码器内部状态（保留已解析的元数据） */
        if (dec_)
        {
            FLAC__stream_decoder_reset(dec_);
        }
        aborted_ = false;
        state_ = AUDIO_DEC_STATE_ACTIVE;
    }

    void close() override
    {
        if (dec_)
        {
            FLAC__stream_decoder_finish(dec_);
            FLAC__stream_decoder_delete(dec_);
            dec_ = nullptr;
        }
        audio_free(pcm_);
        pcm_ = nullptr;
        audio_free(pcm32_);
        pcm32_ = nullptr;
        audio_free(pcm16_);
        pcm16_ = nullptr;
        pcm_cap_ = 0;
        i16_cap_ = 0;
        state_ = AUDIO_DEC_STATE_IDLE;
    }

private:
    /* ---------------- libflac 回调 ---------------- */

    static FLAC__StreamDecoderReadStatus read_cb(const FLAC__StreamDecoder *d, FLAC__byte buf[],
                                                 size_t *bytes, void *ctx)
    {
        (void)d;
        DecoderFlac *self = static_cast<DecoderFlac *>(ctx);
        if (*bytes == 0)
        {
            return FLAC__STREAM_DECODER_READ_STATUS_ABORT;
        }
        int n = self->in_->read(buf, *bytes);
        if (n > 0)
        {
            *bytes = (size_t)n;
            return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
        }
        /* 关键：拿不到数据必须置 *bytes = 0 且返回 END_OF_STREAM。
         * 原实现保留了请求量却返回 CONTINUE，libFLAC 会立即回调 -> 忙等/死锁。 */
        *bytes = 0;
        return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
    }

    static FLAC__StreamDecoderSeekStatus seek_cb(const FLAC__StreamDecoder *d,
                                                 FLAC__uint64 absolute_byte_offset, void *ctx)
    {
        (void)d;
        DecoderFlac *self = static_cast<DecoderFlac *>(ctx);
        if (!self->in_->can_seek())
        {
            return FLAC__STREAM_DECODER_SEEK_STATUS_UNSUPPORTED;
        }
        if (self->in_->seek((int64_t)absolute_byte_offset, SEEK_SET) != AUDIO_OK)
        {
            return FLAC__STREAM_DECODER_SEEK_STATUS_ERROR;
        }
        return FLAC__STREAM_DECODER_SEEK_STATUS_OK;
    }

    static FLAC__StreamDecoderTellStatus tell_cb(const FLAC__StreamDecoder *d,
                                                 FLAC__uint64 *absolute_byte_offset, void *ctx)
    {
        (void)d;
        DecoderFlac *self = static_cast<DecoderFlac *>(ctx);
        int64_t pos = self->in_->tell();
        if (pos < 0)
        {
            return FLAC__STREAM_DECODER_TELL_STATUS_ERROR;
        }
        *absolute_byte_offset = (FLAC__uint64)pos;
        return FLAC__STREAM_DECODER_TELL_STATUS_OK;
    }

    static FLAC__StreamDecoderLengthStatus length_cb(const FLAC__StreamDecoder *d,
                                                     FLAC__uint64 *stream_length, void *ctx)
    {
        (void)d;
        DecoderFlac *self = static_cast<DecoderFlac *>(ctx);
        int64_t sz = self->in_->size();
        if (sz < 0)
        {
            return FLAC__STREAM_DECODER_LENGTH_STATUS_UNSUPPORTED;
        }
        *stream_length = (FLAC__uint64)sz;
        return FLAC__STREAM_DECODER_LENGTH_STATUS_OK;
    }

    static FLAC__bool eof_cb(const FLAC__StreamDecoder *d, void *ctx)
    {
        (void)d;
        DecoderFlac *self = static_cast<DecoderFlac *>(ctx);
        int64_t sz = self->in_->size();
        if (sz < 0)
        {
            return false; /* 长度未知：由 read_cb 判定 */
        }
        return self->in_->tell() >= sz;
    }

    /**
     * @brief 一帧解码完成：统一走 **int32 域**，绝不丢精度。
     *
     * 对齐旧库 AudioGeneratorFLAC 的做法：按源位深左移到 int32 高位
     * （8->24, 16->16, 24->8, 32->0），再交给 sink 在 int32 域做缩放与输出。
     * 这样 24-bit 无损 FLAC 的精度完整保留 —— 是否需要收窄由**调用者**
     * 通过 output_bits 明确决定，解码器自己不擅自下采样。
     */
    static FLAC__StreamDecoderWriteStatus write_cb(const FLAC__StreamDecoder *d,
                                                   const FLAC__Frame *frame,
                                                   const FLAC__int32 *const buffer[], void *ctx)
    {
        (void)d;
        DecoderFlac *self = static_cast<DecoderFlac *>(ctx);
        const unsigned blocksize = frame->header.blocksize;
        const unsigned channels = frame->header.channels;
        if (channels == 0 || blocksize == 0 || channels > FLAC_MAX_CH)
        {
            return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
        }

        const size_t pairs = blocksize;
        const size_t needed = pairs * 2; /* 交错采样点数 */
        if (self->pcm_cap_ < needed)
        {
            audio_free(self->pcm32_);
            self->pcm32_ = (int32_t *)audio_alloc_big(needed * sizeof(int32_t), 0);
            self->pcm_cap_ = self->pcm32_ ? needed : 0;
            if (!self->pcm32_)
            {
                return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
            }
        }

        /* 左移到 int32 高位：<=8->24, <=16->16, <=24->8, else 0 */
        int sh;
        if (self->src_bits_ <= 8)
        {
            sh = 24;
        }
        else if (self->src_bits_ <= 16)
        {
            sh = 16;
        }
        else if (self->src_bits_ <= 24)
        {
            sh = 8;
        }
        else
        {
            sh = 0;
        }

        int32_t *dst = self->pcm32_;
        if (channels == 1)
        {
            for (unsigned i = 0; i < blocksize; i++)
            {
                int32_t v = buffer[0][i] << sh;
                dst[i * 2 + 0] = v;
                dst[i * 2 + 1] = v;
            }
        }
        else
        {
            for (unsigned i = 0; i < blocksize; i++)
            {
                dst[i * 2 + 0] = buffer[0][i] << sh;
                dst[i * 2 + 1] = buffer[1][i] << sh;
            }
        }

        /* 统一 int32 路径：位深按**有效位深**上报，sink 决定是否收窄 */
        int rc = self->out_->write_i32(self->pcm32_, pairs, self->src_bits_ > 16 ? self->src_bits_ : 16, 0);
        if (rc == AUDIO_ERR_NOT_SUPPORTED)
        {
            /* sink 不支持统一路径：退回 16-bit 字节接口。
             * 注意必须用**独立**的 int16 缓冲——早期版本把 int32 缓冲强转成
             * int16* 来写，在 24-bit 源上会越过分配范围（needed*4 字节），
             * 属于越界写。 */
            const int sh_back = (self->src_bits_ > 16) ? (self->src_bits_ - 16) : 0;
            if (self->i16_cap_ < needed)
            {
                audio_free(self->pcm16_);
                self->pcm16_ = (int16_t *)audio_alloc_big(needed * sizeof(int16_t), 0);
                self->i16_cap_ = self->pcm16_ ? needed : 0;
            }
            if (!self->pcm16_)
            {
                return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
            }
            for (size_t i = 0; i < needed; i++)
            {
                self->pcm16_[i] = (int16_t)(sh_back ? (dst[i] >> sh_back) : dst[i]);
            }
            rc = self->out_->write(self->pcm16_, needed * sizeof(int16_t), 0);
        }
        if (rc != AUDIO_OK)
        {
            self->aborted_ = true;
            return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
        }
        return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
    }

    /** 元数据：STREAMINFO 取格式/时长；VORBIS_COMMENT 与 PICTURE 上报 */
    static void metadata_cb(const FLAC__StreamDecoder *d, const FLAC__StreamMetadata *meta,
                            void *ctx)
    {
        (void)d;
        DecoderFlac *self = static_cast<DecoderFlac *>(ctx);

        /* 诊断：列出实际出现的元数据块。用于区分两种情况——
         *   (a) 文件确实没有 VORBIS_COMMENT/PICTURE（此处只会看到 type=0）
         *   (b) 有块但回调逻辑漏了（此处会看到 type=4/6 却没上报文本） */
        AUDIO_LOGI("FLAC metadata block: type=%d, last=%d", (int)meta->type,
                   (int)meta->is_last);

        switch (meta->type)
        {
        case FLAC__METADATA_TYPE_STREAMINFO:
        {
            const FLAC__StreamMetadata_StreamInfo &si = meta->data.stream_info;
            self->fmt_.rate = si.sample_rate;
            self->fmt_.channels = (uint8_t)si.channels;
            self->src_bits_ = (uint8_t)si.bits_per_sample;
            if (si.sample_rate && si.total_samples)
            {
                self->duration_hint_ms_ =
                    (int64_t)((uint64_t)si.total_samples * 1000ull / si.sample_rate);
            }
            break;
        }
        case FLAC__METADATA_TYPE_VORBIS_COMMENT:
        {
            if (!self->meta_cb_)
            {
                break;
            }
            const FLAC__StreamMetadata_VorbisComment &vc = meta->data.vorbis_comment;
            AUDIO_LOGI("FLAC Vorbis comment: %u entries, vendor=\"%s\"", (unsigned)vc.num_comments,
                       (vc.vendor_string.entry && vc.vendor_string.length)
                           ? (const char *)vc.vendor_string.entry
                           : "");
            for (uint32_t i = 0; i < vc.num_comments; i++)
            {
                const FLAC__byte *e = vc.comments[i].entry;
                uint32_t len = vc.comments[i].length;
                const FLAC__byte *eq = (const FLAC__byte *)memchr(e, '=', len);
                if (!eq)
                {
                    continue;
                }
                char key[32];
                size_t klen = (size_t)(eq - e);
                if (klen >= sizeof(key))
                {
                    klen = sizeof(key) - 1;
                }
                memcpy(key, e, klen);
                key[klen] = '\0';
                espaudiocore_meta_t m = {};
                m.type = key;
                m.is_binary = false;
                m.data = (const char *)(eq + 1);
                m.len = (size_t)(len - klen - 1);
                m.pic_type = -1;
                self->meta_cb_(self->meta_user_, &m);
            }
            break;
        }
        case FLAC__METADATA_TYPE_PICTURE:
        {
            if (!self->meta_cb_)
            {
                break;
            }
            const FLAC__StreamMetadata_Picture &pic = meta->data.picture;
            espaudiocore_meta_t m = {};
            m.type = "PICTURE";
            m.is_binary = true;
            m.data = (const char *)pic.data;
            m.len = pic.data_length;
            m.mime = pic.mime_type;
            m.pic_type = (int)pic.type;
            self->meta_cb_(self->meta_user_, &m);
            AUDIO_LOGD("FLAC picture: %s, %ux%u, %u bytes", pic.mime_type ? pic.mime_type : "?",
                       (unsigned)pic.width, (unsigned)pic.height, (unsigned)pic.data_length);
            break;
        }
        default:
            break;
        }
    }

    static void error_cb(const FLAC__StreamDecoder *d, FLAC__StreamDecoderErrorStatus status,
                         void *ctx)
    {
        (void)d;
        (void)ctx;
        AUDIO_LOGW("FLAC error: %s", FLAC__StreamDecoderErrorStatusString[status]);
    }

    AudioInput *in_ = nullptr;
    AudioSink *out_ = nullptr;
    FLAC__StreamDecoder *dec_ = nullptr;
    int16_t *pcm_ = nullptr;
    int32_t *pcm32_ = nullptr; /**< 统一 int32 域缓冲（采样点数） */
    int16_t *pcm16_ = nullptr; /**< 兜底 16-bit 输出缓冲（独立分配） */
    size_t pcm_cap_ = 0;
    size_t i16_cap_ = 0;
    espaudiocore_meta_cb_t meta_cb_ = nullptr;
    void *meta_user_ = nullptr;

    uint8_t src_bits_ = 16;
    bool aborted_ = false;
    int64_t duration_hint_ms_ = -1;
};

/** 魔数：FLAC 流以 "fLaC" 开头 */
bool probe_flac(AudioInput *in)
{
    uint8_t h[4];
    if (!in || in->peek(h, sizeof(h), 4) < 4)
    {
        return false;
    }
    return memcmp(h, "fLaC", 4) == 0;
}

AudioDecoder *create_flac(void)
{
    return new (std::nothrow) DecoderFlac();
}

#else /* !CONFIG_ESPAUDIOCORE_ENABLE_FLAC */

bool probe_flac(AudioInput *in)
{
    (void)in;
    return false;
}

AudioDecoder *create_flac(void)
{
    return nullptr;
}

#endif
