/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 kanfandelong. All rights reserved.
 */

/**
 * @file    audio_port.c
 * @brief   平台适配层实现
 */

#include "audio_port.h"
#include "audio_err.h"

#include <stdlib.h>
#include <string.h>

#if defined(CONFIG_ESPAUDIOCORE_DEBUG_DIAGNOSTICS)
static uint32_t s_psram_dma_miss = 0;
static uint32_t s_psram_miss = 0;
#endif

void *audio_alloc_big(size_t size, size_t align)
{
    if (size == 0) {
        return NULL;
    }
    if (align == 0) {
        align = 32; /* 兼顾 cache 行与 DMA 对齐 */
    }

    /* 第一优先：PSRAM + DMA（在支持 PSRAM-DMA 的芯片上驱动可以直接搬运） */
    void *p = heap_caps_aligned_alloc(align, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    if (p) {
        return p;
    }
#if defined(CONFIG_ESPAUDIOCORE_DEBUG_DIAGNOSTICS)
    s_psram_dma_miss++;
#endif

    /* 第二优先：纯 PSRAM。非 DMA 的 PSRAM 交给 SDMMC 这类驱动时，
     * 驱动会自己分配临时 DMA 缓冲并拷贝，功能与完整性都有保障。 */
    p = heap_caps_aligned_alloc(align, size, MALLOC_CAP_SPIRAM);
    if (p) {
        return p;
    }
#if defined(CONFIG_ESPAUDIOCORE_DEBUG_DIAGNOSTICS)
    s_psram_miss++;
#endif

#if !CONFIG_ESPAUDIOCORE_PSRAM_REQUIRED
    /* 最后兜底：内部 RAM。仅在未强制要求 PSRAM 时启用。 */
    p = heap_caps_aligned_alloc(align, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (p) {
        AUDIO_LOGW("PSRAM exhausted, fell back to internal RAM (%u bytes)", (unsigned)size);
    }
#endif
    return p;
}

void *audio_alloc_dma(size_t size)
{
    if (size == 0) {
        return NULL;
    }
    return heap_caps_aligned_alloc(32, size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
}

void audio_free(void *p)
{
    if (p) {
        heap_caps_free(p);
    }
}

#if defined(CONFIG_ESPAUDIOCORE_DEBUG_DIAGNOSTICS)
uint32_t audio_alloc_psram_dma_miss_count(void)
{
    return s_psram_dma_miss;
}

uint32_t audio_alloc_psram_miss_count(void)
{
    return s_psram_miss;
}
#endif

esp_err_t audio_err_to_esp(int audio_err)
{
    switch (audio_err) {
    case AUDIO_OK:
        return ESP_OK;
    case AUDIO_ERR_INVALID_ARG:
        return ESP_ERR_INVALID_ARG;
    case AUDIO_ERR_NO_MEM:
        return ESP_ERR_NO_MEM;
    case AUDIO_ERR_NOT_SUPPORTED:
        return ESP_ERR_NOT_SUPPORTED;
    case AUDIO_ERR_PARSE:
        return ESP_ERR_INVALID_RESPONSE;
    case AUDIO_ERR_IO:
        return ESP_ERR_INVALID_STATE;
    case AUDIO_ERR_DECODE:
        return ESP_FAIL;
    case AUDIO_ERR_EOF:
        return ESP_OK;
    default:
        return ESP_FAIL;
    }
}

const char *audio_err_name(int audio_err)
{
    switch (audio_err) {
    case AUDIO_OK:
        return "OK";
    case AUDIO_FAIL:
        return "FAIL";
    case AUDIO_ERR_INVALID_ARG:
        return "INVALID_ARG";
    case AUDIO_ERR_NO_MEM:
        return "NO_MEM";
    case AUDIO_ERR_NOT_SUPPORTED:
        return "NOT_SUPPORTED";
    case AUDIO_ERR_PARSE:
        return "PARSE";
    case AUDIO_ERR_IO:
        return "IO";
    case AUDIO_ERR_DECODE:
        return "DECODE";
    case AUDIO_ERR_EOF:
        return "EOF";
    default:
        return "UNKNOWN";
    }
}
