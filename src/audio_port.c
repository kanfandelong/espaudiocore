/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 kanfandelong. All rights reserved.
 */

/**
 * @file    audio_port.c
 * @brief   Native Linux platform layer implementation.
 */

#define _GNU_SOURCE

#include "audio_port.h"

#include "audio_err.h"

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ===========================================================================
 * Logging
 * =========================================================================*/

static int g_log_level = -1; /* -1 = not initialised yet */
static pthread_once_t g_log_once = PTHREAD_ONCE_INIT;

static void log_init_once(void)
{
    const char *env = getenv("ESPAUDIOCORE_LOG_LEVEL");
    if (env && *env) {
        char *end = NULL;
        long v = strtol(env, &end, 10);
        if (end && *end == '\0' && v >= AUDIO_LOG_NONE && v <= AUDIO_LOG_DEBUG) {
            g_log_level = (int)v;
            return;
        }
    }
    g_log_level = AUDIO_LOG_INFO;
}

static int log_level(void)
{
    pthread_once(&g_log_once, log_init_once);
    return g_log_level;
}

void audio_log_set_level(int level)
{
    if (level < AUDIO_LOG_NONE) {
        level = AUDIO_LOG_NONE;
    }
    if (level > AUDIO_LOG_DEBUG) {
        level = AUDIO_LOG_DEBUG;
    }
    pthread_once(&g_log_once, log_init_once);
    g_log_level = level;
}

int audio_log_get_level(void)
{
    return log_level();
}

static const char *level_name(int level)
{
    switch (level) {
    case AUDIO_LOG_ERROR: return "E";
    case AUDIO_LOG_WARN:  return "W";
    case AUDIO_LOG_INFO:  return "I";
    case AUDIO_LOG_DEBUG: return "D";
    default:              return "?";
    }
}

void audio_log_write(int level, const char *fmt, ...)
{
    if (level > log_level()) {
        return;
    }

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);

    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    /* One fprintf per line: stderr is line buffered/thread safe enough for the
     * single decoder thread plus control thread. */
    fprintf(stderr, "[%4ld.%03ld] %s espaudiocore: %s\n", (long)ts.tv_sec,
            (long)(ts.tv_nsec / 1000000L), level_name(level), msg);
}

/* ===========================================================================
 * Memory
 * =========================================================================*/

void *audio_alloc_big(size_t size, size_t align)
{
    if (size == 0) {
        return NULL;
    }
    if (align == 0) {
        align = 64; /* cache line */
    }
    /* posix_memalign requires power-of-two alignment >= sizeof(void*). */
    if (align < sizeof(void *)) {
        align = sizeof(void *);
    }
    void *p = NULL;
    if (posix_memalign(&p, align, size) != 0) {
        return NULL;
    }
    return p;
}

void audio_free(void *p)
{
    free(p);
}

/* ===========================================================================
 * Errors
 * =========================================================================*/

int audio_err_to_code(int audio_err)
{
    switch (audio_err) {
    case AUDIO_OK:
        return 0;
    case AUDIO_ERR_INVALID_ARG:
        return -EINVAL;
    case AUDIO_ERR_NO_MEM:
        return -ENOMEM;
    case AUDIO_ERR_NOT_SUPPORTED:
        return -ENOTSUP;
    case AUDIO_ERR_PARSE:
        return -EBADMSG;
    case AUDIO_ERR_IO:
        return -EIO;
    case AUDIO_ERR_DECODE:
        return -EILSEQ;
    case AUDIO_ERR_EOF:
        return 0;
    default:
        return -EIO;
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
