/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 kanfandelong. All rights reserved.
 */

/**
 * @file    espaudiocore.c
 * @brief   Public API thin wrapper. All real work lives in audio_player.c.
 */

#include "espaudiocore.h"

#include "audio_player.h"
#include "audio_port.h"

#include <errno.h>
#include <stddef.h>
#include <string.h>

const char *espaudiocore_strerror(int err)
{
    if (err == 0)
    {
        return "OK";
    }
    if (err < 0)
    {
        return strerror(-err);
    }
    return strerror(err);
}

void espaudiocore_set_log_level(int level)
{
    audio_log_set_level(level);
}

int espaudiocore_get_log_level(void)
{
    return audio_log_get_level();
}

int espaudiocore_begin(const char *path, const espaudiocore_cfg_t *cfg, int *dec_err)
{
    return audio_player_start_file(path, cfg ? cfg->alsa_device : nullptr, nullptr, cfg, dec_err,
                                   /*device_output=*/true);
}

int espaudiocore_begin_rb(const char *path, espaudiocore_ringbuf_t *out,
                          const espaudiocore_cfg_t *cfg, int *dec_err)
{
    if (!out)
    {
        return ESPAUDIOCORE_ERR_INVALID_ARG;
    }
    return audio_player_start_file(path, nullptr, out, cfg, dec_err, /*device_output=*/false);
}

int espaudiocore_begin_stream(espaudiocore_ringbuf_t *in, espaudiocore_format_t fmt,
                              const espaudiocore_cfg_t *cfg, int *dec_err)
{
    if (!in)
    {
        return ESPAUDIOCORE_ERR_INVALID_ARG;
    }
    return audio_player_start_stream(in, fmt, cfg ? cfg->alsa_device : nullptr, nullptr, cfg,
                                     dec_err, /*device_output=*/true);
}

int espaudiocore_begin_stream_rb(espaudiocore_ringbuf_t *in, espaudiocore_format_t fmt,
                                 espaudiocore_ringbuf_t *out, const espaudiocore_cfg_t *cfg,
                                 int *dec_err)
{
    if (!in || !out)
    {
        return ESPAUDIOCORE_ERR_INVALID_ARG;
    }
    return audio_player_start_stream(in, fmt, nullptr, out, cfg, dec_err,
                                     /*device_output=*/false);
}

int espaudiocore_stop(void)
{
    return audio_player_stop();
}

int espaudiocore_pause(void)
{
    return audio_player_pause();
}

int espaudiocore_resume(void)
{
    return audio_player_resume();
}

int espaudiocore_seek_ms(int64_t ms)
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

int espaudiocore_get_format(uint32_t *rate, uint8_t *channels, uint8_t *bits)
{
    return audio_player_get_format(rate, channels, bits);
}

bool espaudiocore_is_running(void)
{
    return audio_player_is_running();
}

int espaudiocore_set_volume_db(float db)
{
    return audio_player_set_volume_db(db);
}

float espaudiocore_get_volume_db(void)
{
    return audio_player_get_volume_db();
}
