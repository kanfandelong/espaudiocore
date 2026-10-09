/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 kanfandelong. All rights reserved.
 */

/**
 * @file    audio_decoder_route.c
 * @brief   解码器路由表与匹配逻辑（见 D-6）。
 *
 * 新增格式 = 在 g_decoders[] 里加一行 + 实现该格式的 create/probe。
 * 表按 Kconfig 裁剪：未启用的格式条目直接不进表，因此"不支持"是
 * 编译期决定，而不是运行时才发现。
 */

#include "audio_decoder_route.h"
#include "audio_port.h"
#include "decoder.h"

#include <stdio.h>
#include <string.h>
#include <strings.h> /* strcasecmp */

#include "sdkconfig.h"

/* 各格式的 create / probe 由对应 decoder_*.cpp 提供（C++ 链接） */
AudioDecoder *create_wav(void);
AudioDecoder *create_mp3(void);
AudioDecoder *create_aac(void);
AudioDecoder *create_flac(void);
AudioDecoder *create_vorbis(void);
AudioDecoder *create_opus(void);
AudioDecoder *create_wavpack(void);
bool probe_wav(AudioInput *in);
bool probe_mp3(AudioInput *in);
bool probe_aac(AudioInput *in);
bool probe_flac(AudioInput *in);
bool probe_vorbis(AudioInput *in);
bool probe_opus(AudioInput *in);
bool probe_wavpack(AudioInput *in);

/* 注意：OGG 容器要能区分 Vorbis / Opus，所以 probe_vorbis 必须排在 probe_opus 之前
 * （Vorbis 的探测会看第一页 payload 是否为 0x01"vorbis"，不是才让给 Opus）。 */
static const audio_decoder_entry_t g_decoders[] = {
#ifdef CONFIG_ESPAUDIOCORE_ENABLE_WAV
    { "wav", "WAV", probe_wav, create_wav, 8192 },
#endif
#ifdef CONFIG_ESPAUDIOCORE_ENABLE_MP3
    { "mp3", "MP3", probe_mp3, create_mp3, 8192 },
#endif
#ifdef CONFIG_ESPAUDIOCORE_ENABLE_AAC
    { "aac", "AAC", probe_aac, create_aac, 8192 },
#endif
#ifdef CONFIG_ESPAUDIOCORE_ENABLE_FLAC
    { "flac", "FLAC", probe_flac, create_flac, 8192 },
#endif
#ifdef CONFIG_ESPAUDIOCORE_ENABLE_VORBIS
    { "ogg", "Vorbis", probe_vorbis, create_vorbis, 8192 },
#endif
#ifdef CONFIG_ESPAUDIOCORE_ENABLE_OPUS
    { "opus", "Opus", probe_opus, create_opus, 16384 },
#endif
#ifdef CONFIG_ESPAUDIOCORE_ENABLE_WAVPACK
    { "wv", "WavPack", probe_wavpack, create_wavpack, 8192 },
#endif
    /* 终止哨兵：ext == NULL */
    { NULL, NULL, NULL, NULL, 0 },
};

const audio_decoder_entry_t *audio_decoder_table(void)
{
    return g_decoders;
}

/** 把公共的 espaudiocore_format_t 映射到路由表条目（ringbuf 输入用，不做探测） */
const audio_decoder_entry_t *audio_decoder_for_format(espaudiocore_format_t fmt)
{
    const char *ext = NULL;
    switch (fmt) {
    case ESPAUDIOCORE_FMT_WAV:
        ext = "wav";
        break;
    case ESPAUDIOCORE_FMT_MP3:
        ext = "mp3";
        break;
    case ESPAUDIOCORE_FMT_FLAC:
        ext = "flac";
        break;
    case ESPAUDIOCORE_FMT_AAC:
        ext = "aac";
        break;
    case ESPAUDIOCORE_FMT_VORBIS:
        ext = "ogg";
        break;
    case ESPAUDIOCORE_FMT_OPUS:
        ext = "opus";
        break;
    case ESPAUDIOCORE_FMT_M4A:
        ext = "m4a";
        break;
    case ESPAUDIOCORE_FMT_WAVPACK:
        ext = "wv";
        break;
    default:
        return NULL;
    }
    for (int i = 0; g_decoders[i].ext; i++) {
        if (strcasecmp(ext, g_decoders[i].ext) == 0) {
            return &g_decoders[i];
        }
    }
    AUDIO_LOGW("no decoder entry for format %d (%s)", (int)fmt, ext);
    return NULL;
}

static const char *path_ext(const char *path)
{
    if (!path) {
        return NULL;
    }
    const char *dot = strrchr(path, '.');
    if (!dot || !dot[1]) {
        return NULL;
    }
    return dot + 1;
}

int audio_decoder_find_by_ext(const char *path)
{
    const char *ext = path_ext(path);
    if (!ext) {
        return -1;
    }
    for (int i = 0; g_decoders[i].ext; i++) {
        if (strcasecmp(ext, g_decoders[i].ext) == 0) {
            return i;
        }
    }
    return -1;
}

int audio_decoder_find_by_magic(AudioInput *in)
{
    if (!in) {
        return -1;
    }
    for (int i = 0; g_decoders[i].ext; i++) {
        if (g_decoders[i].probe && g_decoders[i].probe(in)) {
            return i;
        }
    }
    return -1;
}

const audio_decoder_entry_t *audio_decoder_route(AudioInput *in, const char *path)
{
    if (!in) {
        return NULL;
    }

    int idx = audio_decoder_find_by_ext(path);
    if (idx >= 0 && g_decoders[idx].create) {
        return &g_decoders[idx];
    }

    /* 扩展名未知或不匹配：退回魔数探测 */
    idx = audio_decoder_find_by_magic(in);
    if (idx >= 0 && g_decoders[idx].create) {
        if (in->can_seek()) {
            /* 探测过程用 peek，本不应改动位置；这里复位是为了稳妥 */
            (void)in->seek(0, SEEK_SET);
        }
        AUDIO_LOGI("routed by magic bytes -> %s", g_decoders[idx].name);
        return &g_decoders[idx];
    }

    AUDIO_LOGE("no decoder available for '%s'", path ? path : "(stream)");
    return NULL;
}
