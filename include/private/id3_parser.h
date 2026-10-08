/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 kanfandelong. All rights reserved.
 */

/**
 * @file    id3_parser.h
 * @brief   ID3v2 标签解析（文本 + 图片）。内部使用。
 *
 * @note 解析器只**读取并上报**，不缓存任何标签内容。图片以原始字节形式
 * 交给回调，由应用决定是否落盘（本库不把封面放进内存）。
 */

#pragma once

#include "audio_types.h"

#ifdef __cplusplus

/**
 * @brief 若数据源开头是 ID3v2 标签，则解析并通过 meta_cb 上报。
 *
 * @param in        数据源（内部会读取，读取位置停在标签之后）
 * @param meta_cb   元数据回调，可为 NULL（为 NULL 时仍会跳过标签）
 * @param meta_user 回调 user
 * @return 已跳过的标签总字节数（无标签返回 0）
 *
 * @note 文本帧的 data 为 NUL 结尾的 UTF-8；图片帧 is_binary=true，
 *       data 指向标签缓冲内的原始图片字节，仅在回调期间有效。
 */
size_t id3_parse_and_report(AudioInput *in, espaudiocore_meta_cb_t meta_cb, void *meta_user);

#endif /* __cplusplus */
