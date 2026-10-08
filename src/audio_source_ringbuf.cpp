/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 kanfandelong. All rights reserved.
 */

/**
 * @file    audio_source_ringbuf.c
 * @brief   ringbuf 数据源：纯顺序流，无 seek。
 */

#include "audio_port.h"
#include "audio_source.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "freertos/task.h"

#include <stdio.h>
#include <new>
#include <stdlib.h>
#include <string.h>

/** 前瞻缓存容量：ID3v2 头 10 字节 + 魔数探测足够；给 4KB 留余量 */
#define RINGBUF_LOOKAHEAD_SIZE 4096

/** 前瞻等待上限（毫秒）：流式数据异步到达，peek 需要给它一点时间。
 *  不能取太大：解码任务会阻塞在这个等待里，而 stop 只能靠超时才能让它醒来，
 *  等待越长 stop 越迟、越容易与资源释放撞车（实测 use-after-free panic）。
 *  300ms 足以覆盖"生产者刚开始喂"的场景。 */
#define LOOKAHEAD_WAIT_MS 300

/** 首次 read 的等待上限（毫秒）：避免把"数据还没到"误判为流结束。
 *  解码器的 read_cb 收到 0 字节通常按 EOF 处理（libFLAC 就是这样）。
 *  同样不宜过长，理由见上。 */
#define READ_WAIT_MS 300

class AudioSourceRingbuf : public AudioInput {
public:
    explicit AudioSourceRingbuf(RingbufHandle_t rb) : rb_(rb) {}

    ~AudioSourceRingbuf() override
    {
        close();
    }

    bool init()
    {
        if (!rb_) {
            return false;
        }
        look_ = (uint8_t *)audio_alloc_big(RINGBUF_LOOKAHEAD_SIZE, 0);
        if (!look_) {
            return false;
        }
        look_cap_ = RINGBUF_LOOKAHEAD_SIZE;
        return true;
    }

    void close()
    {
        if (look_) {
            audio_free(look_);
            look_ = nullptr;
        }
        rb_ = nullptr;
        look_len_ = look_pos_ = 0;
    }

    /* ---------------- AudioInput ---------------- */

    int read(void *dst, size_t want) override
    {
        uint8_t *out = (uint8_t *)dst;
        size_t total = 0;

        /* 1. 先消费前瞻缓存 */
        if (look_pos_ < look_len_) {
            size_t avail = look_len_ - look_pos_;
            size_t take = (want < avail) ? want : avail;
            memcpy(out, look_ + look_pos_, take);
            look_pos_ += take;
            total += take;
            want -= take;
            if (look_pos_ >= look_len_) {
                look_len_ = look_pos_ = 0; /* 缓存耗尽，复位 */
            }
        }

        /* 2. 再从 ringbuf 取。BYTEBUF 下 ReceiveUpTo 最多返回一项，故循环。 */
        while (want > 0) {
            size_t got = 0;
            /* 流式数据是**异步到达**的：第一次读且缓存为空时等一会儿，
             * 否则"还没喂到"会被上层当成 EOF —— libFLAC 的 read_cb 收到 0 就
             * 返回 END_OF_STREAM，实测直接导致 "STREAMINFO missing"。
             * 已经读到部分数据时不再等待，避免拖慢正常的部分读。 */
            TickType_t wait = (total == 0) ? pdMS_TO_TICKS(READ_WAIT_MS) : 0;
            uint8_t *item = (uint8_t *)xRingbufferReceiveUpTo(rb_, &got, wait, want);
            if (!item || got == 0) {
                break; /* 等过之后仍无数据；是否 EOF 由上层语义决定 */
            }
            size_t take = (got < want) ? got : want;
            memcpy(out + total, item, take);
            vRingbufferReturnItem(rb_, item);
            total += take;
            want -= take;
            /* 注意：同一 item 不能多次 ReturnItem，所以剩余部分只能在本次拷完 */
        }

        consumed_ += total;
        return (int)total;
    }

    int peek(void *dst, size_t want, size_t need) override
    {
        /* 语义：**尽量多给**（最多 want），同时至少争取 need。
         *  - want = 调用者缓冲区大小，是返回上限；
         *  - need = 最少需要多少（不够时由调用者自己判断）。
         *
         * 早期实现忽略 want、只保证并返回 need，于是 WAV 请求
         * `peek(hdr, 4096, 12)` 时只拿到 12 字节，却要拿它扫描整个 RIFF 头部，
         * 必然报 "header incomplete (fmt=0 ch=0 ...)"。 */
        size_t target = (want < look_cap_) ? want : look_cap_;
        if (target < need) {
            target = (need < look_cap_) ? need : look_cap_;
        }

        (void)ensure_lookahead(target); /* 尽力而为：等不到就返回现有的量 */

        size_t avail = look_len_ - look_pos_;
        size_t give = (avail < target) ? avail : target;
        if (give) {
            memcpy(dst, look_ + look_pos_, give);
        }
        return (int)give;
    }

    bool can_seek() const override
    {
        return false;
    }

    int seek(int64_t pos, int whence) override
    {
        (void)pos;
        (void)whence;
        return AUDIO_ERR_NOT_SUPPORTED;
    }

    int64_t tell() override
    {
        return consumed_;
    }

    int64_t size() const override
    {
        return -1; /* 流式源长度未知 */
    }

    int wait_data(uint32_t timeout_ms) override
    {
        if (look_pos_ < look_len_) {
            return 1; /* 缓存里还有 */
        }
        size_t got = 0;
        uint8_t *item = (uint8_t *)xRingbufferReceiveUpTo(rb_, &got, pdMS_TO_TICKS(timeout_ms), 1);
        if (!item || got == 0) {
            return 0; /* 超时 */
        }
        /* 拿到 1 字节，塞进前瞻缓存头部 */
        look_len_ = look_pos_ = 0;
        look_[0] = item[0];
        vRingbufferReturnItem(rb_, item);
        look_len_ = 1;
        look_pos_ = 0;
        return 1;
    }

private:
    /**
     * @brief 保证前瞻缓存中至少有 need 字节。
     *
     * 流式源的数据是**异步到达**的，所以这里必须能等一小会儿，而不是只看当前
     * ringbuf 里有什么：
     *   - 超时 0（非阻塞）会让 WAV 的 peek(4096) 头部扫描、FLAC 的 metadata
     *     连续读取在数据尚未喂满时直接失败（实测 WAV 报
     *     "begin failed: ESP_ERR_INVALID_RESPONSE (dec_err=-2)"，
     *     FLAC 报 "process_single failed, state=ABORTED"）；
     *   - 但也不能无限等：给总超时，超时后有多少算多少，由调用者判断够不够。
     */
    bool ensure_lookahead(size_t need)
    {
        /* 先规整缓存：把未消费部分搬到头部 */
        if (look_pos_ > 0) {
            size_t avail = look_len_ - look_pos_;
            if (avail && look_pos_ > 0) {
                memmove(look_, look_ + look_pos_, avail);
            }
            look_len_ = avail;
            look_pos_ = 0;
        }

        TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(LOOKAHEAD_WAIT_MS);
        while (look_len_ < need && look_len_ < look_cap_) {
            size_t got = 0;
            size_t space = look_cap_ - look_len_;
            /* 每次最多等 50ms，便于在总超时到点时退出 */
            uint8_t *item = (uint8_t *)xRingbufferReceiveUpTo(rb_, &got, pdMS_TO_TICKS(50), space);
            if (!item || got == 0) {
                if ((int32_t)(deadline - xTaskGetTickCount()) <= 0) {
                    break; /* 总超时 */
                }
                continue; /* 数据还没到，继续等 */
            }
            memcpy(look_ + look_len_, item, got);
            vRingbufferReturnItem(rb_, item);
            look_len_ += got;
        }
        return look_len_ >= need;
    }

    RingbufHandle_t rb_ = nullptr;
    uint8_t *look_ = nullptr;
    size_t   look_cap_ = 0;
    size_t   look_len_ = 0; /**< 缓存内有效字节 */
    size_t   look_pos_ = 0; /**< 缓存内已消费位置 */
    int64_t  consumed_ = 0;
};

AudioInput *audio_source_ringbuf_create(RingbufHandle_t handle)
{
    AudioSourceRingbuf *s = new (std::nothrow) AudioSourceRingbuf(handle);
    if (!s) {
        return nullptr;
    }
    if (!s->init()) {
        delete s;
        return nullptr;
    }
    return s;
}
