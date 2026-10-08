/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 kanfandelong. All rights reserved.
 */

/**
 * @file    audio_sink.h
 * @brief   数据汇具体实现：I2S / ringbuf。内部使用。
 */

#pragma once

#include "audio_types.h"

#include "driver/i2s_std.h"
#include "freertos/ringbuf.h"

/* 本头文件含 C++ 类型，仅供 .cpp 包含 */

/**
 * @brief I2S 输出汇。
 *
 * @param tx            调用者持有的 TX 通道（须已 init_std_mode + enable）
 * @param io_timeout_ms 单次 i2s_channel_write 的超时；0 表示用默认值（20ms）
 * @param target_bits   强制输出位宽（8/16/24/32）；0 = 跟随解码器原生位宽。
 *                      大于源位宽时样本左移补齐（PCM5102 需要 32）。
 * @note 本类只写数据与重配时钟，不负责创建/销毁通道。
 */
AudioSink *audio_sink_i2s_create(i2s_chan_handle_t tx, uint32_t io_timeout_ms, uint8_t target_bits);

/** 绑定停止标志：write() 的等待会被它打断（见 D-14） */
void audio_sink_i2s_bind_stop(AudioSink *sink, const volatile bool *flag);

/** I2S DMA 队列内尚未播出的字节数（用于位置查询扣除） */
size_t audio_sink_i2s_pending_bytes(AudioSink *sink);

/** 当前输出汇中尚未播出的字节数；ringbuf 输出恒为 0 */
size_t audio_sink_pending_bytes(AudioSink *sink);

/**
 * @brief ringbuf 输出汇（PCM 帧）。
 *
 * 该模式不碰任何硬件：采样率变化不重配任何东西，只由
 * espaudiocore_get_format() 报告，应用层自行处理（见 D-9）。
 *
 * @param target_bits 强制输出位宽；0 = 跟随解码器原生位宽
 */
AudioSink *audio_sink_ringbuf_create(RingbufHandle_t rb, uint8_t target_bits);

/** 绑定停止标志 */
void audio_sink_ringbuf_bind_stop(AudioSink *sink, const volatile bool *flag);

