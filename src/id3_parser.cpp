/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 kanfandelong. All rights reserved.
 */

/**
 * @file    id3_parser.cpp
 * @brief   ID3v2 标签解析。
 */

#include "id3_parser.h"
#include "audio_port.h"

#include <stdlib.h>
#include <string.h>

/** 允许的最大标签尺寸（防内存耗尽），与原实现一致 */
static const size_t MAX_ID3_SIZE = 384 * 1024;

/* ---------------------------------------------------------------------------
 * 基础读取辅助（移植自原实现）
 * -------------------------------------------------------------------------*/

static inline uint32_t big32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static inline uint32_t big24(const uint8_t *p)
{
    return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
}

/** synchsafe：每字节只用低 7 位 */
static inline uint32_t synch32(const uint8_t *p)
{
    return ((uint32_t)(p[0] & 0x7F) << 21) | ((uint32_t)(p[1] & 0x7F) << 14) |
           ((uint32_t)(p[2] & 0x7F) << 7) | (uint32_t)(p[3] & 0x7F);
}

/** ISO-8859-1 -> UTF-8 */
static char *copyLatin1ToUTF8(const uint8_t *src, int len)
{
    if (len <= 0) {
        return nullptr;
    }
    char *out = (char *)malloc((size_t)len * 2 + 1);
    if (!out) {
        return nullptr;
    }
    int j = 0;
    for (int i = 0; i < len; i++) {
        uint8_t c = src[i];
        if (c == 0) {
            break;
        }
        if (c < 0x80) {
            out[j++] = (char)c;
        } else {
            out[j++] = (char)(0xC0 | (c >> 6));
            out[j++] = (char)(0x80 | (c & 0x3F));
        }
    }
    out[j] = '\0';
    return out;
}

/** UTF-8 纯拷贝 */
static char *copyUTF8(const uint8_t *src, int len)
{
    if (len <= 0) {
        return nullptr;
    }
    char *out = (char *)malloc((size_t)len + 1);
    if (!out) {
        return nullptr;
    }
    memcpy(out, src, (size_t)len);
    out[len] = '\0';
    return out;
}

/**
 * UTF-16 -> UTF-8。encoding: 1 = UTF-16LE, 2 = UTF-16BE；BOM 优先。
 * 逻辑与原实现一致（含 BOM 决定字节序、无 BOM 时用传入 endian）。
 */
static char *copyUTF16ToUTF8(const uint8_t *src, int len, bool isBigEndian)
{
    if (len < 2) {
        return nullptr;
    }
    bool bom_found = false;
    bool bigEndian = isBigEndian;
    uint16_t bom = (uint16_t)(((uint16_t)src[0] << 8) | src[1]);
    if (bom == 0xFEFF) {
        bigEndian = true;
        bom_found = true;
    } else if (bom == 0xFFFE) {
        bigEndian = false;
        bom_found = true;
    }

    int start = bom_found ? 2 : 0;
    int maxChars = (len - start) / 2;
    char *out = (char *)malloc((size_t)maxChars * 3 + 1);
    if (!out) {
        return nullptr;
    }
    int j = 0;
    for (int i = start; i + 1 < len; i += 2) {
        uint32_t code;
        if (bigEndian) {
            code = ((uint32_t)src[i] << 8) | src[i + 1];
        } else {
            code = ((uint32_t)src[i + 1] << 8) | src[i];
        }
        if (code == 0) {
            break;
        }
        if (code < 0x80) {
            out[j++] = (char)code;
        } else if (code < 0x800) {
            out[j++] = (char)(0xC0 | (code >> 6));
            out[j++] = (char)(0x80 | (code & 0x3F));
        } else {
            out[j++] = (char)(0xE0 | (code >> 12));
            out[j++] = (char)(0x80 | ((code >> 6) & 0x3F));
            out[j++] = (char)(0x80 | (code & 0x3F));
        }
    }
    out[j] = '\0';
    return out;
}

static int skipUTF16Null(const uint8_t *buf, int max)
{
    for (int i = 0; i + 1 < max; i += 2) {
        if (buf[i] == 0 && buf[i + 1] == 0) {
            return i + 2;
        }
    }
    return -1;
}

static int skipLatinNull(const uint8_t *buf, int max)
{
    for (int i = 0; i < max; ++i) {
        if (buf[i] == 0) {
            return i + 1;
        }
    }
    return -1;
}

/** 按编码类型抽取字符串；enc: 0=Latin1, 1=UTF-16, 2=UTF-16BE, 3=UTF-8 */
static char *extractString(const uint8_t *data, int len, uint8_t enc, bool &isUnicode)
{
    if (len <= 0) {
        return nullptr;
    }
    isUnicode = false;
    switch (enc) {
    case 0:
        return copyLatin1ToUTF8(data, len);
    case 3:
        return copyUTF8(data, len);
    case 1:
    case 2:
        isUnicode = true;
        return copyUTF16ToUTF8(data, len, enc == 2);
    default:
        return copyLatin1ToUTF8(data, len);
    }
}

/* ---------------------------------------------------------------------------
 * 帧解析（移植自原实现，图片改为上报数据）
 * -------------------------------------------------------------------------*/

struct Id3Ctx {
    espaudiocore_meta_cb_t cb;
    void *user;
};

static void emit_text(Id3Ctx *c, const char *type, char *text, bool is_unicode)
{
    if (c->cb && text) {
        espaudiocore_meta_t m = {};
        m.type      = type;
        m.is_binary = false;
        m.data      = text;
        m.len       = strlen(text);
        m.mime      = nullptr;
        m.pic_type  = -1;
        c->cb(c->user, &m);
        (void)is_unicode;
    }
    free(text);
}

/**
 * @brief APIC/PIC 图片帧 -> meta 回调（is_binary = true）。
 *
 * 帧结构：encoding(1) + MIME(以 0 结尾，v2.2 是 3 字节图片格式) + picture type(1)
 *         + description(以 0 结尾，编码同 encoding) + 图片数据
 */
static void emit_picture(Id3Ctx *c, const uint8_t *d, int len, bool v2_2)
{
    if (!c->cb || len < 4) {
        return;
    }
    int pos = 0;
    uint8_t enc = d[pos++];
    const char *mime = nullptr;
    char mime_buf[32];

    if (v2_2) {
        /* v2.2 用 3 字节图片格式，如 "JPG"/"PNG" */
        if (len < pos + 3) {
            return;
        }
        const uint8_t *f = d + pos;
        snprintf(mime_buf, sizeof(mime_buf), "image/%.3s", (const char *)f);
        /* 转小写以便应用做 MIME 比较 */
        for (char *p = mime_buf; *p; p++) {
            if (*p >= 'A' && *p <= 'Z') {
                *p = (char)(*p - 'A' + 'a');
            }
        }
        mime = mime_buf;
        pos += 3;
    } else {
        /* v2.3/2.4：MIME 是以 0 结尾的 Latin-1 字符串 */
        int mlen = 0;
        while (pos + mlen < len && d[pos + mlen] != 0) {
            mlen++;
        }
        if (mlen > 0 && mlen < (int)sizeof(mime_buf) - 1) {
            memcpy(mime_buf, d + pos, (size_t)mlen);
            mime_buf[mlen] = '\0';
            mime = mime_buf;
        }
        pos += mlen + 1; /* 跳过 MIME 与终止符 */
    }

    if (pos >= len) {
        return;
    }
    int pic_type = d[pos++];

    /* 跳过 description */
    int skip;
    if (enc == 1 || enc == 2) {
        skip = skipUTF16Null(d + pos, len - pos);
    } else {
        skip = skipLatinNull(d + pos, len - pos);
    }
    if (skip >= 0) {
        pos += skip;
    }
    if (pos >= len) {
        return;
    }

    espaudiocore_meta_t m = {};
    m.type      = "APIC";
    m.is_binary = true;
    m.data      = (const char *)(d + pos);
    m.len       = (size_t)(len - pos);
    m.mime      = mime;
    m.pic_type  = pic_type;
    c->cb(c->user, &m);

    AUDIO_LOGD("ID3 picture: %s, type=%d, %u bytes", mime ? mime : "?", pic_type,
               (unsigned)m.len);
}

static void parse_frames(Id3Ctx *c, const uint8_t *buf, size_t len, uint8_t version)
{
    size_t pos = 0;
    bool v2_2 = (version == 2);

    while (pos < len) {
        /* 填充区检测 */
        if (len - pos >= 4 && buf[pos] == 0 && buf[pos + 1] == 0 && buf[pos + 2] == 0 &&
            buf[pos + 3] == 0) {
            break;
        }

        char frameId[5] = {0};
        uint32_t frameSize = 0;
        int headerLen = 0;
        bool compressed = false;

        if (v2_2) {
            if (pos + 6 > len) {
                break;
            }
            memcpy(frameId, buf + pos, 3);
            frameId[3] = '\0';
            frameSize = big24(buf + pos + 3);
            headerLen = 6;
        } else {
            if (pos + 10 > len) {
                break;
            }
            memcpy(frameId, buf + pos, 4);
            frameId[4] = '\0';
            /* v2.4 用 synchsafe，v2.3 用普通 big-endian */
            frameSize = (version == 4) ? synch32(buf + pos + 4) : big32(buf + pos + 4);
            uint8_t flag2 = buf[pos + 9];
            compressed = (flag2 & 0x80) != 0;
            headerLen = 10;
        }
        pos += (size_t)headerLen;

        if (frameSize == 0) {
            continue;
        }
        if (compressed) {
            pos += 4;
            if (pos + frameSize > len) {
                break;
            }
            pos += frameSize - 4;
            continue;
        }
        if (pos + frameSize > len) {
            break;
        }

        const uint8_t *frameData = buf + pos;
        size_t dataLen = frameSize;

        /* ---- 图片帧：上报数据（原实现是跳过） ---- */
        if (strcmp(frameId, "APIC") == 0 || (v2_2 && strcmp(frameId, "PIC") == 0)) {
            emit_picture(c, frameData, (int)dataLen, v2_2);
            pos += dataLen;
            continue;
        }

        /* ---- 歌词帧 ---- */
        if (strcmp(frameId, "SYLT") == 0 || strcmp(frameId, "USLT") == 0 ||
            (v2_2 && strcmp(frameId, "SLT") == 0)) {
            if (dataLen < 4) {
                pos += dataLen;
                continue;
            }
            uint8_t enc = frameData[0];
            int consumed = 4; /* encoding(1) + language(3) */
            if (enc == 1 || enc == 2) {
                int skip = skipUTF16Null(frameData + consumed, (int)dataLen - consumed);
                consumed = (skip >= 0) ? consumed + skip : (int)dataLen;
            } else {
                int skip = skipLatinNull(frameData + consumed, (int)dataLen - consumed);
                consumed = (skip >= 0) ? consumed + skip : (int)dataLen;
            }
            int lyricLen = (int)dataLen - consumed;
            if (lyricLen < 0) {
                lyricLen = 0;
            }
            if (lyricLen > 10240) {
                lyricLen = 10240;
            }
            bool isUnicode = false;
            char *lyrics = extractString(frameData + consumed, lyricLen, enc, isUnicode);
            emit_text(c, frameId, lyrics, isUnicode);
            pos += dataLen;
            continue;
        }

        /* ---- 评论帧 ---- */
        if (strcmp(frameId, "COMM") == 0 || (v2_2 && strcmp(frameId, "COM") == 0)) {
            if (dataLen < 4) {
                pos += dataLen;
                continue;
            }
            uint8_t enc = frameData[0];
            int consumed = 4;
            if (enc == 1 || enc == 2) {
                int skip = skipUTF16Null(frameData + consumed, (int)dataLen - consumed);
                consumed = (skip >= 0) ? consumed + skip : (int)dataLen;
            } else {
                int skip = skipLatinNull(frameData + consumed, (int)dataLen - consumed);
                consumed = (skip >= 0) ? consumed + skip : (int)dataLen;
            }
            int textLen = (int)dataLen - consumed;
            if (textLen > 0) {
                bool isUnicode = false;
                char *text = extractString(frameData + consumed, textLen, enc, isUnicode);
                emit_text(c, "COMM", text, isUnicode);
            }
            pos += dataLen;
            continue;
        }

        /* ---- 自定义帧 TXXX ---- */
        if (strcmp(frameId, "TXXX") == 0 || (v2_2 && strcmp(frameId, "TXX") == 0)) {
            if (dataLen < 1) {
                pos += dataLen;
                continue;
            }
            uint8_t enc = frameData[0];
            int consumed = 1;
            if (enc == 1 || enc == 2) {
                int skip = skipUTF16Null(frameData + consumed, (int)dataLen - consumed);
                consumed = (skip >= 0) ? consumed : (int)dataLen;
                if (skip >= 0) {
                    consumed = 1 + skip;
                }
            } else {
                int skip = skipLatinNull(frameData + consumed, (int)dataLen - consumed);
                consumed = (skip >= 0) ? 1 + skip : (int)dataLen;
            }
            int textLen = (int)dataLen - consumed;
            if (textLen > 10240) {
                textLen = 10240;
            }
            if (textLen > 0) {
                bool isUnicode = false;
                char *text = extractString(frameData + consumed, textLen, enc, isUnicode);
                emit_text(c, frameId, text, isUnicode);
            }
            pos += dataLen;
            continue;
        }

        /* ---- 普通文本帧 ---- */
        if (dataLen < 2) {
            pos += dataLen;
            continue;
        }
        uint8_t enc = frameData[0];
        int textLen = (int)dataLen - 1;
        bool isUnicode = false;
        char *text = extractString(frameData + 1, textLen, enc, isUnicode);
        emit_text(c, frameId, text, isUnicode);
        pos += dataLen;
    }
}

/* ---------------------------------------------------------------------------
 * 入口
 * -------------------------------------------------------------------------*/

size_t id3_parse_and_report(AudioInput *in, espaudiocore_meta_cb_t meta_cb, void *meta_user)
{
    if (!in) {
        return 0;
    }

    uint8_t header[10];
    int got = in->read(header, sizeof(header));
    if (got < 10 || memcmp(header, "ID3", 3) != 0) {
        /* 没有 ID3：把读掉的字节退回去（仅当可 seek） */
        if (got > 0 && in->can_seek()) {
            (void)in->seek(0, SEEK_SET);
        } else if (got > 0) {
            /* 流式源无法回退：这是设计限制，需要调用者注意 */
            AUDIO_LOGW("non-seekable source: %d bytes consumed while probing ID3", got);
        }
        return 0;
    }

    uint8_t version = header[3];
    if (version < 2 || version > 4 || header[4] != 0) {
        /* 非法或非 2.x：回退 */
        if (in->can_seek()) {
            (void)in->seek(0, SEEK_SET);
        }
        return 0;
    }

    bool unsync = (header[5] & 0x80) != 0;
    bool exthdr = (version >= 3) && (header[5] & 0x40);
    uint32_t rawSize = synch32(header + 6);
    uint32_t id3Size = rawSize + 10;

    /* 超大标签：直接跳过，不分配 */
    if (id3Size > MAX_ID3_SIZE) {
        AUDIO_LOGW("ID3 tag too large (%u bytes), skipping", (unsigned)id3Size);
        uint32_t remain = id3Size - 10;
        uint8_t sink[256];
        while (remain > 0) {
            uint32_t chunk = (remain < sizeof(sink)) ? remain : sizeof(sink);
            int n = in->read(sink, chunk);
            if (n <= 0) {
                break;
            }
            remain -= (uint32_t)n;
        }
        return id3Size;
    }

    /* 一次性读入整个标签；优先 PSRAM，解析完立即释放 */
    uint8_t *tag = (uint8_t *)audio_alloc_big(id3Size, 0);
    if (!tag) {
        AUDIO_LOGW("no memory for ID3 tag (%u bytes), skipping", (unsigned)id3Size);
        uint32_t remain = id3Size - 10;
        uint8_t sink[256];
        while (remain > 0) {
            uint32_t chunk = (remain < sizeof(sink)) ? remain : sizeof(sink);
            int n = in->read(sink, chunk);
            if (n <= 0) {
                break;
            }
            remain -= (uint32_t)n;
        }
        return id3Size;
    }

    memcpy(tag, header, 10);
    uint32_t total = 10;
    uint32_t toRead = id3Size - 10;
    while (toRead > 0) {
        int n = in->read(tag + total, toRead);
        if (n <= 0) {
            break;
        }
        total += (uint32_t)n;
        toRead -= (uint32_t)n;
    }

    /* 去同步（原实现逻辑） */
    if (unsync && total > 10) {
        size_t bodyLen = total - 10;
        size_t newBody = 0;
        for (size_t i = 0; i < bodyLen; ++i) {
            uint8_t ch = tag[10 + i];
            tag[10 + newBody++] = ch;
            if (ch == 0xFF && i + 1 < bodyLen && tag[10 + i + 1] == 0x00) {
                ++i;
            }
        }
        total = (uint32_t)(10 + newBody);
    }

    /* 跳过扩展头 */
    size_t bodyOffset = 10;
    size_t bodyLength = (total > 10) ? (total - 10) : 0;
    if (exthdr && bodyLength >= 4) {
        uint32_t ehsize = synch32(tag + bodyOffset);
        if (ehsize >= 4 && ehsize <= bodyLength) {
            bodyOffset += ehsize;
            bodyLength -= ehsize;
        }
    }

    Id3Ctx ctx = {meta_cb, meta_user};
    parse_frames(&ctx, tag + bodyOffset, bodyLength, version);

    /* 标签结束标记（沿用原实现的约定） */
    if (meta_cb) {
        espaudiocore_meta_t m = {};
        m.type      = "eof";
        m.is_binary = false;
        m.data      = "id3";
        m.len       = 3;
        m.pic_type  = -1;
        meta_cb(meta_user, &m);
    }

    AUDIO_LOGI("ID3v%u parsed: %u bytes, unsync=%s", (unsigned)version, (unsigned)total,
               unsync ? "yes" : "no");

    audio_free(tag);
    return id3Size;
}
