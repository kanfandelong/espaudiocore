/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 kanfandelong. All rights reserved.
 */

/**
 * @file    audio_ringbuf.c
 * @brief   Thread-safe byte ring buffer used by the stream APIs.
 *
 * The implementation is deliberately simple: one mutex, one condition
 * variable, a linear byte buffer and head/used indices. A blocking reader or
 * writer wakes up every 100 ms to observe the session stop flag, which is what
 * makes espaudiocore_stop() bounded even when the ring is full.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "audio_ringbuf.h"

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <atomic>

struct espaudiocore_ringbuf {
    pthread_mutex_t        lock;
    pthread_cond_t         cond;
    uint8_t               *buf;
    size_t                 cap;
    size_t                 head; /**< index of the oldest byte */
    size_t                 used; /**< bytes queued */
    const std::atomic<bool> *stop; /**< optional session stop flag */
};

#define WAIT_SLICE_MS 100

static void deadline_from_now(struct timespec *ts, int timeout_ms)
{
    clock_gettime(CLOCK_MONOTONIC, ts);
    ts->tv_sec += timeout_ms / 1000;
    ts->tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec++;
        ts->tv_nsec -= 1000000000L;
    }
}

static int timeout_expired(const struct timespec *deadline)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (now.tv_sec > deadline->tv_sec) {
        return 1;
    }
    if (now.tv_sec < deadline->tv_sec) {
        return 0;
    }
    return now.tv_nsec >= deadline->tv_nsec;
}

static int should_stop(const espaudiocore_ringbuf_t *rb)
{
    return rb->stop && rb->stop->load(std::memory_order_relaxed);
}

/* Wait for at most `slice_ms`, or until the absolute deadline. Returns 1 when
 * the deadline has been reached (callers must re-check their predicate). */
static int wait_slice(espaudiocore_ringbuf_t *rb, const struct timespec *deadline, int has_deadline)
{
    if (!has_deadline) {
        struct timespec ts;
        deadline_from_now(&ts, WAIT_SLICE_MS);
        (void)pthread_cond_timedwait(&rb->cond, &rb->lock, &ts);
        return 0;
    }

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    int64_t remain_ms = ((int64_t)deadline->tv_sec - now.tv_sec) * 1000 +
                        ((int64_t)deadline->tv_nsec - now.tv_nsec) / 1000000;
    if (remain_ms <= 0) {
        return 1;
    }
    int slice = remain_ms > WAIT_SLICE_MS ? WAIT_SLICE_MS : (int)remain_ms;
    struct timespec ts;
    deadline_from_now(&ts, slice);
    (void)pthread_cond_timedwait(&rb->cond, &rb->lock, &ts);
    return timeout_expired(deadline);
}

espaudiocore_ringbuf_t *espaudiocore_ringbuf_create(size_t capacity)
{
    if (capacity == 0) {
        return NULL;
    }

    espaudiocore_ringbuf_t *rb = (espaudiocore_ringbuf_t *)calloc(1, sizeof(*rb));
    if (!rb) {
        return NULL;
    }
    rb->buf = (uint8_t *)malloc(capacity);
    if (!rb->buf) {
        free(rb);
        return NULL;
    }
    rb->cap = capacity;

    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
    /* Monotonic clock keeps timed waits immune to wall-clock jumps. */
    (void)pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);

    if (pthread_mutex_init(&rb->lock, NULL) != 0 ||
        pthread_cond_init(&rb->cond, &attr) != 0) {
        pthread_condattr_destroy(&attr);
        free(rb->buf);
        free(rb);
        return NULL;
    }
    pthread_condattr_destroy(&attr);
    return rb;
}

void espaudiocore_ringbuf_destroy(espaudiocore_ringbuf_t *rb)
{
    if (!rb) {
        return;
    }
    pthread_mutex_destroy(&rb->lock);
    pthread_cond_destroy(&rb->cond);
    free(rb->buf);
    free(rb);
}

void audio_ringbuf_set_stop(espaudiocore_ringbuf_t *rb, const std::atomic<bool> *flag)
{
    if (!rb) {
        return;
    }
    pthread_mutex_lock(&rb->lock);
    rb->stop = flag;
    pthread_cond_broadcast(&rb->cond);
    pthread_mutex_unlock(&rb->lock);
}

size_t espaudiocore_ringbuf_write(espaudiocore_ringbuf_t *rb, const void *data, size_t len,
                                  int timeout_ms)
{
    if (!rb || !data || len == 0) {
        return 0;
    }

    const uint8_t *src = (const uint8_t *)data;
    size_t total = 0;
    int has_deadline = timeout_ms > 0;
    struct timespec deadline;

    if (has_deadline) {
        deadline_from_now(&deadline, timeout_ms);
    }

    pthread_mutex_lock(&rb->lock);
    while (total < len) {
        if (should_stop(rb)) {
            break;
        }
        size_t space = rb->cap - rb->used;
        if (space > 0) {
            size_t chunk = len - total;
            if (chunk > space) {
                chunk = space;
            }
            size_t tail = (rb->head + rb->used) % rb->cap;
            size_t first = rb->cap - tail;
            if (first > chunk) {
                first = chunk;
            }
            memcpy(rb->buf + tail, src + total, first);
            if (chunk > first) {
                memcpy(rb->buf, src + total + first, chunk - first);
            }
            rb->used += chunk;
            total += chunk;
            pthread_cond_broadcast(&rb->cond);
            continue;
        }

        if (timeout_ms == 0) {
            break;
        }
        if (wait_slice(rb, &deadline, has_deadline)) {
            break;
        }
    }
    pthread_mutex_unlock(&rb->lock);
    return total;
}

size_t espaudiocore_ringbuf_read(espaudiocore_ringbuf_t *rb, void *data, size_t len,
                                 int timeout_ms)
{
    if (!rb || !data || len == 0) {
        return 0;
    }

    uint8_t *dst = (uint8_t *)data;
    int has_deadline = timeout_ms > 0;
    struct timespec deadline;

    if (has_deadline) {
        deadline_from_now(&deadline, timeout_ms);
    }

    pthread_mutex_lock(&rb->lock);
    while (rb->used == 0) {
        if (should_stop(rb) || timeout_ms == 0) {
            pthread_mutex_unlock(&rb->lock);
            return 0;
        }
        if (wait_slice(rb, &deadline, has_deadline)) {
            break;
        }
    }

    size_t n = rb->used < len ? rb->used : len;
    if (n > 0) {
        size_t first = rb->cap - rb->head;
        if (first > n) {
            first = n;
        }
        memcpy(dst, rb->buf + rb->head, first);
        if (n > first) {
            memcpy(dst + first, rb->buf, n - first);
        }
        rb->head = (rb->head + n) % rb->cap;
        rb->used -= n;
        pthread_cond_broadcast(&rb->cond);
    }
    pthread_mutex_unlock(&rb->lock);
    return n;
}

size_t espaudiocore_ringbuf_used(const espaudiocore_ringbuf_t *rb)
{
    if (!rb) {
        return 0;
    }
    /* Cast away const only to take the lock; no mutation happens. */
    espaudiocore_ringbuf_t *m = (espaudiocore_ringbuf_t *)rb;
    pthread_mutex_lock(&m->lock);
    size_t used = m->used;
    pthread_mutex_unlock(&m->lock);
    return used;
}

size_t espaudiocore_ringbuf_space(const espaudiocore_ringbuf_t *rb)
{
    if (!rb) {
        return 0;
    }
    espaudiocore_ringbuf_t *m = (espaudiocore_ringbuf_t *)rb;
    pthread_mutex_lock(&m->lock);
    size_t space = m->cap - m->used;
    pthread_mutex_unlock(&m->lock);
    return space;
}

size_t espaudiocore_ringbuf_capacity(const espaudiocore_ringbuf_t *rb)
{
    return rb ? rb->cap : 0;
}

void espaudiocore_ringbuf_clear(espaudiocore_ringbuf_t *rb)
{
    if (!rb) {
        return;
    }
    pthread_mutex_lock(&rb->lock);
    rb->head = 0;
    rb->used = 0;
    pthread_cond_broadcast(&rb->cond);
    pthread_mutex_unlock(&rb->lock);
}
