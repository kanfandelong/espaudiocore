/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 kanfandelong. All rights reserved.
 */

/**
 * @file    audio_player.c
 * @brief   播放状态机 + 解码任务。
 *
 * 职责：
 *  - 持有唯一播放会话的全部资源
 *  - 解码器自动路由（扩展名优先 -> 魔数回退）
 *  - 解码任务主循环，以及暂停 / 跳转 / 停止的串行化
 *  - 采样率随内容变化时的内部重配（**仅 I2S 输出**，见 D-9）
 *
 * 线程模型：
 *  - 应用任务调用 begin/stop/pause/resume/seek，只置标志并等待
 *  - 解码任务独占 decoder 与 sink 的写路径；seek/reset 都在它里面做
 *  - 所有阻塞点都会检查 stop_req，因此 stop() 是有界等待，不需要强杀
 */

#include "audio_decoder_route.h"
#include "audio_player.h"
#include "audio_port.h"
#include "audio_sink.h"
#include "audio_source.h"
#include "audio_types.h"
#include "decoder.h"

#include <stdio.h>
#include <new>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#ifndef CONFIG_ESPAUDIOCORE_TASK_STACK_DEFAULT
#define CONFIG_ESPAUDIOCORE_TASK_STACK_DEFAULT 8192
#endif
#ifndef CONFIG_ESPAUDIOCORE_TASK_PRIORITY
#define CONFIG_ESPAUDIOCORE_TASK_PRIORITY 8
#endif

/* ===========================================================================
 * 会话上下文
 * =========================================================================*/

struct PlayerCtx {
    AudioInput   *in = nullptr;
    AudioSink    *sink = nullptr;
    AudioDecoder *dec = nullptr;

    espaudiocore_cfg_t cfg = {};

    int64_t duration_ms = -1;
    bool    i2s_output = false; /**< true = 输出为 I2S（采样率由本库内部处理） */
    int     dec_err = AUDIO_DEC_ERR_NONE;

    TaskHandle_t      task = nullptr;
    SemaphoreHandle_t done_sem = nullptr;

    volatile bool    stop_req = false;
    volatile bool    pause_req = false;
    volatile bool    seek_req = false;
    volatile int64_t seek_target_ms = 0;

    /**
     * @brief 元数据回调包装：拦截 "tlen" 同步时长，再转发给用户回调。
     *
     * 背景：MP3/AAC 这类没有头部时长字段的格式，只能在**解码过程中**按比特率
     * 估算总时长，并通过 meta 回调上报。若不在这里截住，player 的 duration_ms
     * 会一直停在 open() 时的 -1，导致 get_duration_ms() 与 meta 回调给出的
     * 答案不一致（实测 AAC 就是这种情况：meta 报 131630，API 返回 -1）。
     */
    static void meta_trampoline(void *user, const espaudiocore_meta_t *meta)
    {
        PlayerCtx *c = static_cast<PlayerCtx *>(user);
        if (c && meta && meta->type && meta->data &&
            strcmp(meta->type, "tlen") == 0) {
            int64_t v = (int64_t)atoll(meta->data);
            if (v > 0) {
                c->duration_ms = v;
            }
        }
        if (c && c->cfg.on_meta) {
            c->cfg.on_meta(c->cfg.user, meta);
        }
    }
};

static PlayerCtx *s_ctx = nullptr;

/* ===========================================================================
 * 资源生命周期（只在确认解码任务已退出后调用）
 * =========================================================================*/

static void ctx_destroy(PlayerCtx *c)
{
    if (!c) {
        return;
    }
    if (c->dec) {
        c->dec->close();
        delete c->dec;
        c->dec = nullptr;
    }
    if (c->sink) {
        delete c->sink;
        c->sink = nullptr;
    }
    if (c->in) {
        delete c->in;
        c->in = nullptr;
    }
    if (c->done_sem) {
        vSemaphoreDelete(c->done_sem);
        c->done_sem = nullptr;
    }
    if (s_ctx == c) {
        s_ctx = nullptr;
    }
    delete c;
}

/**
 * @brief 请求停止并等待解码任务退出（有界）。
 *
 * 解码任务里每个阻塞点都用短超时 + 检查 stop_req 的模式，
 * 所以正常情况下这是毫秒级返回；这里的 2s 只是最后一道防线。
 */
static void ctx_stop_and_join(PlayerCtx *c)
{
    if (!c) {
        return;
    }
    c->stop_req = true;
    c->pause_req = false;

    if (c->task) {
        if (xSemaphoreTake(c->done_sem, pdMS_TO_TICKS(2000)) != pdTRUE) {
            AUDIO_LOGW("decode task did not exit in time; forcing delete (should not happen)");
            vTaskDelete(c->task);
        } else {
            /* 拿到信号量说明解码任务已跑完主循环、不再访问任何共享资源；
             * 它紧接着会调用 vTaskDelete 自行消失，而 vTaskDelete 不碰我们的资源。
             *
             * 这里**绝不能**用 eTaskGetState(c->task) 去确认它是否已消失：
             * 任务自删除后句柄即失效，实测直接触发
             * "assert failed: eTaskGetState tasks.c:1632 (pxTCB)"。
             * 让出两个 tick 即可——语义上此刻已经安全。 */
            vTaskDelay(2);
        }
        c->task = nullptr;
    }
    ctx_destroy(c);
}

/* ===========================================================================
 * 解码任务
 * =========================================================================*/

static void decode_task(void *arg)
{
    PlayerCtx *c = static_cast<PlayerCtx *>(arg);

    AUDIO_LOGI("decode task started (stack headroom=%u)",
               (unsigned)uxTaskGetStackHighWaterMark(nullptr));

    bool eos = false;
    audio_err_t err = AUDIO_OK;

    while (!c->stop_req) {
        if (c->pause_req) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        if (c->seek_req) {
            c->seek_req = false;
            int64_t target_ms = c->seek_target_ms;

            if (!c->in->can_seek()) {
                AUDIO_LOGW("seek not supported by this source");
            } else {
                /* 用字节率把时间换算成偏移；解码器会在 reset() 后重新同步 */
                const audio_format_t &f = c->dec->format();
                int64_t byte_rate = (int64_t)f.rate * (f.channels ? f.channels : 2) * ((f.bits ? f.bits : 16) / 8);
                if (byte_rate <= 0) {
                    byte_rate = 176400; /* 兜底 44.1k/16bit/stereo */
                }
                int64_t off = (target_ms * byte_rate) / 1000;
                if (c->in->seek(off, SEEK_SET) == AUDIO_OK) {
                    c->dec->reset();
                    if (c->cfg.on_event) {
                        c->cfg.on_event(c->cfg.user, ESPAUDIOCORE_EVT_SEEKED, "seeked");
                    }
                } else {
                    AUDIO_LOGW("seek failed: %lld ms -> %lld bytes", (long long)target_ms, (long long)off);
                }
            }
        }

        audio_err_t r = c->dec->decode();

        /* 采样率/格式变化：I2S 输出时内部重配（D-9）；
         * ringbuf 输出时不碰硬件，只由 get_format() 报告给应用。 */
        const audio_format_t &f = c->dec->format();
        if (c->sink && f.rate && c->sink->format_changed(f)) {
            c->sink->set_rate(f.rate);
            c->sink->set_format(f.bits, f.channels);
        }

        if (r == AUDIO_ERR_EOF) {
            eos = true;
            break;
        }
        if (r != AUDIO_OK) {
            err = r;
            break;
        }
    }

    if (c->stop_req) {
        AUDIO_LOGI("decode loop stopped by request");
    } else if (eos) {
        AUDIO_LOGI("decode finished: end of stream");
        if (c->cfg.on_event) {
            c->cfg.on_event(c->cfg.user, ESPAUDIOCORE_EVT_EOS, "end of stream");
        }
    } else {
        AUDIO_LOGE("decode failed: %s", audio_err_name(err));
        if (c->cfg.on_event) {
            char msg[64];
            snprintf(msg, sizeof(msg), "decode failed: %s", audio_err_name(err));
            c->cfg.on_event(c->cfg.user, ESPAUDIOCORE_EVT_ERROR, msg);
        }
    }

    /* 统一补一个 STOPPED，便于应用只用一个分支做收尾 */
    if (c->cfg.on_event) {
        c->cfg.on_event(c->cfg.user, ESPAUDIOCORE_EVT_STOPPED, "stopped");
    }

    AUDIO_LOGI("decode task exiting (stack headroom=%u)",
               (unsigned)uxTaskGetStackHighWaterMark(nullptr));

    /* 注意：这里**不要**去清 c->task。ctx_stop_and_join() 是"先判断 c->task、
     * 再等 done_sem"，如果解码任务抢先把 c->task 置空，停等方就可能跳过等待，
     * 直接 ctx_destroy() 释放 in_（ringbuf 源），而本任务这一刻可能仍在
     * read() 里 -> use-after-free（实测 panic 落在 vRingbufferReturnItem，
     * A2=0x5a5a5a5a）。c->task 的生命周期完全交给 ctx_stop_and_join 管理。 */
    xSemaphoreGive(c->done_sem);
    vTaskDelete(nullptr);
}

/* ===========================================================================
 * 起播公共实现
 * =========================================================================*/

static esp_err_t player_begin(AudioInput *in, const audio_decoder_entry_t *entry, i2s_chan_handle_t tx,
                              void *out_rb, const espaudiocore_cfg_t *cfg, int *dec_err, bool i2s_output)
{
    if (!in) {
        return ESP_ERR_INVALID_ARG;
    }
    if (dec_err) {
        *dec_err = AUDIO_DEC_ERR_NONE;
    }
    if (!entry || !entry->create) {
        delete in;
        if (dec_err) {
            *dec_err = AUDIO_DEC_ERR_OPEN;
        }
        return ESP_ERR_NOT_SUPPORTED;
    }

    /* 一个进程只允许一个播放会话；重复 begin 先收尾旧的 */
    if (s_ctx) {
        ctx_stop_and_join(s_ctx);
    }

    PlayerCtx *c = new (std::nothrow) PlayerCtx();
    if (!c) {
        delete in;
        return ESP_ERR_NO_MEM;
    }
    if (cfg) {
        c->cfg = *cfg;
    }
    c->in = in;
    c->i2s_output = i2s_output;

    c->done_sem = xSemaphoreCreateBinary();
    if (!c->done_sem) {
        ctx_destroy(c);
        return ESP_ERR_NO_MEM;
    }

    c->dec = entry->create();
    if (!c->dec) {
        ctx_destroy(c);
        if (dec_err) {
            *dec_err = AUDIO_DEC_ERR_OPEN;
        }
        return ESP_ERR_NOT_SUPPORTED;
    }

    /* 输出汇：output_bits 由 cfg 指定（0 = 跟随 PCM 位宽） */
    if (i2s_output) {
        c->sink = audio_sink_i2s_create(tx, c->cfg.io_timeout_ms, c->cfg.output_bits);
    } else {
        c->sink = audio_sink_ringbuf_create(reinterpret_cast<RingbufHandle_t>(out_rb),
                                            c->cfg.output_bits);
    }
    if (!c->sink) {
        ctx_destroy(c);
        return ESP_ERR_INVALID_STATE;
    }
    /* 初始音量 */
    c->sink->set_volume_db(c->cfg.volume_db);

    /* 打开解码器：解析头部、上报元数据、回填格式与时长。
     * 注意传的是**包装回调**，不是用户回调本身——这样解码过程中估算出的
     * 时长（tlen）能同步回 player，保证 get_duration_ms() 与 meta 回调一致。 */
    audio_format_t fmt = {};
    audio_err_t r = c->dec->open(c->in, c->sink, PlayerCtx::meta_trampoline, c, &fmt,
                                 &c->duration_ms, &c->dec_err);
    if (r != AUDIO_OK) {
        esp_err_t e = audio_err_to_esp(r);
        if (dec_err) {
            *dec_err = c->dec_err;
        }
        ctx_destroy(c);
        return e;
    }
    c->duration_ms = c->dec->format().rate ? c->duration_ms : c->duration_ms;
    if (c->duration_ms == 0) {
        c->duration_ms = -1;
    }

    /* 首次配置输出格式（此时才拿到真实采样率） */
    c->sink->set_rate(c->dec->format().rate);
    c->sink->set_format(c->dec->format().bits, c->dec->format().channels);

    /* 绑定停止标志：让 sink 的阻塞等待可被 stop 唤醒（D-14） */
    if (i2s_output) {
        audio_sink_i2s_bind_stop(c->sink, &c->stop_req);
    } else {
        audio_sink_ringbuf_bind_stop(c->sink, &c->stop_req);
    }

    uint32_t stack = c->cfg.task_stack ? c->cfg.task_stack : entry->task_stack;
    if (!stack) {
        stack = CONFIG_ESPAUDIOCORE_TASK_STACK_DEFAULT;
    }
    int prio = c->cfg.task_priority ? c->cfg.task_priority : CONFIG_ESPAUDIOCORE_TASK_PRIORITY;
    int core = c->cfg.task_core;

    s_ctx = c; /* 先发布，on_event(STARTED) 里可能回调查询状态 */

    BaseType_t ok = xTaskCreatePinnedToCore(decode_task, "espaudio_dec", stack, c, prio, &c->task, core);
    if (ok != pdPASS) {
        AUDIO_LOGE("failed to create decode task (%u bytes stack, prio %d, core %d)", (unsigned)stack,
                   prio, core);
        ctx_destroy(c);
        return ESP_ERR_NO_MEM;
    }

    const audio_format_t &f = c->dec->format();
    AUDIO_LOGI("playing %s: %u Hz, %u ch, %u-bit, duration=%lld ms, task stack=%u, prio=%d",
               entry->name, (unsigned)f.rate, (unsigned)f.channels, (unsigned)f.bits,
               (long long)c->duration_ms, (unsigned)stack, prio);

    if (c->cfg.on_event) {
        c->cfg.on_event(c->cfg.user, ESPAUDIOCORE_EVT_STARTED, "started");
    }
    return ESP_OK;
}

/* ===========================================================================
 * 内部接口实现
 * =========================================================================*/

esp_err_t audio_player_start_file(const char *path, i2s_chan_handle_t tx, void *out_rb,
                                  const espaudiocore_cfg_t *cfg, int *dec_err, bool i2s_output)
{
    if (dec_err) {
        *dec_err = AUDIO_DEC_ERR_NONE;
    }
    if (!path || path[0] != '/') {
        AUDIO_LOGE("path must be absolute (include the mount point): %s", path ? path : "(null)");
        return ESP_ERR_INVALID_ARG;
    }

    AudioInput *in = audio_source_fs_create(path);
    if (!in) {
        return ESP_ERR_NOT_FOUND;
    }

    const audio_decoder_entry_t *entry = audio_decoder_route(in, path);
    return player_begin(in, entry, tx, out_rb, cfg, dec_err, i2s_output);
}

esp_err_t audio_player_start_stream(RingbufHandle_t in_rb, espaudiocore_format_t fmt, i2s_chan_handle_t tx,
                                    void *out_rb, const espaudiocore_cfg_t *cfg, int *dec_err,
                                    bool i2s_output)
{
    if (dec_err) {
        *dec_err = AUDIO_DEC_ERR_NONE;
    }
    if (!in_rb) {
        return ESP_ERR_INVALID_ARG;
    }

    AudioInput *in = audio_source_ringbuf_create(in_rb);
    if (!in) {
        return ESP_ERR_NO_MEM;
    }

    /* ringbuf 源无法回退，所以不做探测：直接按调用者给的格式查表 */
    const audio_decoder_entry_t *entry = audio_decoder_for_format(fmt);
    if (!entry) {
        delete in;
        if (dec_err) {
            *dec_err = AUDIO_DEC_ERR_OPEN;
        }
        return ESP_ERR_NOT_SUPPORTED;
    }

    return player_begin(in, entry, tx, out_rb, cfg, dec_err, i2s_output);
}

esp_err_t audio_player_stop()
{
    if (!s_ctx) {
        return ESP_ERR_INVALID_STATE;
    }
    ctx_stop_and_join(s_ctx);
    return ESP_OK;
}

esp_err_t audio_player_pause()
{
    if (!s_ctx) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_ctx->pause_req) {
        return ESP_OK;
    }
    s_ctx->pause_req = true;
    if (s_ctx->cfg.on_event) {
        s_ctx->cfg.on_event(s_ctx->cfg.user, ESPAUDIOCORE_EVT_PAUSED, "paused");
    }
    return ESP_OK;
}

esp_err_t audio_player_resume()
{
    if (!s_ctx) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_ctx->pause_req) {
        return ESP_OK;
    }
    s_ctx->pause_req = false;
    if (s_ctx->cfg.on_event) {
        s_ctx->cfg.on_event(s_ctx->cfg.user, ESPAUDIOCORE_EVT_RESUMED, "resumed");
    }
    return ESP_OK;
}

esp_err_t audio_player_seek_ms(int64_t ms)
{
    if (!s_ctx) {
        return ESP_ERR_INVALID_STATE;
    }
    if (ms < 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_ctx->in->can_seek()) {
        return ESP_ERR_NOT_SUPPORTED; /* ringbuf 源：无 seek */
    }

    /* 只登记请求，真正执行在解码任务里串行完成 */
    s_ctx->seek_target_ms = ms;
    s_ctx->seek_req = true;

    /* 有界等待请求被消费，保证返回时位置已生效 */
    for (int i = 0; i < 200 && s_ctx->seek_req; i++) {
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    return ESP_OK;
}

int64_t audio_player_get_position_ms()
{
    if (!s_ctx || !s_ctx->sink) {
        return 0;
    }
    AudioSink *sink = s_ctx->sink;
    uint32_t rate = sink->cur_rate;
    if (!rate) {
        return 0;
    }

    /* 已写出的帧数，减去仍在 I2S DMA 队列里没播出的部分（见 D-10） */
    uint64_t played = sink->played_frames();
    uint8_t bytes_per_sample = sink->cur_bits ? (sink->cur_bits / 8) : 2;
    uint8_t channels = sink->cur_channels ? sink->cur_channels : 2;
    size_t frame_bytes = (size_t)bytes_per_sample * channels;
    if (frame_bytes) {
        size_t pending = audio_sink_pending_bytes(sink);
        uint64_t pending_frames = pending / frame_bytes;
        if (pending_frames < played) {
            played -= pending_frames;
        } else {
            played = 0;
        }
    }
    return (int64_t)((played * 1000ull) / rate);
}

esp_err_t audio_player_set_volume_db(float db)
{
    if (!s_ctx || !s_ctx->sink) {
        return ESP_ERR_INVALID_STATE;
    }
    s_ctx->sink->set_volume_db(db);
    return ESP_OK;
}

float audio_player_get_volume_db()
{
    return (s_ctx && s_ctx->sink) ? s_ctx->sink->volume_db() : 0.0f;
}

int64_t audio_player_get_duration_ms()
{
    return s_ctx ? s_ctx->duration_ms : -1;
}

esp_err_t audio_player_get_format(uint32_t *rate, uint8_t *channels, uint8_t *bits)
{
    if (!s_ctx || !s_ctx->dec) {
        return ESP_ERR_INVALID_STATE;
    }
    const audio_format_t &f = s_ctx->dec->format();
    if (rate) {
        *rate = f.rate;
    }
    if (channels) {
        *channels = f.channels;
    }
    if (bits) {
        *bits = f.bits;
    }
    return ESP_OK;
}

bool audio_player_is_running()
{
    return s_ctx && s_ctx->task && !s_ctx->stop_req;
}

bool audio_player_is_i2s_output()
{
    return s_ctx && s_ctx->i2s_output;
}

int audio_player_last_decoder_error()
{
    return s_ctx ? s_ctx->dec_err : AUDIO_DEC_ERR_NONE;
}

AudioSink *audio_player_sink()
{
    return s_ctx ? s_ctx->sink : nullptr;
}
