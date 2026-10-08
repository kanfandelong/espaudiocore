/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 kanfandelong. All rights reserved.
 */

/**
 * @file    xing_parser.h
 * @brief   Xing / Info / LAME 头解析（用于 MP3 精确时长与平均比特率）。
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus

struct XingHeaderInfo {
    bool     valid = false; /**< 是否成功解析 */
    uint32_t frames = 0;    /**< 总帧数 */
    uint32_t bytes = 0;     /**< 音频数据总字节数（0 表示头里未存） */
    uint32_t bitrate = 0;   /**< 平均比特率 bps */
    float    duration = 0;  /**< 总时长（秒） */
    int      sampleRate = 0;
    int      channels = 0;
};

/**
 * @brief 解析 Xing/Info 头。
 * @param buf 至少包含一个完整 MPEG 帧的数据
 * @param len 可用字节数
 */
XingHeaderInfo parseXingHeader(const uint8_t *buf, size_t len);

#endif /* __cplusplus */
