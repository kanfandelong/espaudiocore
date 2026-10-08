/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 kanfandelong. All rights reserved.
 */

/**
 * @file    audio_player.h
 * @brief   播放器内部接口。公共 API（espaudiocore.h）薄封装到这一层。
 */

#pragma once

#include "audio_types.h"
#include "espaudiocore.h"

#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"

/* 本头文件含 C++ 类型，仅供 .cpp 包含 */

/** 从本地文件起播（path 必须为含挂载点的绝对路径） */
esp_err_t audio_player_start_file(const char *path, i2s_chan_handle_t tx, void *out_rb,
                                  const espaudiocore_cfg_t *cfg, int *dec_err, bool i2s_output);

/** 从 ringbuf 起播（须显式指定格式，不做探测） */
esp_err_t audio_player_start_stream(RingbufHandle_t in, espaudiocore_format_t fmt,
                                    i2s_chan_handle_t tx, void *out_rb,
                                    const espaudiocore_cfg_t *cfg, int *dec_err, bool i2s_output);

esp_err_t audio_player_stop();
esp_err_t audio_player_pause();
esp_err_t audio_player_resume();
esp_err_t audio_player_seek_ms(int64_t ms);

/** 设置音量（dB）。I2S 输出会施加缩放；ringbuf 输出忽略以保持数据纯净。 */
esp_err_t audio_player_set_volume_db(float db);
float     audio_player_get_volume_db();

int64_t   audio_player_get_position_ms();
int64_t   audio_player_get_duration_ms();
esp_err_t audio_player_get_format(uint32_t *rate, uint8_t *channels, uint8_t *bits);
bool      audio_player_is_running();

/** 输出是否为 I2S（决定采样率由本库内部处理还是交给应用，见 D-9） */
bool audio_player_is_i2s_output();

/** 最近一次解码器原生错误码 */
int audio_player_last_decoder_error();

/** 取当前输出汇（位置计算等内部用途） */
AudioSink *audio_player_sink();

