/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 kanfandelong. All rights reserved.
 */

/**
 * @file    espaudiocore.c
 * @brief   公共 API 薄封装。所有实际工作都在 audio_player.c。
 */

#include "espaudiocore.h"

#include "audio_player.h"
#include "audio_port.h"

#include <stddef.h>

esp_err_t espaudiocore_begin(const char *path, i2s_chan_handle_t tx, const espaudiocore_cfg_t *cfg,
                             int *dec_err)
{
    return audio_player_start_file(path, tx, nullptr, cfg, dec_err, /*i2s_output=*/true);
}

esp_err_t espaudiocore_begin_rb(const char *path, RingbufHandle_t out,
                                const espaudiocore_cfg_t *cfg, int *dec_err)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    return audio_player_start_file(path, nullptr, reinterpret_cast<void *>(out), cfg, dec_err,
                                   /*i2s_output=*/false);
}

esp_err_t espaudiocore_begin_stream(RingbufHandle_t in, espaudiocore_format_t fmt, i2s_chan_handle_t tx,
                                    const espaudiocore_cfg_t *cfg, int *dec_err)
{
    if (!in) {
        return ESP_ERR_INVALID_ARG;
    }
    return audio_player_start_stream(in, fmt, tx, nullptr, cfg, dec_err, /*i2s_output=*/true);
}

esp_err_t espaudiocore_begin_stream_rb(RingbufHandle_t in, espaudiocore_format_t fmt,
                                       RingbufHandle_t out, const espaudiocore_cfg_t *cfg,
                                       int *dec_err)
{
    if (!in || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    return audio_player_start_stream(in, fmt, nullptr, reinterpret_cast<void *>(out), cfg, dec_err,
                                     /*i2s_output=*/false);
}

esp_err_t espaudiocore_stop(void)
{
    return audio_player_stop();
}

esp_err_t espaudiocore_pause(void)
{
    return audio_player_pause();
}

esp_err_t espaudiocore_resume(void)
{
    return audio_player_resume();
}

esp_err_t espaudiocore_seek_ms(int64_t ms)
{
    return audio_player_seek_ms(ms);
}

int64_t espaudiocore_get_position_ms(void)
{
    return audio_player_get_position_ms();
}

int64_t espaudiocore_get_duration_ms(void)
{
    return audio_player_get_duration_ms();
}

esp_err_t espaudiocore_get_format(uint32_t *rate, uint8_t *channels, uint8_t *bits)
{
    return audio_player_get_format(rate, channels, bits);
}

bool espaudiocore_is_running(void)
{
    return audio_player_is_running();
}

esp_err_t espaudiocore_set_volume_db(float db)
{
    return audio_player_set_volume_db(db);
}

float espaudiocore_get_volume_db(void)
{
    return audio_player_get_volume_db();
}
