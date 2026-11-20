/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 kanfandelong. All rights reserved.
 */

/**
 * @file    audio_source_fs.cpp
 * @brief   Local file source using plain POSIX I/O.
 *
 * Linux already provides page-cache read-ahead and a sane readahead window, so
 * there is no private read-ahead buffer here: read() maps to read(2) and
 * peek() uses pread(2), which leaves the file offset untouched.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "audio_port.h"
#include "audio_source.h"

#include <errno.h>
#include <fcntl.h>
#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

class AudioSourceFs : public AudioInput
{
public:
    ~AudioSourceFs() override
    {
        close();
    }

    bool open(const char *path)
    {
        if (!path || !path[0])
        {
            AUDIO_LOGE("empty file path");
            return false;
        }

        size_t path_len = strlen(path);
        path_copy_ = (char *)malloc(path_len + 1);
        if (!path_copy_)
        {
            return false;
        }
        memcpy(path_copy_, path, path_len + 1);
        this->path = path_copy_;

        fd_ = ::open(path, O_RDONLY | O_CLOEXEC);
        if (fd_ < 0)
        {
            AUDIO_LOGE("open('%s') failed: %s", path, strerror(errno));
            close();
            return false;
        }

        struct stat st;
        if (fstat(fd_, &st) == 0)
        {
            size_ = (int64_t)st.st_size;
        }
        else
        {
            size_ = -1;
        }
        AUDIO_LOGD("file source: %s (%lld bytes)", path, (long long)size_);
        return true;
    }

    void close()
    {
        if (fd_ >= 0)
        {
            ::close(fd_);
            fd_ = -1;
        }
        if (path_copy_)
        {
            this->path = nullptr;
            free(path_copy_);
            path_copy_ = nullptr;
        }
        consumed_ = 0;
        size_ = -1;
    }

    /* ---------------- AudioInput ---------------- */

    int read(void *dst, size_t want) override
    {
        if (fd_ < 0)
        {
            return -1;
        }
        size_t total = 0;
        while (total < want)
        {
            ssize_t n = ::read(fd_, (char *)dst + total, want - total);
            if (n < 0)
            {
                if (errno == EINTR)
                {
                    continue;
                }
                AUDIO_LOGE("read(2) failed: %s", strerror(errno));
                if (total == 0)
                {
                    return -1;
                }
                break;
            }
            if (n == 0)
            {
                break; /* EOF */
            }
            total += (size_t)n;
        }
        consumed_ += (int64_t)total;
        return (int)total;
    }

    /**
     * @brief Look ahead without moving the logical position.
     *
     * pread(2) reads from an absolute offset, so no lseek/restore dance is
     * needed. `need` is a hint for stream sources; a regular file either has
     * the bytes or hits EOF.
     */
    int peek(void *dst, size_t want, size_t need) override
    {
        (void)need;
        if (fd_ < 0)
        {
            return -1;
        }
        size_t total = 0;
        while (total < want)
        {
            ssize_t n = pread(fd_, (char *)dst + total, want - total,
                              (off_t)(consumed_ + (int64_t)total));
            if (n < 0)
            {
                if (errno == EINTR)
                {
                    continue;
                }
                break;
            }
            if (n == 0)
            {
                break;
            }
            total += (size_t)n;
        }
        return (int)total;
    }

    bool can_seek() const override
    {
        return fd_ >= 0;
    }

    int seek(int64_t pos, int whence) override
    {
        if (fd_ < 0)
        {
            return AUDIO_ERR_IO;
        }
        off_t np = lseek(fd_, (off_t)pos, whence);
        if (np < 0)
        {
            AUDIO_LOGE("lseek(%lld, %d) failed: %s", (long long)pos, whence, strerror(errno));
            return AUDIO_ERR_IO;
        }
        consumed_ = (int64_t)np;
        return AUDIO_OK;
    }

    int64_t tell() override
    {
        return consumed_;
    }

    int64_t size() const override
    {
        return size_;
    }

    int wait_data(uint32_t timeout_ms) override
    {
        (void)timeout_ms;
        return 1; /* files are always "ready"; 0 from read() is EOF */
    }

private:
    int     fd_ = -1;
    char   *path_copy_ = nullptr;
    int64_t consumed_ = 0;
    int64_t size_ = -1;
};

AudioInput *audio_source_fs_create(const char *path)
{
    AudioSourceFs *s = new (std::nothrow) AudioSourceFs();
    if (!s)
    {
        return nullptr;
    }
    if (!s->open(path))
    {
        delete s;
        return nullptr;
    }
    return s;
}
