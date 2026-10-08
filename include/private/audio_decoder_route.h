/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 kanfandelong. All rights reserved.
 */

/**
 * @file    audio_decoder_route.h
 * @brief   解码器路由：扩展名优先 + 魔数回退。内部使用。
 */

#pragma once

#include "audio_types.h"
#include "decoder.h"

#ifdef __cplusplus

/** 本头文件含 C++ 类型，仅供 .cpp 包含 */

/**
 * @brief 为给定数据源挑选解码器条目。
 *
 * 流程（见 D-6）：
 *  1. 按路径扩展名查表得到首选；
 *  2. 若首选不被该源支持（例如 ringbuf 上不支持 seek 的容器格式），换下一个候选；
 *  3. 候选都不行 -> 读头部魔数探测；
 *  4. 命中魔数 -> 把源位置复位到 0（仅当 can_seek()）后采用。
 *
 * @param in   数据源（探测过程不改动其逻辑位置）
 * @param path 文件路径，可为 NULL（ringbuf 场景）
 * @return 命中的条目；NULL 表示没有能处理的解码器
 */
const audio_decoder_entry_t *audio_decoder_route(AudioInput *in, const char *path);

/** 按扩展名查表。返回下标，-1 表示无匹配。 */
int audio_decoder_find_by_ext(const char *path);

/** 按魔数查表（用各条目的 probe）。返回下标，-1 表示无匹配。 */
int audio_decoder_find_by_magic(AudioInput *in);

/** 取内部路由表（以 ext==NULL 结尾） */
const audio_decoder_entry_t *audio_decoder_table();

/**
 * @brief 把公共格式枚举映射到路由表条目（ringbuf 输入用，不做探测）。
 * @return NULL 表示该格式没有可用的解码器
 */
const audio_decoder_entry_t *audio_decoder_for_format(espaudiocore_format_t fmt);

#endif /* __cplusplus */
