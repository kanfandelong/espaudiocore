/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 kanfandelong. All rights reserved.
 */

/**
 * @file    audio_source_fs.cpp
 * @brief   本地文件数据源。
 */

#include <errno.h>
#include <stdio.h>
#include <new>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "audio_port.h"
#include "audio_source.h"
#include "sdkconfig.h"

/* 模式选择：Kconfig 是默认来源；也允许用编译定义强制覆盖，
 * 便于一次构建里对照验证两条路径（见组件 CMakeLists 的 *_VFS_TEST 选项）。 */
#if defined(ESPAUDIOCORE_FORCE_FILE_SOURCE_VFS)
#define FS_USE_VFS 1
#elif defined(ESPAUDIOCORE_FORCE_FILE_SOURCE_WINDOW)
#define FS_USE_VFS 0
#elif defined(CONFIG_ESPAUDIOCORE_FILE_SOURCE_VFS)
#define FS_USE_VFS 1
#else
#define FS_USE_VFS 0
#endif

#ifndef CONFIG_ESPAUDIOCORE_SOURCE_BUFFER_SIZE
#define CONFIG_ESPAUDIOCORE_SOURCE_BUFFER_SIZE 32768
#endif
#ifndef CONFIG_ESPAUDIOCORE_VFS_BUFFER_SIZE
#define CONFIG_ESPAUDIOCORE_VFS_BUFFER_SIZE 65536
#endif

#define WIN_BUF_SIZE ((size_t)CONFIG_ESPAUDIOCORE_SOURCE_BUFFER_SIZE)
#define VFS_BUF_SIZE ((size_t)CONFIG_ESPAUDIOCORE_VFS_BUFFER_SIZE)

class AudioSourceFs : public AudioInput {
public:
    ~AudioSourceFs() override
    {
        close();
    }

    bool open(const char *path)
    {
        if (!path || path[0] != '/') {
            AUDIO_LOGE("path must be absolute (start with '/'): %s", path ? path : "(null)");
            return false;
        }

        fp_ = fopen(path, "rb");
        if (!fp_) {
            AUDIO_LOGE("fopen failed: %s (errno=%d)", path, errno);
            return false;
        }

#if FS_USE_VFS
        /* 纯 VFS 模式：把 stdio 自己的缓冲指到 PSRAM，不做自管理窗口。
         * 必须在任何其它 I/O 之前设置。缓冲由我们 free，不借给 stdio 释放。 */
        size_t want = VFS_BUF_SIZE;
        vfs_buf_ = (char *)audio_alloc_big(want, 0);
        if (!vfs_buf_) {
            AUDIO_LOGE("no memory for %u-byte setvbuf buffer", (unsigned)want);
            fclose(fp_);
            fp_ = nullptr;
            return false;
        }
        if (setvbuf(fp_, vfs_buf_, _IOFBF, want) != 0) {
            AUDIO_LOGW("setvbuf failed, falling back to default stdio buffer");
            audio_free(vfs_buf_);
            vfs_buf_ = nullptr;
        } else {
            AUDIO_LOGD("file source: VFS mode, setvbuf %u bytes (PSRAM preferred)",
                       (unsigned)want);
        }
#else
        /* 窗口模式：自己管理缓冲，关掉 stdio 内部缓冲避免双重缓冲 */
        size_t want = WIN_BUF_SIZE;
        buf_ = (uint8_t *)audio_alloc_big(want, 0);
        if (!buf_) {
            AUDIO_LOGE("no memory for %u-byte read-ahead buffer", (unsigned)want);
            fclose(fp_);
            fp_ = nullptr;
            return false;
        }
        cap_ = want;
        setvbuf(fp_, nullptr, _IONBF, 0);
        AUDIO_LOGD("file source: window mode, %u bytes", (unsigned)want);
#endif

        /* 一次性取大小，避免每次 size() 都 seek 到结尾 */
        struct stat st;
        if (fstat(fileno(fp_), &st) == 0) {
            size_ = (int64_t)st.st_size;
        } else {
            size_ = -1;
        }

#if !FS_USE_VFS
        /* 预读第一窗，让 probe/ID3 立刻可用 */
        (void)refill();
#endif
        return true;
    }

    void close()
    {
        if (fp_) {
            fclose(fp_);
            fp_ = nullptr;
        }
        if (buf_) {
            audio_free(buf_);
            buf_ = nullptr;
        }
        if (vfs_buf_) {
            /* fclose 已完成，此时才能释放 setvbuf 的缓冲 */
            audio_free(vfs_buf_);
            vfs_buf_ = nullptr;
        }
        cap_ = len_ = pos_ = 0;
        consumed_ = 0;
        eof_ = false;
    }

    /* ---------------- AudioInput ---------------- */

    int read(void *dst, size_t want) override
    {
#if FS_USE_VFS
        if (!fp_) {
            return -1;
        }
        size_t got = fread(dst, 1, want, fp_);
        consumed_ += got;
        return (int)got;
#else
        uint8_t *out = (uint8_t *)dst;
        size_t total = 0;

        while (want > 0) {
            if (pos_ >= len_) {
                /* 窗口空了：只有 eof_ 为真才是真的结束
                 * （refill() 返回 0 也可能是"窗口满"，此时数据仍可交付） */
                if (eof_) {
                    break;
                }
                (void)refill();
                if (pos_ >= len_) {
                    break; /* 确实没有更多数据 */
                }
            }
            size_t avail = len_ - pos_;
            size_t take = (want < avail) ? want : avail;
            memcpy(out + total, buf_ + pos_, take);
            pos_ += take;
            total += take;
            want -= take;
        }

        consumed_ += total;
        return (int)total;
#endif
    }

    /**
     * @brief 前瞻：最多返回 want 字节，且**至少**保证能返回 need 字节（若文件足够长）。
     *
     * 关键点（曾经的 bug）：不能只依赖内部窗口凑数据。窗口一次最多装 cap_ 字节，
     * 若 need > 窗口可提供的量，就必须**临时消费式读取**把 dst 填满，再 seek 回原位。
     * 否则调用者（如 WAV 的 chunk 扫描、ID3 的整标签读取）会拿到一部分数据
     * 加一片未初始化的栈垃圾，解析必然失败。
     */
    int peek(void *dst, size_t want, size_t need) override
    {
#if FS_USE_VFS
        /* 纯 VFS 模式：保存位置 -> 读 -> 复位。
         * 底层就是同一个 stdio 缓冲，fseeko 到原位不会再触发真正的设备读。 */
        if (!fp_) {
            return -1;
        }
        off_t here = ftello(fp_);
        if (here < 0) {
            return -1;
        }
        size_t got = fread(dst, 1, want, fp_);
        (void)fseeko(fp_, here, SEEK_SET);
        return (int)got;
#else
        if (!fp_) {
            return -1;
        }

        /* 先把窗口内已有的数据吐出去（不移动窗口游标） */
        size_t supplied = 0;
        if (len_ > pos_) {
            size_t avail = len_ - pos_;
            size_t take = (want < avail) ? want : avail;
            memcpy(dst, buf_ + pos_, take);
            supplied = take;
        }

        /* 还差就临时读一段，然后复位（只在可 seek 的源上做） */
        if (need > supplied && can_seek()) {
            int64_t here = consumed_; /* 逻辑位置 */
            size_t left = want - supplied;
            size_t g = 0;
            while (g < left) {
                int n = read((uint8_t *)dst + supplied + g, left - g);
                if (n <= 0) {
                    break;
                }
                g += (size_t)n;
            }
            (void)seek(here, SEEK_SET); /* 复位到原逻辑位置 */
            supplied += g;
        }
        return (int)supplied;
#endif
    }

    bool can_seek() const override
    {
        return fp_ != nullptr;
    }

    int seek(int64_t pos, int whence) override
    {
        if (!fp_) {
            return AUDIO_ERR_IO;
        }
        if (fseeko(fp_, (off_t)pos, whence) != 0) {
            AUDIO_LOGE("fseeko failed (pos=%lld whence=%d)", (long long)pos, whence);
            return AUDIO_ERR_IO;
        }
        off_t np = ftello(fp_);
        if (np < 0) {
            return AUDIO_ERR_IO;
        }
        consumed_ = (int64_t)np;
#if !FS_USE_VFS
        pos_ = len_ = 0;
#endif
        eof_ = false;
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
        return 1; /* 文件源：数据总是"立刻可读"（读不到就是 EOF） */
    }

private:
    /**
     * @brief 把窗口填到至少 need 字节可用（若文件允许）。
     *
     * 若窗口容量不足，则搬移已有数据到头部，尽量腾出空间。
     * 用作 ID3/魔数探测的前瞻读取。
     */
    int ensure(size_t need)
    {
        if (need > cap_) {
            need = cap_; /* 参数不合理时退化为"尽量多" */
        }
        if (len_ - pos_ >= need) {
            return (int)(len_ - pos_);
        }

        /* 把未消费数据搬到缓冲头部 */
        size_t avail = len_ - pos_;
        if (avail > 0 && pos_ > 0) {
            memmove(buf_, buf_ + pos_, avail);
        }
        len_ = avail;
        pos_ = 0;

        /* 继续预读直到满足 need 或 EOF */
        while (len_ < need) {
            int n = refill();
            if (n <= 0) {
                break;
            }
        }
        return (int)(len_ - pos_);
    }

    /**
     * @brief 从文件读一块追加到窗口尾部。
     *
     * @return >0 读到的字节数；0 表示"本次没读到新数据"。
     *
     * 注意：调用者不能把 0 直接当作 EOF。窗口满（len_ >= cap_）时也有未消费数据
     * 可交付，此时同样返回 0。早期的 read() 把 0 当 EOF 处理，导致只要一次请求
     * 超过窗口容量就**丢弃剩余数据**（vorbis/opus 这种连续大块读取必踩）。
     * 现在 read() 会在 pos_ == len_ 时才依赖 eof_ 判定。
     */
    int refill()
    {
        if (!fp_ || eof_) {
            return 0;
        }
        if (pos_ > 0) {
            /* 只要还有未消费数据就搬到头部，腾出尾部空间 */
            size_t avail = len_ - pos_;
            if (avail > 0) {
                memmove(buf_, buf_ + pos_, avail);
            }
            len_ = avail;
            pos_ = 0;
        }

        if (len_ >= cap_) {
            return 0; /* 窗口满，但数据仍可交付 */
        }

        size_t space = cap_ - len_;
        size_t got = fread(buf_ + len_, 1, space, fp_);
        len_ += got;
        if (got == 0) {
            if (feof(fp_)) {
                eof_ = true;
            } else if (ferror(fp_)) {
                AUDIO_LOGE("fread error (errno=%d)", errno);
                eof_ = true;
            }
        }
        return (int)got;
    }

    FILE   *fp_ = nullptr;
    uint8_t *buf_ = nullptr;       /**< WINDOW 模式：自管理窗口缓冲 */
    char   *vfs_buf_ = nullptr;    /**< VFS 模式：setvbuf 的缓冲（PSRAM 优先） */
    size_t  cap_ = 0;       /**< 窗口容量 */
    size_t  len_ = 0;       /**< 窗口内有效字节数 */
    size_t  pos_ = 0;       /**< 窗口内已消费位置 */
    int64_t consumed_ = 0;  /**< 对外逻辑位置（已交付给调用者的字节数） */
    int64_t size_ = -1;     /**< <0 = 未知 */
    bool    eof_ = false;
};

AudioInput *audio_source_fs_create(const char *path)
{
    AudioSourceFs *s = new (std::nothrow) AudioSourceFs();
    if (!s) {
        return nullptr;
    }
    if (!s->open(path)) {
        delete s;
        return nullptr;
    }
    return s;
}
