/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 kanfandelong. All rights reserved.
 */

/**
 * @file    audio_picture.cpp
 * @brief   Cover-art parsing shared by FLAC, Vorbis and Opus.
 */

#include "audio_picture.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "audio_port.h"

namespace {

/** Keep a malformed tag from exhausting PSRAM.  Real covers are far smaller. */
constexpr size_t kMaxPictureBytes = 4u * 1024u * 1024u;

int base64_value(unsigned char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

uint8_t *base64_decode_alloc(const char *src, size_t len, size_t *out_len)
{
    if (!src || !out_len) {
        return nullptr;
    }

    /* 4 base64 characters -> 3 bytes.  Reject absurd inputs before allocating. */
    if (len > (kMaxPictureBytes * 4u / 3u) + 64u) {
        AUDIO_LOGW("picture base64 too large (%u bytes)", (unsigned)len);
        return nullptr;
    }

    size_t cap = (len / 4u + 1u) * 3u;
    if (cap > kMaxPictureBytes + 3u) {
        cap = kMaxPictureBytes + 3u;
    }
    uint8_t *out = (uint8_t *)audio_alloc_big(cap, 0);
    if (!out) {
        return nullptr;
    }

    uint32_t acc = 0;
    unsigned bits = 0;
    size_t n = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v') {
            continue;
        }
        if (c == '=') {
            break; /* padding; the accumulated bits are flushed below */
        }
        int v = base64_value(c);
        if (v < 0) {
            AUDIO_LOGW("invalid base64 character 0x%02x in picture", c);
            audio_free(out);
            return nullptr;
        }
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (n >= cap) {
                audio_free(out);
                return nullptr;
            }
            out[n++] = (uint8_t)((acc >> bits) & 0xFFu);
        }
    }

    if (n == 0) {
        audio_free(out);
        return nullptr;
    }
    *out_len = n;
    return out;
}

bool emit_picture(const uint8_t *data, size_t len, const char *mime, int pic_type,
                  espaudiocore_meta_cb_t cb, void *user)
{
    if (!cb || !data || len < 4) {
        return false;
    }

    const char *actual = audio_picture_mime(data, len);
    if (!actual) {
        AUDIO_LOGW("picture format not recognized (%u bytes, mime=%s)", (unsigned)len,
                   mime ? mime : "?");
        return false;
    }

    /* Ignore URL-only picture blocks and formats the display cannot draw. */
    if (mime && strcmp(mime, "-->") == 0) {
        return false;
    }

    espaudiocore_meta_t m = {};
    m.type = "PICTURE";
    m.is_binary = true;
    m.data = (const char *)data;
    m.len = len;
    m.mime = actual;
    m.pic_type = pic_type;
    cb(user, &m);

    AUDIO_LOGD("picture: %s type=%d %u bytes", actual, pic_type, (unsigned)len);
    return true;
}

} // namespace

const char *audio_picture_mime(const uint8_t *data, size_t len)
{
    static const uint8_t png_sig[] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};

    if (!data) {
        return nullptr;
    }
    if (len >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF) {
        return "image/jpeg";
    }
    if (len >= sizeof(png_sig) && memcmp(data, png_sig, sizeof(png_sig)) == 0) {
        return "image/png";
    }
    if (len >= 6 && (memcmp(data, "GIF87a", 6) == 0 || memcmp(data, "GIF89a", 6) == 0)) {
        return "image/gif";
    }
    if (len >= 12 && memcmp(data, "RIFF", 4) == 0 && memcmp(data + 8, "WEBP", 4) == 0) {
        return "image/webp";
    }
    if (len >= 2 && data[0] == 'B' && data[1] == 'M') {
        return "image/bmp";
    }
    return nullptr;
}

bool audio_emit_flac_picture(const uint8_t *data, size_t len,
                             espaudiocore_meta_cb_t cb, void *user)
{
    if (!data || len < 32) {
        return false;
    }

    size_t pos = 0;
    uint32_t pic_type = audio_rd_be32(data + pos);
    pos += 4;

    if (pos + 4 > len) {
        return false;
    }
    uint32_t mime_len = audio_rd_be32(data + pos);
    pos += 4;
    if (mime_len > len - pos) {
        return false;
    }
    const uint8_t *mime_ptr = data + pos;
    pos += mime_len;

    if (pos + 4 > len) {
        return false;
    }
    uint32_t desc_len = audio_rd_be32(data + pos);
    pos += 4;
    if (desc_len > len - pos) {
        return false;
    }
    pos += desc_len;

    /* width + height + depth + colors */
    if (pos + 16 > len) {
        return false;
    }
    pos += 16;

    if (pos + 4 > len) {
        return false;
    }
    uint32_t image_len = audio_rd_be32(data + pos);
    pos += 4;
    if (image_len == 0 || image_len > len - pos) {
        return false;
    }

    char mime_buf[48];
    const char *mime = nullptr;
    if (mime_len > 0) {
        size_t copy = mime_len;
        if (copy >= sizeof(mime_buf)) {
            copy = sizeof(mime_buf) - 1;
        }
        memcpy(mime_buf, mime_ptr, copy);
        mime_buf[copy] = '\0';
        mime = mime_buf;
    }

    return emit_picture(data + pos, image_len, mime, (int)pic_type, cb, user);
}

bool audio_emit_picture_comment(const char *key, const char *value, size_t len,
                                espaudiocore_meta_cb_t cb, void *user)
{
    if (!key) {
        return false;
    }

    bool block = strcasecmp(key, "METADATA_BLOCK_PICTURE") == 0;
    bool coverart = strcasecmp(key, "COVERART") == 0;
    bool coverart_mime = strcasecmp(key, "COVERARTMIME") == 0;
    if (!block && !coverart && !coverart_mime) {
        return false;
    }

    /* COVERARTMIME is metadata for the next COVERART; the image signature is
     * enough, so consume the value without reporting it as text. */
    if (coverart_mime || !value || len == 0) {
        return true;
    }

    size_t decoded_len = 0;
    uint8_t *decoded = base64_decode_alloc(value, len, &decoded_len);
    if (!decoded) {
        return true;
    }

    if (block) {
        (void)audio_emit_flac_picture(decoded, decoded_len, cb, user);
    } else {
        (void)emit_picture(decoded, decoded_len, nullptr, 3, cb, user);
    }
    audio_free(decoded);
    return true;
}
