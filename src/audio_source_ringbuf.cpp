/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 kanfandelong. All rights reserved.
 */

/**
 * @file    audio_source_ringbuf.c
 * @brief   Ring-buffer source: a pure sequential stream with no seek.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "audio_port.h"
#include "audio_ringbuf.h"
#include "audio_source.h"

#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** Look-ahead capacity: enough for ID3 headers and magic probing. */
#define RINGBUF_LOOKAHEAD_SIZE 4096

/** Look-ahead wait budget (ms). Streaming data arrives asynchronously; peek
 *  gives it a short window instead of failing immediately. It must stay small
 *  because the decoder thread sleeps inside it and stop() waits for the
 *  thread to notice the stop flag. */
#define LOOKAHEAD_WAIT_MS 300

/** First-read wait budget (ms): a 0-byte read is treated as EOF by libFLAC and
 *  friends, so do not mistake "producer is not ready yet" for end of stream. */
#define READ_WAIT_MS 300

class AudioSourceRingbuf : public AudioInput
{
public:
    explicit AudioSourceRingbuf(espaudiocore_ringbuf_t *rb) : rb_(rb)
    {
    }

    ~AudioSourceRingbuf() override
    {
        close();
    }

    bool init()
    {
        if (!rb_)
        {
            return false;
        }
        look_ = (uint8_t *)audio_alloc_big(RINGBUF_LOOKAHEAD_SIZE, 0);
        if (!look_)
        {
            return false;
        }
        look_cap_ = RINGBUF_LOOKAHEAD_SIZE;
        return true;
    }

    void close()
    {
        if (look_)
        {
            audio_free(look_);
            look_ = nullptr;
        }
        rb_ = nullptr;
        look_len_ = look_pos_ = 0;
    }

    /* ---------------- AudioInput ---------------- */

    int read(void *dst, size_t want) override
    {
        uint8_t *out = (uint8_t *)dst;
        size_t total = 0;

        /* 1. Consume the look-ahead cache first. */
        if (look_pos_ < look_len_)
        {
            size_t avail = look_len_ - look_pos_;
            size_t take = (want < avail) ? want : avail;
            memcpy(out, look_ + look_pos_, take);
            look_pos_ += take;
            total += take;
            want -= take;
            if (look_pos_ >= look_len_)
            {
                look_len_ = look_pos_ = 0;
            }
        }

        /* 2. Then pull from the ring buffer. */
        while (want > 0)
        {
            int wait = (total == 0) ? READ_WAIT_MS : 0;
            size_t got = espaudiocore_ringbuf_read(rb_, out + total, want, wait);
            if (got == 0)
            {
                break; /* no data within the window; upper layer decides EOF */
            }
            total += got;
            want -= got;
        }

        consumed_ += (int64_t)total;
        return (int)total;
    }

    int peek(void *dst, size_t want, size_t need) override
    {
        /* Return as much as available up to `want`, trying to guarantee at
         * least `need`. */
        size_t target = (want < look_cap_) ? want : look_cap_;
        if (target < need)
        {
            target = (need < look_cap_) ? need : look_cap_;
        }

        (void)ensure_lookahead(target);

        size_t avail = look_len_ - look_pos_;
        size_t give = (avail < target) ? avail : target;
        if (give)
        {
            memcpy(dst, look_ + look_pos_, give);
        }
        return (int)give;
    }

    bool can_seek() const override
    {
        return false;
    }

    int seek(int64_t pos, int whence) override
    {
        (void)pos;
        (void)whence;
        return AUDIO_ERR_NOT_SUPPORTED;
    }

    int64_t tell() override
    {
        return consumed_;
    }

    int64_t size() const override
    {
        return -1;
    }

    int wait_data(uint32_t timeout_ms) override
    {
        if (look_pos_ < look_len_)
        {
            return 1;
        }
        uint8_t byte = 0;
        size_t got = espaudiocore_ringbuf_read(rb_, &byte, 1, (int)timeout_ms);
        if (got == 0)
        {
            return 0;
        }
        look_len_ = look_pos_ = 0;
        look_[0] = byte;
        look_len_ = 1;
        return 1;
    }

private:
    /**
     * @brief Ensure the look-ahead cache holds at least `need` bytes.
     *
     * Streaming data arrives asynchronously, so wait a bounded amount instead
     * of only inspecting what is currently queued.
     */
    bool ensure_lookahead(size_t need)
    {
        if (look_pos_ > 0)
        {
            size_t avail = look_len_ - look_pos_;
            if (avail && look_pos_ > 0)
            {
                memmove(look_, look_ + look_pos_, avail);
            }
            look_len_ = avail;
            look_pos_ = 0;
        }

        int waited = 0;
        while (look_len_ < need && look_len_ < look_cap_)
        {
            size_t space = look_cap_ - look_len_;
            size_t got = espaudiocore_ringbuf_read(rb_, look_ + look_len_, space, 50);
            if (got == 0)
            {
                waited += 50;
                if (waited >= LOOKAHEAD_WAIT_MS)
                {
                    break;
                }
                continue;
            }
            look_len_ += got;
        }
        return look_len_ >= need;
    }

    espaudiocore_ringbuf_t *rb_ = nullptr;
    uint8_t *look_ = nullptr;
    size_t look_cap_ = 0;
    size_t look_len_ = 0;
    size_t look_pos_ = 0;
    int64_t consumed_ = 0;
};

AudioInput *audio_source_ringbuf_create(espaudiocore_ringbuf_t *rb)
{
    AudioSourceRingbuf *s = new (std::nothrow) AudioSourceRingbuf(rb);
    if (!s)
    {
        return nullptr;
    }
    if (!s->init())
    {
        delete s;
        return nullptr;
    }
    return s;
}
