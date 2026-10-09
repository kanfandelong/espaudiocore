/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 kanfandelong. All rights reserved.
 */

/**
 * @file    espaudiocore.h
 * @brief   多解码器统一封装接口库。
 *
 * 用法（一次调用起播，解码在内部任务里自治）：
 *
 * @code{c}
 *   i2s_chan_handle_t tx;
 *   // ... 由应用创建 I2S 通道并 init_std_mode + enable ...
 *
 *   espaudiocore_cfg_t cfg = {
 *       .on_event = my_event_cb,
 *       .on_meta  = my_meta_cb,
 *       .user     = my_ctx,
 *   };
 *   int dec_err = 0;
 *   ESP_ERROR_CHECK(espaudiocore_begin("/sdcard/a.mp3", tx, &cfg, &dec_err));
 *   // 播放结束会通过 ESPAUDIOCORE_EVT_EOS 通知
 * @endcode
 *
 * @author  kanfandelong <kanfandelong@outlook.com>
 * @date    2026-10-08
 * @version 1.0.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 条件编译：让句柄类型在裁剪了对应功能时也能通过编译 */
#include "sdkconfig.h"
#if defined(CONFIG_ESPAUDIOCORE_ENABLE_MP3) || defined(CONFIG_ESPAUDIOCORE_ENABLE_AAC) || \
    defined(CONFIG_ESPAUDIOCORE_ENABLE_FLAC) || defined(CONFIG_ESPAUDIOCORE_ENABLE_VORBIS) || \
    defined(CONFIG_ESPAUDIOCORE_ENABLE_OPUS)
#include "driver/i2s_std.h"
#endif
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"

/* ===========================================================================
 * 错误码
 * =========================================================================*/

/** 返回码复用 IDF 语义，保证 esp_err_to_name() 打出正确名字 */
typedef enum {
    ESPAUDIOCORE_ERR_UNSUPPORTED = ESP_ERR_NOT_SUPPORTED,    /**< 没有能处理的路由 */
    ESPAUDIOCORE_ERR_OPEN_FAILED = ESP_ERR_NOT_FOUND,        /**< 文件/路径打不开 */
    ESPAUDIOCORE_ERR_HEADER      = ESP_ERR_INVALID_RESPONSE, /**< 头部/容器解析失败 */
    ESPAUDIOCORE_ERR_NOT_RUNNING = ESP_ERR_INVALID_STATE,    /**< 未在播放 */
    ESPAUDIOCORE_ERR_DECODER     = ESP_FAIL,                 /**< 解码失败，详情看 dec_err / EVT_ERROR */
} espaudiocore_err_t;

/* ===========================================================================
 * 事件
 * =========================================================================*/

typedef enum {
    ESPAUDIOCORE_EVT_STARTED = 0, /**< begin() 成功、解码任务已启动，开始出数据 */
    ESPAUDIOCORE_EVT_PAUSED,      /**< pause() 生效，已停止推送 */
    ESPAUDIOCORE_EVT_RESUMED,     /**< resume() 生效，恢复推送 */
    ESPAUDIOCORE_EVT_SEEKED,      /**< seek_ms() 完成，位置已生效 */
    ESPAUDIOCORE_EVT_EOS,         /**< 数据正常读到结尾，播放结束 */
    ESPAUDIOCORE_EVT_STOPPED,     /**< stop() 完成，任务已退出（EOS 之后也会自然触发） */
    ESPAUDIOCORE_EVT_ERROR,       /**< 错误；详情见 msg */
} espaudiocore_event_t;

/**
 * @brief 事件回调。在**解码任务上下文**同步执行。
 *
 * 契约：禁止阻塞（不要在里面做 I2S 写、网络请求、GUI 重绘）。
 *       msg 指向栈上格式化缓冲，仅在回调期间有效，需要留存必须拷贝。
 *       需要异步通知时，自行在回调里 xQueueSend()。
 */
typedef void (*espaudiocore_event_cb_t)(void *user, espaudiocore_event_t ev, const char *msg);

/**
 * @brief 一条元数据。文本与图片共用同一结构。
 *
 * @note APIC 等图片的 data 指向**解析缓冲内的原始字节**，本库不做额外拷贝，
 *       所以一个带大封面的 MP3 不会让本库吃掉几百 KB 堆。
 *       代价是 data 只在回调期间有效——需要留存必须在回调里自行拷贝。
 *       图片建议用"存文件 + 只留路径"的语义，而不是把图放进内存。
 */
typedef struct {
    const char *type;      /**< "Title"/"Artist"/"Album"/"APIC"/... */
    bool        is_binary; /**< APIC 等二进制元数据为 true */
    const char *data;      /**< 文本：NUL 结尾；二进制：原始字节 */
    size_t      len;       /**< 文本为 strlen；二进制为字节数（必需） */
    const char *mime;      /**< 图片 MIME，如 "image/jpeg"；文本为 NULL */
    int         pic_type;  /**< ID3 APIC picture type 0-20；非图片为 -1 */
} espaudiocore_meta_t;

/**
 * @brief 元数据回调。在**解码任务上下文**同步执行。
 *
 * @note meta->data 对图片指向解析缓冲内的原始字节，仅在回调期间有效。
 *       本回调同样禁止阻塞。
 */
typedef void (*espaudiocore_meta_cb_t)(void *user, const espaudiocore_meta_t *meta);

/* ===========================================================================
 * 格式（ringbuf 输入时必须显式给出）
 * =========================================================================*/

typedef enum {
    ESPAUDIOCORE_FMT_UNKNOWN = 0,
    ESPAUDIOCORE_FMT_WAV,
    ESPAUDIOCORE_FMT_MP3,
    ESPAUDIOCORE_FMT_AAC,
    ESPAUDIOCORE_FMT_FLAC,
    ESPAUDIOCORE_FMT_VORBIS,
    ESPAUDIOCORE_FMT_OPUS,
    ESPAUDIOCORE_FMT_M4A,
    ESPAUDIOCORE_FMT_WAVPACK,
} espaudiocore_format_t;

/* ===========================================================================
 * 配置
 * =========================================================================*/

typedef struct {
    uint32_t task_stack;    /**< 0 = 用解码器表推荐值（默认 8192，Opus 16384） */
    int      task_priority; /**< 0 = 用 Kconfig 默认值 */
    int      task_core;     /**< 默认 1；-1 = 不绑核 */
    uint32_t io_timeout_ms; /**< 输出阻塞超时；0 = 无限等待（推荐） */

    /**
     * 输出位宽，两种模式（可由 Kconfig 设默认值，此处优先）：
     *  - `0`：**跟随 PCM 位宽**，不做任何位宽处理；
     *  - `16/24/32`：**固定位宽输出**。当 PCM 位宽 < 目标位宽时样本左移补齐，
     *    当 PCM 位宽 > 目标位宽时右移截断。
     *
     * 例：PCM5102 固定 32-bit 槽宽时设为 32；16-bit 源会被左移 16 位。
     */
    uint8_t output_bits;

    /** 初始音量（dB）。0 = 原始幅度；-60 及以下视为静音。可为 0。 */
    float volume_db;

    espaudiocore_event_cb_t on_event; /**< 可为 NULL */
    espaudiocore_meta_cb_t  on_meta;  /**< 可为 NULL */
    void                   *user;     /**< 回调 user 指针 */
} espaudiocore_cfg_t;

/* ===========================================================================
 * 生命周期
 * =========================================================================*/

/**
 * @brief 打开本地文件并**立即开始**播放（内部起解码任务，不等播放结束）。
 *
 * @param path     绝对路径，如 "/sdcard/a.mp3"。**必须包含挂载点**，
 *                 不接受 Arduino 风格的相对路径。本库不做挂载动作，
 *                 挂载由应用负责。路径不以 '/' 开头时返回 ESP_ERR_INVALID_ARG。
 * @param tx       已 init_std_mode + enable 的 I2S TX 通道。所有权归应用，
 *                 本库只写入数据，并在采样率变化时重配其时钟（输出为 I2S 时）。
 * @param cfg      可为 NULL（全用默认值）
 * @param dec_err  [out] 解码器原生错误码；仅解码阶段失败时有效，可传 NULL。
 *                 调用者可据此区分"格式不支持"与"文件损坏"。
 *
 * @return
 *  - ESP_OK
 *  - ESP_ERR_INVALID_ARG       路径不合法 / 参数为空
 *  - ESP_ERR_NOT_FOUND         文件打不开
 *  - ESP_ERR_INVALID_RESPONSE  头部解析失败
 *  - ESP_ERR_NOT_SUPPORTED     没有能处理的路由
 *  - ESP_ERR_NO_MEM            内存不足
 *  - ESP_ERR_INVALID_STATE     I2S 配置失败
 */
esp_err_t espaudiocore_begin(const char *path, i2s_chan_handle_t tx, const espaudiocore_cfg_t *cfg,
                             int *dec_err);

/**
 * @brief 打开本地文件，把**解码后的 PCM 写进 ringbuf**（不碰任何硬件）。
 *
 * 与 espaudiocore_begin() 的唯一区别是输出端：这里写 ringbuf，由应用自行
 * 取走（落盘、再送 I2S、推流等）。
 *
 * @param path 绝对路径（含挂载点），同 espaudiocore_begin()
 * @param out  输出 PCM 的 ringbuf，所有权归调用者
 *
 * @note 输出为 ringbuf 时，采样率**不由本库重配**，而是通过
 *       espaudiocore_get_format() 上报，由应用自行处理（设计 D-9）。
 */
esp_err_t espaudiocore_begin_rb(const char *path, RingbufHandle_t out,
                                const espaudiocore_cfg_t *cfg, int *dec_err);

/**
 * @brief 从 ringbuf 读压缩数据流播放（网络流、上游解码器等）。
 *
 * ringbuf 源是**纯顺序流**：不支持 seek，也不做格式探测，
 * 因此格式必须由调用者显式给出。
 *
 * @param in  输入 ringbuf（压缩/编码数据），所有权归调用者
 * @param fmt 显式指定解码格式
 * @param tx  已 init_std_mode + enable 的 I2S TX 通道
 *
 * @note 该模式下以下能力返回 ESP_ERR_NOT_SUPPORTED：
 *       - espaudiocore_seek_ms()
 *       - 依赖 seek 的时长估算（get_duration_ms() 可能返回 <0）
 */
esp_err_t espaudiocore_begin_stream(RingbufHandle_t in, espaudiocore_format_t fmt,
                                    i2s_chan_handle_t tx, const espaudiocore_cfg_t *cfg,
                                    int *dec_err);

/**
 * @brief **两端都是 ringbuf** 的流式会话：输入压缩数据、输出 PCM。
 *
 * 适合"完整流式管道"这类拓扑——上游任务喂压缩数据、下游任务取 PCM，
 * 中间只经过本库解码，全程不触碰硬件：
 *
 *     任务A ──(压缩)──> in ringbuf ──> 本库解码 ──> out ringbuf ──> 任务B
 *
 * 与 espaudiocore_begin_stream() 相比，输出端不再伪装成 i2s_chan_handle_t，
 * 类型自解释，也不会误把 ringbuf 当 I2S 通道使用。
 *
 * @param in  输入 ringbuf（压缩数据）
 * @param fmt 显式指定解码格式（ringbuf 无法回退探测）
 * @param out 输出 ringbuf（PCM）
 *
 * @note 采样率通过 espaudiocore_get_format() 上报，由应用自行处理（D-9）。
 */
esp_err_t espaudiocore_begin_stream_rb(RingbufHandle_t in, espaudiocore_format_t fmt,
                                       RingbufHandle_t out, const espaudiocore_cfg_t *cfg,
                                       int *dec_err);

/**
 * @brief 停止播放并释放资源。**阻塞**至解码任务真正退出。
 *
 * 所有阻塞点都可被本调用唤醒，所以正常情况是毫秒级返回。
 * 返回后即可安全销毁 I2S 通道或 ringbuf。
 */
esp_err_t espaudiocore_stop(void);

/**
 * @brief 暂停：停止向输出推送新数据。
 *
 * @note 输出为 I2S 时，DMA 队列内的残留数据会继续播完，所以有
 *       dma_desc_num * dma_frame_num / sample_rate 的余音，该值由应用
 *       创建 I2S 通道时的 DMA 配置决定。需要更快的暂停响应就减小队列深度。
 *       本接口不会调用 i2s_channel_disable()（那会产生爆音）。
 */
esp_err_t espaudiocore_pause(void);

/** @brief 恢复播放。 */
esp_err_t espaudiocore_resume(void);

/**
 * @brief 跳转到指定位置。**阻塞**至跳转真正生效。
 *
 * 跳转在解码任务内串行执行（seek 源 → 复位解码器 → 继续解码），
 * 不能在应用层直接对源 seek，否则解码器内部状态会与文件位置错配。
 *
 * @param ms 目标位置（毫秒）
 * @return ESP_ERR_NOT_SUPPORTED（ringbuf 输入）/ ESP_ERR_NOT_RUNNING / ESP_OK
 */
esp_err_t espaudiocore_seek_ms(int64_t ms);

/**
 * @brief 设置输出音量（dB）。0 dB = 原始幅度，不改变电平。
 *
 * @param db 增益，单位 dB。建议范围 -60..+6；-60 及以下视为静音。
 *           正值放大（可能削顶饱和），负值衰减。
 *
 * @note 音量在输出汇的写入路径上做整数缩放（在 int32 域完成，带饱和处理），
 *       I2S 与 ringbuf 两种输出**语义一致**：
 *        - 增益为 0 dB 时不做任何乘加，直接写入（零额外开销）；
 *        - 正值放大，超过满量程会饱和削顶；负值衰减。
 *       可在播放中随时调用，立即生效，无需重开。
 *       若下游需要**未缩放**的原始 PCM，把音量保持 0 dB 即可。
 */
esp_err_t espaudiocore_set_volume_db(float db);

/** @brief 读取当前音量设置（dB）。 */
float espaudiocore_get_volume_db(void);

/* ===========================================================================
 * 查询
 * =========================================================================*/

/**
 * @brief 当前播放位置（毫秒），已扣除 I2S DMA 队列内的残留时长。
 *
 * 暂停期间位置冻结。未在播放时返回 0。
 */
int64_t espaudiocore_get_position_ms(void);

/** @brief 总时长（毫秒）；<0 表示未知（流式源、无 Xing 头等）。 */
int64_t espaudiocore_get_duration_ms(void);

/**
 * @brief 当前解码输出的格式。
 *
 * 用途取决于输出方式：
 *  - 输出为 I2S：本库内部自动处理采样率变化，此接口用于观察/UI 显示。
 *  - 输出为 Ringbuf：本库不碰任何硬件，此接口是应用层**唯一**的采样率信息源，
 *    应用需自行处理（重采样 / 下游协商 / 忽略）。
 *
 * 任一 out 参数可为 NULL。
 */
esp_err_t espaudiocore_get_format(uint32_t *rate, uint8_t *channels, uint8_t *bits);

/** @brief 是否正在播放（含暂停态）。 */
bool espaudiocore_is_running(void);

#ifdef __cplusplus
}
#endif
