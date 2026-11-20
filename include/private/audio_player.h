/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 kanfandelong. All rights reserved.
 */

/**
 * @file    audio_player.h
 * @brief   Internal player interface. The public API in espaudiocore.h is a
 *          thin wrapper around this layer.
 */

#pragma once

#include "audio_types.h"
#include "espaudiocore.h"

/* This header contains C++ types and is for .cpp files only. */

/** Start from a local file (device output when alsa_device != NULL). */
int audio_player_start_file(const char *path, const char *alsa_device,
                            espaudiocore_ringbuf_t *out_rb, const espaudiocore_cfg_t *cfg,
                            int *dec_err, bool device_output);

/** Start from a ring buffer (format must be explicit, no probing). */
int audio_player_start_stream(espaudiocore_ringbuf_t *in_rb, espaudiocore_format_t fmt,
                              const char *alsa_device, espaudiocore_ringbuf_t *out_rb,
                              const espaudiocore_cfg_t *cfg, int *dec_err, bool device_output);

int audio_player_stop();
int audio_player_pause();
int audio_player_resume();
int audio_player_seek_ms(int64_t ms);

/** Set volume in dB (applied by the sink). */
int   audio_player_set_volume_db(float db);
float audio_player_get_volume_db();

int64_t audio_player_get_position_ms();
int64_t audio_player_get_duration_ms();
int     audio_player_get_format(uint32_t *rate, uint8_t *channels, uint8_t *bits);
bool    audio_player_is_running();

/** True when output is an ALSA device (the library owns rate changes). */
bool audio_player_is_device_output();

/** Last decoder-native error code. */
int audio_player_last_decoder_error();

/** Current output sink (internal uses such as position accounting). */
AudioSink *audio_player_sink();
