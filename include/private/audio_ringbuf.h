/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 kanfandelong. All rights reserved.
 */

/**
 * @file    audio_ringbuf.h
 * @brief   Internal helpers for the public ring buffer.
 */

#pragma once

#include <atomic>

#include "espaudiocore.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Bind a stop flag to the ring buffer.
 *
 * Blocking reads/writes poll this flag every 100 ms and return early once it
 * becomes true. This is how espaudiocore_stop() interrupts a decoder blocked
 * on a full output ring.
 */
void audio_ringbuf_set_stop(espaudiocore_ringbuf_t *rb, const std::atomic<bool> *flag);

#ifdef __cplusplus
}
#endif
