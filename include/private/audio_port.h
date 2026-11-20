/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 kanfandelong. All rights reserved.
 */

/**
 * @file    audio_port.h
 * @brief   Linux platform layer: logging, aligned memory, error mapping.
 *
 * This is a *native* Linux layer, not an emulation of an embedded RTOS. It
 * contains no ESP-IDF, FreeRTOS, I2S or ring-buffer API.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ===========================================================================
 * Logging
 * =========================================================================*/

#define AUDIO_LOG_NONE  0
#define AUDIO_LOG_ERROR 1
#define AUDIO_LOG_WARN  2
#define AUDIO_LOG_INFO  3
#define AUDIO_LOG_DEBUG 4

/** Process-wide log level, initialised from $ESPAUDIOCORE_LOG_LEVEL. */
void audio_log_set_level(int level);
int  audio_log_get_level(void);

/** printf-style log sink; normally called through the macros below. */
#if defined(__GNUC__)
__attribute__((format(printf, 2, 3)))
#endif
void audio_log_write(int level, const char *fmt, ...);

#define AUDIO_LOGE(...) audio_log_write(AUDIO_LOG_ERROR, __VA_ARGS__)
#define AUDIO_LOGW(...) audio_log_write(AUDIO_LOG_WARN, __VA_ARGS__)
#define AUDIO_LOGI(...) audio_log_write(AUDIO_LOG_INFO, __VA_ARGS__)
#define AUDIO_LOGD(...) audio_log_write(AUDIO_LOG_DEBUG, __VA_ARGS__)

/* ===========================================================================
 * Memory
 * =========================================================================*/

/**
 * @brief Allocate a large, cache-line aligned buffer.
 *
 * @param align Alignment in bytes; 0 means 64 (cache line).
 * @return NULL on failure.
 */
void *audio_alloc_big(size_t size, size_t align);

/** @brief Free a buffer returned by audio_alloc_big(). NULL is accepted. */
void audio_free(void *p);

/* ===========================================================================
 * Errors
 * =========================================================================*/

/** Internal audio_err_t -> public negative errno code. */
int audio_err_to_code(int audio_err);

/** Readable name for an internal audio_err_t, used by logs. */
const char *audio_err_name(int audio_err);

/* ===========================================================================
 * Byte order helpers (decoders parse little/big endian containers)
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
