/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 kanfandelong. All rights reserved.
 */

/**
 * @file    decoder.h
 * @brief   解码器抽象与路由表。内部使用，不对外暴露。
 *
 * 一个格式 = 一个 AudioDecoder 子类 + 一个路由表条目。
 * 新增格式只需要：写一个子类，然后在 s_decoders[] 里加一行。
 */

#pragma once

#include "audio_types.h"

#ifdef __cplusplus

/** 本头文件含 C++ 类，仅供 .cpp 包含 */

/* ===========================================================================
 * 解码器接口
 * =========================================================================*/

typedef enum {
    AUDIO_DEC_STATE_IDLE = 0,
    AUDIO_DEC_STATE_ACTIVE,
    AUDIO_DEC_STATE_EOS,   /**< 数据正常结束 */
    AUDIO_DEC_STATE_ERROR, /**< 出错，原因存 last_error */
} audio_dec_state_t;

/**
 * @brief 解码器实例（每个播放会话一个）。
 *
 * 生命周期：
 *   open()  -> [decode() / reset() ...] -> close()
 *
 * 契约：
 *  - open() 里若解析失败，**先**调用 meta_cb 把已知的 ID3/标签发出去
 *    （APIC 图片用"存文件+存路径"语义，不要缓存图片到内存），再用
 *    **真实解析到的格式**回填 fmt、用解析到的时长回填 duration_ms，最后返回。
 *  - decode() 返回 AUDIO_EOF 表示数据流结束（不是错误）。
 *  - reset() 用于 seek 之后复位解码状态；源位置由调用者负责定位。
 */
class AudioDecoder {
public:
    AudioDecoder() = default;
    virtual ~AudioDecoder() = default;
    AudioDecoder(const AudioDecoder &) = delete;
    AudioDecoder &operator=(const AudioDecoder &) = delete;

    /**
     * @param in        数据源（所有权不转移，由调用者持有）
     * @param out       数据汇（所有权不转移）
     * @param meta_cb   元数据回调，可为 NULL
     * @param meta_user 回调 user 指针
     * @param[out] fmt  回填真实格式
     * @param[out] duration_ms 回填总时长，<0 表示未知
     * @param[out] dec_err 回填解码器原生错误码（见 AUDIO_DEC_ERR_*）
     */
    virtual audio_err_t open(AudioInput *in, AudioSink *out, espaudiocore_meta_cb_t meta_cb,
                             void *meta_user, audio_format_t *fmt, int64_t *duration_ms,
                             int *dec_err) = 0;

    /**
     * @brief 解出一帧 PCM 写入 sink。
     * @return AUDIO_OK，或 AUDIO_EOF（流结束），或错误码。
     * @note 必须自行保证"要么全写出、要么因 stop 放弃"（见 AudioSink 契约）。
     */
    virtual audio_err_t decode() = 0;

    /** seek 之后复位内部状态（清空解码器缓存、丢弃半帧） */
    virtual void reset() = 0;

    /** 释放资源。可重复调用（幂等）。 */
    virtual void close() = 0;

    /**
     * @brief 取当前格式。
     *
     * 恒定采样率的格式（WAV/FLAC/MP3）实现为返回 open() 时解析到的值；
     * 采样率可能中途变化的流式格式可在此返回最新值。
     */
    virtual const audio_format_t &format() const
    {
        return fmt_;
    }

    virtual audio_dec_state_t state() const { return state_; }
    virtual int last_error() const { return last_error_; }

protected:
    audio_format_t    fmt_ = {};
    audio_dec_state_t state_      = AUDIO_DEC_STATE_IDLE;
    int               last_error_ = AUDIO_DEC_ERR_NONE;
};

/* ===========================================================================
 * 路由表
 * =========================================================================*/

typedef struct {
    const char *ext;  /**< 扩展名，用于优先匹配；可为 NULL */
    const char *name; /**< 人类可读名，用于日志 */

    /**
     * 读头部判魔数。必须通过 peek 实现（不移动位置），对 ringbuf 源亦可用。
     * 可为 NULL（表示该格式无法靠魔数识别，只能靠扩展名）。
     */
    bool (*probe)(AudioInput *in);

    /** 创建一个新的解码器实例；不支持时返回 NULL */
    AudioDecoder *(*create)(void);

    uint32_t task_stack; /**< 该格式推荐栈大小；0 = 用 Kconfig 默认值 */
} audio_decoder_entry_t;

/** 取路由表（以 ext == NULL 结尾）。实现在 audio_decoder_route.cpp。 */
const audio_decoder_entry_t *audio_decoder_table(void);

#endif /* __cplusplus */
