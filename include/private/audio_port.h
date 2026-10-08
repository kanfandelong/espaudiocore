/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 kanfandelong. All rights reserved.
 */

/**
 * @file    audio_port.h
 * @brief   平台适配层：日志 / 内存 / 错误转换。**唯一**允许出现平台差异的地方。
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ===========================================================================
 * 日志
 * =========================================================================*/

/** 全库统一 TAG：可用 esp_log_level_set(ESPAUDIOCORE_TAG, ESP_LOG_WARN) 一次控制 */
#define ESPAUDIOCORE_TAG "espaudiocore"

#define AUDIO_LOGE(fmt, ...) ESP_LOGE(ESPAUDIOCORE_TAG, fmt, ##__VA_ARGS__)
#define AUDIO_LOGW(fmt, ...) ESP_LOGW(ESPAUDIOCORE_TAG, fmt, ##__VA_ARGS__)
#define AUDIO_LOGI(fmt, ...) ESP_LOGI(ESPAUDIOCORE_TAG, fmt, ##__VA_ARGS__)
#define AUDIO_LOGD(fmt, ...) ESP_LOGD(ESPAUDIOCORE_TAG, fmt, ##__VA_ARGS__)
#define AUDIO_LOGV(fmt, ...) ESP_LOGV(ESPAUDIOCORE_TAG, fmt, ##__VA_ARGS__)

/* ===========================================================================
 * 内存
 * =========================================================================*/

/**
 * @brief 大块缓冲：优先 PSRAM 且尝试带 DMA 能力，失败退回纯 PSRAM。
 *
 * 两者都失败时返回 NULL（不回退到内部 RAM，避免悄悄吃掉宝贵的内部内存；
 * 若确实需要内部 RAM，调用者显式用 audio_alloc_dma()）。
 *
 * @param align 对齐字节数，0 表示默认（32 字节，兼顾 cache 行与 DMA）
 */
void *audio_alloc_big(size_t size, size_t align);

/**
 * @brief 必须能被 DMA 访问的缓冲（I2S 描述符相关）。
 * @note 只在明确需要 DMA 直连时使用。
 */
void *audio_alloc_dma(size_t size);

void audio_free(void *p);

/** @brief 大块缓冲申请的降级统计，用于诊断（返回尝试 PSRAM+DMA 失败的次数） */
uint32_t audio_alloc_psram_dma_miss_count(void);

/* ===========================================================================
 * 错误转换
 * =========================================================================*/

/** 内部错误码 -> 公共 esp_err_t */
esp_err_t audio_err_to_esp(int audio_err);

/** 取可读名，用于日志 */
const char *audio_err_name(int audio_err);

/* ===========================================================================
 * 字节序 / 小工具
 * =========================================================================*/

static inline uint16_t audio_rd_le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static inline uint32_t audio_rd_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline uint32_t audio_rd_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static inline uint64_t audio_rd_be64(const uint8_t *p)
{
    return ((uint64_t)audio_rd_be32(p) << 32) | (uint64_t)audio_rd_be32(p + 4);
}

#ifdef __cplusplus
}
#endif
