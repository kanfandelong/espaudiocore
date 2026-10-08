/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 kanfandelong. All rights reserved.
 */

/**
 * @file    xing_parser.cpp
 * @brief   Xing / Info 头解析。
 */

#include "xing_parser.h"
#include "audio_port.h"

XingHeaderInfo parseXingHeader(const uint8_t *buf, size_t len)
{
    XingHeaderInfo info;
    if (!buf || len < 4) {
        AUDIO_LOGD("Xing: buffer too short");
        return info;
    }
    if ((buf[0] != 0xFF) || ((buf[1] & 0xE0) != 0xE0)) {
        AUDIO_LOGD("Xing: no sync word");
        return info;
    }

    uint8_t b1 = buf[1];
    uint8_t b2 = buf[2];
    uint8_t b3 = buf[3];

    int version = (b1 >> 3) & 0x03; /* 0=MPEG2.5, 2=MPEG2, 3=MPEG1 */
    int layer = (b1 >> 1) & 0x03;   /* 3=L1, 2=L2, 1=L3 */
    int protection = b1 & 0x01;     /* 0=有 CRC, 1=无 */
    int bitrate_index = (b2 >> 4) & 0x0F;
    int samplerate_index = (b2 >> 2) & 0x03;
    int padding = (b2 >> 1) & 0x01;
    int mode = (b3 >> 6) & 0x03; /* 0=stereo, 3=mono */

    static const int samplerates[4][3] = {
        {11025, 12000, 8000}, /* MPEG2.5 */
        {0, 0, 0},
        {22050, 24000, 16000}, /* MPEG2 */
        {44100, 48000, 32000}, /* MPEG1 */
    };
    int sample_rate = samplerates[version][samplerate_index];
    if (!sample_rate) {
        AUDIO_LOGW("Xing: invalid sample rate index");
        return info;
    }

    static const int spf_table[4][4] = {
        {0, 576, 1152, 384}, /* MPEG2.5 */
        {0, 0, 0, 0},
        {0, 576, 1152, 384}, /* MPEG2 */
        {0, 1152, 1152, 384},/* MPEG1 */
    };
    int spf = spf_table[version][layer];
    if (!spf) {
        AUDIO_LOGW("Xing: invalid layer");
        return info;
    }

    static const uint16_t bitrates[2][3][16] = {
        { /* MPEG1 */
          {0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448, 0},
          {0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 0},
          {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0} },
        { /* MPEG2/2.5 */
          {0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256, 0},
          {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0},
          {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0} },
    };
    int mpeg1 = (version == 3) ? 0 : 1;
    int layer_idx = layer - 1;
    if (layer_idx < 0 || layer_idx > 2) {
        return info;
    }
    uint32_t bitrate_kbps = bitrates[mpeg1][layer_idx][bitrate_index];
    if (!bitrate_kbps) {
        AUDIO_LOGW("Xing: invalid bitrate index");
        return info;
    }

    int frame_size = (spf * bitrate_kbps * 1000) / (sample_rate * 8);
    if (padding) {
        frame_size++;
    }
    if (frame_size <= 0 || frame_size > (int)len) {
        AUDIO_LOGD("Xing: frame_size out of range (%d > %u)", frame_size, (unsigned)len);
        return info;
    }

    int side_info_size = (version == 3) ? ((mode == 3) ? 17 : 32) : ((mode == 3) ? 9 : 17);
    int xing_offset = 4 + ((protection == 0) ? 2 : 0) + side_info_size;
    if (xing_offset + 8 > frame_size) {
        AUDIO_LOGD("Xing: offset beyond frame");
        return info;
    }

    const char *tag = (const char *)(buf + xing_offset);
    bool is_xing = (tag[0] == 'X' && tag[1] == 'i' && tag[2] == 'n' && tag[3] == 'g');
    bool is_info = (tag[0] == 'I' && tag[1] == 'n' && tag[2] == 'f' && tag[3] == 'o');
    if (!is_xing && !is_info) {
        AUDIO_LOGD("Xing: no Xing/Info tag at offset %d", xing_offset);
        return info;
    }

    uint32_t flags = ((uint32_t)buf[xing_offset + 4] << 24) |
                     ((uint32_t)buf[xing_offset + 5] << 16) |
                     ((uint32_t)buf[xing_offset + 6] << 8) | (uint32_t)buf[xing_offset + 7];
    int data_pos = xing_offset + 8;

    if (flags & 0x0001) {
        if (data_pos + 4 > frame_size) {
            return info;
        }
        info.frames = ((uint32_t)buf[data_pos] << 24) | ((uint32_t)buf[data_pos + 1] << 16) |
                      ((uint32_t)buf[data_pos + 2] << 8) | (uint32_t)buf[data_pos + 3];
        data_pos += 4;
    }
    if (flags & 0x0002) {
        if (data_pos + 4 > frame_size) {
            return info;
        }
        info.bytes = ((uint32_t)buf[data_pos] << 24) | ((uint32_t)buf[data_pos + 1] << 16) |
                     ((uint32_t)buf[data_pos + 2] << 8) | (uint32_t)buf[data_pos + 3];
        data_pos += 4;
    }
    if (flags & 0x0004) {
        data_pos += 100; /* TOC */
    }
    if (flags & 0x0008) {
        data_pos += 4; /* quality */
    }

    info.valid = true;
    info.sampleRate = sample_rate;
    info.channels = (mode == 3) ? 1 : 2;

    if (info.frames && spf && sample_rate) {
        info.duration = (float)info.frames * (float)spf / (float)sample_rate;
        if (info.bytes && info.duration > 0.0f) {
            info.bitrate = (uint32_t)((double)info.bytes * 8.0 / (double)info.duration);
        } else {
            info.bitrate = bitrate_kbps * 1000;
        }
    }
    return info;
}
