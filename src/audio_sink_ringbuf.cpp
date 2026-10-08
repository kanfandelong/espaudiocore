/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 kanfandelong. All rights reserved.
 */

/**
 * @file    audio_sink_ringbuf.c
 * @brief   ringbuf 输出汇（PCM 帧）。
 *
 * 采样率变化不只通过回调上报给应用层。
 * 写入统一走 xRingbufferSend：它对 BYTEBUF / NOSPLIT / ALLOWSPLIT 三种类型都有效。
 * 不用 xRingbufferSendAcquire 做零拷贝——它仅支持 NOSPLIT/ALLOWSPLIT，
 * 对 BYTEBUF 会直接 configASSERT 失败（实测 ringbuf.c:999）。
 */

#include "audio_port.h"
#include "audio_sink.h"
#include "audio_types.h"

#include <new>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "freertos/task.h"

/** 每次向 ringbuf 请求写入的最大字节数。
 *  必须远小于 ringbuf 容量：BYTEBUF 需要留 1 字节哨兵，单次请求超过
 *  可写容量会永远等不到空间（实测 FLAC 的 32768 字节块撞上 32KB ringbuf
 *  导致解码任务忙等 + 看门狗）。 */
#define RAW_SEND_CHUNK 4096

class AudioSinkRingbuf : public AudioSink {
public:
    AudioSinkRingbuf(RingbufHandle_t rb, uint8_t target_bits) : rb_(rb), target_bits_(target_bits) {}

    ~AudioSinkRingbuf() override
    {
        audio_free(tmp32_);
        tmp32_ = nullptr;
        tmp32_cap_ = 0;
    }

    bool init()
    {
        return rb_ != nullptr;
    }

    void set_stop_flag(const volatile bool *flag)
    {
        stop_flag_ = flag;
    }

    int write(const void *frames, size_t bytes, uint32_t timeout_ms) override
    {
        (void)timeout_ms;
        if (!rb_ || bytes == 0) {
            return AUDIO_OK;
        }

        /* 约定：write() 的输入是 **int16 交错 PCM**（与 AudioSinkI2s::write 一致）。
         * 因此位宽换算的**源位宽恒为 16**，绝不能用 cur_bits——后者是「解码器的
         * 源位宽」（AAC/MP3 是 16，但 FLAC 可能是 24/32），拿它当输入位宽会把
         * 样本数算错。
         *
         * 另外：write_i32() **不**走本函数（它输入 int32 且已自行换算），
         * 否则同一份数据会被换算两次——实测表现就是严重爆音。 */
        const void *p_src = frames;
        size_t p_bytes = bytes;
        if (target_bits_ && target_bits_ != 16) {
            const size_t samples = bytes / sizeof(int16_t);
            const size_t out =
                convert_bits(frames, samples, 16, conv_, sizeof(conv_), target_bits_);
            if (out) {
                p_src   = conv_;
                p_bytes = out;
            }
        }
        return raw_send(p_src, p_bytes);
    }

    /**
     * @brief 直接写入 ringbuf：不做位宽换算、不施加音量，只负责"写进去"。
     *
     * 有界等待 + 可被 stop 唤醒：
     *  - 不能把 `timeout_ms==0` 直接交给 pdMS_TO_TICKS —— 那是 0 tick = 非阻塞，
     *    ringbuf 一满就丢数据（实测 "output ringbuf full, N bytes dropped"，
     *    进而 "decode failed: IO"）。0 的语义应与 I2S 输出一致：无限等待。
     *  - 但也不能用一个超长超时真的阻塞住：那样 stop 无法唤醒。所以每 100ms
     *    醒一次检查停止标志，形成"有界等待循环"。
     *
     * 只用 xRingbufferSend：它对 BYTEBUF / NOSPLIT / ALLOWSPLIT 三种类型都有效。
     * （不用 xRingbufferSendAcquire 零拷贝：它仅支持 NOSPLIT/ALLOWSPLIT，
     *  对 BYTEBUF 会**直接 configASSERT 失败**（ringbuf.c:999）而不是返回
     *  pdFALSE，所以"失败再退化"的写法在这里没有退路。） */
    int raw_send(const void *data, size_t bytes)
    {
        if (!rb_ || bytes == 0) {
            return AUDIO_OK;
        }
        const uint8_t *src = (const uint8_t *)data;
        size_t left = bytes;

        /* **分块写入**：单次请求一旦超过 ringbuf 的可写容量，就永远等不到空间，
         * 因为 BYTEBUF 需要留 1 字节哨兵（实际可写 = 容量-1）。
         *
         * 实测踩中：FLAC 的 block 通常 4096 帧，24-bit 转 int32 后
         * 4096 × 2ch × 4B = 32768 字节，恰好等于 32KB ringbuf 的容量，
         * 于是 xRingbufferSend 永远返回失败 -> 解码任务忙等不让出 CPU
         * -> IDLE1 饿死 -> 触发 task_wdt。AAC(8192B)/WAV(4096B) 块小，所以没事。
         *
         * 按小块循环写入既绕开这个陷阱，也让停止响应更及时。 */
        while (left > 0) {
            const size_t chunk = (left > RAW_SEND_CHUNK) ? RAW_SEND_CHUNK : left;
            while (xRingbufferSend(rb_, src, chunk, pdMS_TO_TICKS(100)) != pdTRUE) {
                if (stop_flag_ && *stop_flag_) {
                    return AUDIO_ERR_NOT_SUPPORTED; /* 被停止打断，放弃剩余数据 */
                }
            }
            src += chunk;
            left -= chunk;
        }
        account_write(bytes, cur_bits);
        return AUDIO_OK;
    }

    bool can_block() const override
    {
        return true;
    }

    /**
     * @brief 音量：与 I2S 输出保持一致的语义。
     *
     * 早期版本在 ringbuf 输出上**忽略**音量，理由是"下游未知，保持 PCM 纯净"。
     * 现按需求改为与 I2S 对齐：真正设置增益，由 write_i32 在 int32 域施加。
     * 若调用者需要未缩放的原始 PCM，把 volume_db 保持 0 即可——增益恰为 1，
     * write_i32 会走免缩放的直通分支，一次乘法都不做。
     */
    void set_volume_db(float db) override
    {
        volume_db_ = db;
        gain_q16_  = db_to_gain_q16(db);
        AUDIO_LOGI("volume %.1f dB (ringbuf output, applied in int32 domain)", (double)db);
    }

    /**
     * @brief ringbuf 输出不做任何硬件重配。
     *
     * 采样率变化通过 espaudiocore_get_format() 报告，由应用自行处理。
     * 这里只更新记账字段，始终返回 OK，表示"不阻止继续解码"。
     */
    /**
     * @brief 采样率只做记录（不碰硬件），且**只在变化时打日志**。
     *
     * 解码器可能逐帧调用本函数（AAC 就是如此）。无条件打日志会刷屏并堵死
     * UART——实测 94 秒的 AAC 刷了 4000+ 行 "sample rate -> ..."，
     * 日志输出本身把解码任务卡住，测试停在第一个文件不再前进。
     * I2S 输出的 set_rate 早就有同样的判断（其注释写着"避免每帧重试刷屏"）。
     */
    int set_rate(uint32_t hz) override
    {
        if (hz == cur_rate) {
            return AUDIO_OK;
        }
        cur_rate = hz;
        AUDIO_LOGI("sample rate -> %u Hz (ringbuf output: application must handle)", (unsigned)hz);
        return AUDIO_OK;
    }

    int set_format(uint8_t bits, uint8_t channels) override
    {
        if (bits == cur_bits && channels == cur_channels) {
            return AUDIO_OK;
        }
        cur_bits     = bits;
        cur_channels = channels;
        return AUDIO_OK;
    }

    /**
     * @brief 统一 int32 路径，语义与 I2S 输出一致（见 D-23）。
     *
     * 收到的 int32 已按 src_bits 对齐（16<<16 / 24<<8 / 32 原值）。
     * 这里在 **int32 域施加音量缩放**，再按目标位宽下采样，最后交给 write()
     * 走 xRingbufferSend 写入 —— 与 AudioSinkI2s::write_i32 的处理方式对齐。
     */
    int write_i32(const int32_t *pcm, size_t pairs, uint8_t src_bits,
                  uint32_t timeout_ms) override
    {
        (void)src_bits;
        (void)timeout_ms;
        if (!pcm || pairs == 0) {
            return AUDIO_OK;
        }
        /* 输出位宽的选取必须与 I2S 输出一致：**优先 target_bits_**（即
         * cfg.output_bits），而不是解码器的源位宽 cur_bits。
         *
         * 早期实现只看 cur_bits，于是 cfg.output_bits=32 被忽略、ringbuf 里
         * 实际是 16-bit 数据；下游若按 32-bit 槽搬运到 I2S，每个 32-bit 槽会
         * 装进两个 16-bit 半样本，整个采样流错位——实测听感就是连续"哒哒"声。 */
        const uint8_t ob = target_bits_ ? target_bits_ : (cur_bits ? cur_bits : 32);
        const size_t nsamp = pairs * 2;
        const bool unity = gain_is_unity();
        const uint32_t g = gain_q16();

        /* 32-bit 输出：缩放后直接写 int32（增益为 1 时省掉整趟缩放遍历）。
         * 注意走 raw_send 而非 write()：数据已是最终位宽，再进 write() 会被
         * 当成 int16 二次换算（实测爆音）。 */
        if (ob >= 32) {
            if (unity) {
                return raw_send(pcm, nsamp * sizeof(int32_t));
            }
            int32_t *dst = ensure32(nsamp);
            if (!dst) {
                return AUDIO_ERR_NO_MEM;
            }
            for (size_t i = 0; i < nsamp; i++) {
                int64_t v = ((int64_t)pcm[i] * (int64_t)g) >> 16;
                if (v > INT32_MAX) {
                    v = INT32_MAX;
                } else if (v < INT32_MIN) {
                    v = INT32_MIN;
                }
                dst[i] = (int32_t)v;
            }
            return raw_send(dst, nsamp * sizeof(int32_t));
        }

        if (ob == 16 || ob == 24) {
            /* 先在 int32 域缩放（若需要），再收窄到目标位宽；同样走 raw_send */
            const int32_t *src = pcm;
            if (!unity) {
                int32_t *dst = ensure32(nsamp);
                if (!dst) {
                    return AUDIO_ERR_NO_MEM;
                }
                for (size_t i = 0; i < nsamp; i++) {
                    int64_t v = ((int64_t)pcm[i] * (int64_t)g) >> 16;
                    if (v > INT32_MAX) {
                        v = INT32_MAX;
                    } else if (v < INT32_MIN) {
                        v = INT32_MIN;
                    }
                    dst[i] = (int32_t)v;
                }
                src = dst;
            }
            size_t nb = convert_bits(src, nsamp, 32, conv_, sizeof(conv_), ob);
            if (nb == 0) {
                return AUDIO_ERR_IO;
            }
            return raw_send(conv_, nb);
        }
        return AUDIO_ERR_NOT_SUPPORTED;
    }

    int flush() override
    {
        return AUDIO_OK;
    }

private:
    /** 取/扩容"缩放后 int32"缓冲（堆分配，按需增长） */
    int32_t *ensure32(size_t nsamp)
    {
        if (!tmp32_ || tmp32_cap_ < nsamp) {
            audio_free(tmp32_);
            tmp32_ = (int32_t *)audio_alloc_big(nsamp * sizeof(int32_t), 0);
            tmp32_cap_ = tmp32_ ? nsamp : 0;
        }
        return tmp32_;
    }

    int32_t               *tmp32_ = nullptr; /**< 音量缩放后的 int32 缓冲（堆） */
    size_t                 tmp32_cap_ = 0;
    RingbufHandle_t        rb_ = nullptr;
    uint8_t                target_bits_ = 0; /**< 0 = 跟随 PCM 位宽 */
    const volatile bool   *stop_flag_ = nullptr;
    /** 位宽转换输出缓冲（convert_bits 内部带容量检查，不会越界） */
    uint8_t                conv_[16384];
};

AudioSink *audio_sink_ringbuf_create(RingbufHandle_t rb, uint8_t target_bits)
{
    AudioSinkRingbuf *s = new (std::nothrow) AudioSinkRingbuf(rb, target_bits);
    if (!s) {
        return nullptr;
    }
    if (!s->init()) {
        delete s;
        return nullptr;
    }
    return s;
}

void audio_sink_ringbuf_bind_stop(AudioSink *sink, const volatile bool *flag)
{
    AudioSinkRingbuf *rb = static_cast<AudioSinkRingbuf *>(sink);
    if (rb) {
        rb->set_stop_flag(flag);
    }
}
