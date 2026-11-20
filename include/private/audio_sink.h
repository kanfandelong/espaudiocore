/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 kanfandelong. All rights reserved.
 */

/**
 * @file    audio_sink.h
 * @brief   Concrete sinks: ALSA device / ring buffer. Internal use.
 */

#pragma once

#include <atomic>

#include "audio_types.h"
#include "espaudiocore.h"

/* This header contains C++ types and is for .cpp files only. */

/**
 * @brief ALSA output sink.
 *
 * @param device        ALSA PCM name ("default", "plughw:0,0", ...)
 * @param io_timeout_ms write timeout; 0 = no timeout
 * @param target_bits   forced output bit depth (16/24/32); 0 = follow source
 * @param latency_ms    target ALSA latency; 0 = 200 ms
 * @return NULL on failure to open/configure the device
 *
 * @note The device is opened immediately so configuration errors surface at
 *       begin() time. Actual hw_params are applied lazily on the first write,
 *       after the decoder has reported its real rate/bits/channels.
 */
AudioSink *audio_sink_alsa_create(const char *device, uint32_t io_timeout_ms, uint8_t target_bits,
                                  uint32_t latency_ms);

/** @brief Bind the session stop flag so blocking writes can be interrupted. */
void audio_sink_alsa_bind_stop(AudioSink *sink, const std::atomic<bool> *flag);

/**
 * @brief Bind the PCM tap callback (FFT / waveform).
 *
 * Fired before volume scaling and before the ALSA write, with int32 samples.
 */
void audio_sink_alsa_set_pcm(AudioSink *sink, espaudiocore_pcm_cb_t cb, void *user);

/** @brief Frames still buffered in ALSA (used by get_position_ms). */
size_t audio_sink_alsa_pending_frames(AudioSink *sink);

/** @brief Frames still pending in the output; 0 for the ring-buffer sink. */
size_t audio_sink_pending_frames(AudioSink *sink);

/**
 * @brief Ring-buffer output sink (PCM frames).
 *
 * This mode never touches hardware. Rate changes are not reconfigured; the
 * application reads them through espaudiocore_get_format().
 *
 * @param rb          output ring buffer (owned by the application)
 * @param target_bits forced output bit depth; 0 = follow source
 */
AudioSink *audio_sink_ringbuf_create(espaudiocore_ringbuf_t *rb, uint8_t target_bits);

/** @brief Bind the session stop flag. */
void audio_sink_ringbuf_bind_stop(AudioSink *sink, const std::atomic<bool> *flag);
