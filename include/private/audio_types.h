/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 kanfandelong. All rights reserved.
 */

/**
 * @file    audio_types.h
 * @brief   内部公共类型 + 数据源/数据汇抽象。
 *
 * 本头文件包含 C++ 类定义，**只能被 .cpp 包含**；纯 C 实现请只包含
 * espaudiocore.h（公共类型）与 audio_port.h（平台层）。
 *
 * 分层约定：
 *   - 公共 C 接口      espaudiocore.h        （C 编译器可用）
 *   - 错误码/内部枚举   include/private/audio_err.h（C 可用）
 *   - 抽象与实现       本文件及 decoder.h    （C++）
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <math.h>

#include "audio_err.h"
#include "espaudiocore.h" /* espaudiocore_meta_t */
#include "esp_err.h"

#ifdef __cplusplus

class AudioInput;
class AudioSink;
class AudioDecoder;

/* ===========================================================================
 * PCM 格式
 * =========================================================================*/

typedef struct
{
    uint32_t rate;    /**< 采样率 Hz */
    uint8_t channels; /**< 声道数 1/2 */
    uint8_t bits;     /**< 位宽 16/24/32 */
} audio_format_t;

/* ===========================================================================
 * 数据源
 * =========================================================================*/

/**
 * @brief 音频数据源抽象。
 *
 * 契约：
 *  - read() 返回实际读到的**字节数**，0 表示流真正结束（不是"暂时没数据"）。
 *  - 允许短读，调用者必须循环。
 *  - peek() 不移动逻辑位置，且保证至少能提供 need 字节供分析
 *    （ID3 解析、魔数探测都要靠它）。ringbuf 源用内部前瞻缓存实现。
 *  - can_seek() == false 时 seek() 必须返回 AUDIO_ERR_NOT_SUPPORTED。
 *    依赖 seek 的能力（M4A 容器、Xing 时长、FLAC seek）在此模式下不可用。
 *  - wait_data() 只对 ringbuf 源有意义；文件源实现为立即返回。
 */
class AudioInput
{
public:
    virtual ~AudioInput() = default;

    const char *path = nullptr; /**< 文件源路径；非文件流保持 nullptr */

    virtual int read(void *dst, size_t len) = 0;
    virtual int peek(void *dst, size_t len, size_t need) = 0;
    virtual bool can_seek() const = 0;
    virtual int seek(int64_t pos, int whence) = 0; /**< whence: SEEK_SET/CUR/END */
    virtual int64_t tell() = 0;
    virtual int64_t size() const = 0; /**< <0 表示未知（流式源） */
    virtual int wait_data(uint32_t timeout_ms) = 0;

    /** 便捷封装：读满 len 字节；返回实际读到的字节数（不足即为 EOF） */
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
 * 数据汇
 * =========================================================================*/

/**
 * @brief 音频输出抽象。
 *
 * 契约（重要）：
 *  - write() 要么把 bytes 全部写出，要么因为 stop 请求而放弃；
 *    **绝不返回"只写了 0 个"**，否则解码器会以为可以继续，导致采样点被静默丢弃。
 *  - 阻塞是正常背压：DMA 满 / ringbuf 满时应当等待，但必须能被停止请求唤醒。
 *
 * 两级入口（见 D-23）：
 *  1. write()       —— 原始字节 + 源位深。老路径，ringbuf 输出仍可用。
 *  2. write_i32()   —— **推荐的统一路径**：解码器把 PCM 对齐成 int32
 *                      （16-bit<<16 / 24-bit<<8 / 32-bit 原值），
 *                      音量缩放与位宽下采样都由 sink 在这之后完成。
 *                      这样"解码 → 缩放 → 输出"三段职责清晰，
 *                      也避免解码器各自实现一套位宽处理而互相不一致。
 */
class AudioSink
{
public:
    virtual ~AudioSink() = default;

    virtual int write(const void *frames, size_t bytes, uint32_t timeout_ms) = 0;

    /**
     * @brief 以 int32 为单位写 PCM（每对采样点 = L,R）。
     *
     * @param pcm      int32 采样点数组，交错，长度 = pairs*2
     * @param pairs    采样点对数（每对含 L/R）
     * @param src_bits 这些 int32 值代表的有效位深（16/24/32）。
     *                 16 -> 值已 <<16；24 -> 值已 <<8；32 -> 原值。
     *                 sink 需要比这更窄的输出时才右移，否则保持精度。
     * @param timeout_ms 0 = 用默认
     *
     * 默认实现：按 src_bits 收窄到目标位深后走 write()。
     * 子类可覆盖以获得零拷贝或更精确的处理。
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

    virtual bool can_block() const = 0;
    virtual int set_rate(uint32_t hz) = 0;
    virtual int set_format(uint8_t bits, uint8_t channels) = 0;
    virtual int flush() = 0;

    /** 当前配置是否与目标格式不一致 */
    bool format_changed(const audio_format_t &fmt) const
    {
        return (fmt.rate != cur_rate) || (fmt.channels != cur_channels) || (fmt.bits != cur_bits);
    }

    uint32_t cur_rate = 0;
    uint8_t cur_channels = 0;
    uint8_t cur_bits = 0;

    /** 设置音量（dB）。实现方自行决定是否以及如何施加。 */
    virtual void set_volume_db(float db)
    {
        volume_db_ = db;
        gain_q16_ = db_to_gain_q16(db);
    }
    float volume_db() const { return volume_db_; }
    uint32_t gain_q16() const { return gain_q16_; }
    bool gain_is_unity() const { return gain_q16_ == 65536; }

    /**
     * @brief 累计已成功写出的采样帧数（每个子类的 write 成功后调用）。
     * @param src_bits 本次写入的样本位宽，用于换算帧数
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

    /** 已写出的总帧数（用于位置查询） */
    uint64_t played_frames() const { return played_frames_; }

    /** 重置帧计数（每次 begin 新会话时调用） */
    void reset_accounting() { played_frames_ = 0; }

protected:
    float volume_db_ = 0.0f;
    uint32_t gain_q16_ = 65536;  /**< Q16 线性增益，65536 = 0 dB */
    uint64_t played_frames_ = 0; /**< 已写出的采样帧数 */

public:
    /**
     * @brief 输出位宽大于源位宽时，把样本左移补齐到目标位宽的高位。
     *
     * @note - 输入 PCM 默认按小端、左对齐有符号样本处理。
     * @note - 仅支持 16/24/32 位 PCM。
     * @note - 输出到目标位宽时，目标样本同样左对齐到其位宽f的高位。
     *
     * @param src        源 PCM（交错，位宽 src_bits）
     * @param samples    样本总数（不是帧数）
     * @param dst        目标缓冲（位宽 dst_bits）
     * @param dst_cap    目标缓冲容量（字节）
     * @return 写入 dst 的字节数
     */
    static size_t convert_bits(const void *src, size_t samples, uint8_t src_bits, void *dst,
                               size_t dst_cap, uint8_t dst_bits)
    {
        if (!src || !dst || samples == 0 || src_bits == 0 || dst_bits == 0)
        {
            return 0;
        }

        /* 只支持 16/24/32 位 PCM */
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
            /* 统一为 int32_t，且左对齐到 32 位 */
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
                    u24 |= 0xFF000000; /* 24 位有符号数扩展为 32 位 */
                }
                v32 = (int32_t)(u24 << 8); /* 左对齐到 32 位 */
            }
            else
            { /* src_bits == 32 */
                uint32_t u32 = (uint32_t)s[0] | ((uint32_t)s[1] << 8) |
                               ((uint32_t)s[2] << 16) | ((uint32_t)s[3] << 24);
                v32 = (int32_t)u32;
            }
            s += src_bytes;

            /* 从 32 位左对齐值转换为目标位宽左对齐值 */
            int32_t out;
            if (dst_bits == 32)
            {
                out = v32;
            }
            else
            {
                /* v32 的高 dst_bits 位就是目标样本，右移掉多余低位 */
                out = v32 >> (32 - dst_bits);
            }

            /* 小端写入目标缓冲 */
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
            { /* dst_bits == 32 */
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
     * @brief 整数音量缩放（dB -> 线性增益）。
     *
     * 每个样本乘以增益并做饱和处理。正增益会削顶到满量程。
     *
     * @param buf     32-bit 样本缓冲（原地修改）
     * @param samples 样本数
     * @param gain    Q16 定点增益（65536 = 0 dB）
     */
    static void apply_gain(int32_t *buf, size_t samples, uint32_t gain)
    {
        if (!buf || gain == 65536)
        {
            return; /* 0 dB：不动数据，省一次遍历 */
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

    /** dB -> Q16 线性增益。<= -60 dB 视为静音（返回 0）。 */
    static uint32_t db_to_gain_q16(float db)
    {
        /*         if (db <= -60.0f) {
                    return 0;
                }
                if (db > 12.0f) {
                    db = 12.0f;
                } */
        float linear = powf(10.0f, db / 20.0f);
        uint32_t g = (uint32_t)(linear * 65536.0f + 0.5f);
        return g;
    }
};
#endif /* __cplusplus */
