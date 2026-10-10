/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 kanfandelong. All rights reserved.
 */

/**
 * @file    audio_sink_i2s.c
 * @brief   I2S 输出：调用者持有通道，本类负责写数据与采样率重配。
 *
 * 关键点：
 *  - 采样率变化时内部自动重配置。
 */

#include "audio_port.h"
#include "audio_sink.h"
#include "audio_types.h"

#include <math.h>
#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

/** 每次 i2s_channel_write 的默认超时（毫秒）。上层可覆盖。 */
#define I2S_WRITE_TIMEOUT_DEFAULT 20

class AudioSinkI2s : public AudioSink {
public:
    AudioSinkI2s(i2s_chan_handle_t tx, uint32_t io_timeout_ms, uint8_t target_bits)
        : tx_(tx), io_timeout_ms_(io_timeout_ms ? io_timeout_ms : I2S_WRITE_TIMEOUT_DEFAULT),
          target_bits_(target_bits)
    {
        lock_ = xSemaphoreCreateMutex();
    }

    ~AudioSinkI2s() override
    {
        
#if defined(CONFIG_ESPAUDIOCORE_DEBUG_DIAGNOSTICS)
        stat_dump();
        if (dump_) {
            fclose(dump_);
            dump_ = nullptr;
        }
#endif

        if (lock_) {
            vSemaphoreDelete(lock_);
            lock_ = nullptr;
        }
        audio_free(buf_a_);
        audio_free(buf_b_);
        // audio_free(buf_c_);
        buf_a_ = nullptr;
        buf_b_ = nullptr;
        // buf_c_ = nullptr;
    }

    bool init()
    {
        if (!tx_ || !lock_) {
            return false;
        }
        
#if defined(CONFIG_ESPAUDIOCORE_DEBUG_DIAGNOSTICS)
        /* 诊断 dump：把真实送去 I2S 的字节落盘，用电脑端工具精确比对。
         * 统计数字已经不足以定位，必须看原始样本。 */
        if (!dump_) {
            dump_ = fopen("/sdcard/i2s_dump.pcm", "wb");
            if (dump_) {
                AUDIO_LOGW("diagnostic dump enabled -> /sdcard/i2s_dump.pcm");
            }
        }
#endif

        /* 转换缓冲放堆/PSRAM，不放在对象里：三块合计几十 KB，
         * 放对象内会让每次 new AudioSinkI2s 都吃掉一大块堆。 */
        if (!buf_a_) {
            buf_a_ = (int32_t *)audio_alloc_big(CONV_SAMPLES * sizeof(int32_t), 0);
            buf_b_ = (uint8_t *)audio_alloc_big(CONV_SAMPLES * 4, 0);
            // buf_c_ = (int32_t *)audio_alloc_big(CONV_SAMPLES * sizeof(int32_t), 0);
            if (!buf_a_ || !buf_b_/*  || !buf_c_ */) {
                AUDIO_LOGE("no memory for I2S conversion buffers");
                return false;
            }
        }
        /* 主动同步通道的**实际**配置，避免第一次 set_rate/set_format 做多余的
         * disable->reconfig->enable（那会在起播前造成一次静音与时钟中断）。
         * 依据：i2s_chan_info_t::mode_cfg 在 STD 模式下就是 i2s_std_config_t*。 */
        i2s_chan_info_t info = {};
        if (i2s_channel_get_info(tx_, &info) == ESP_OK && info.mode_cfg) {
            const i2s_std_config_t *std_cfg = (const i2s_std_config_t *)info.mode_cfg;
            cur_rate = std_cfg->clk_cfg.sample_rate_hz;
            uint8_t hw_from_info = 0;
            switch (std_cfg->slot_cfg.data_bit_width) {
            case I2S_DATA_BIT_WIDTH_8BIT:  hw_from_info = 8;  break;
            case I2S_DATA_BIT_WIDTH_16BIT: hw_from_info = 16; break;
            case I2S_DATA_BIT_WIDTH_24BIT: hw_from_info = 24; break;
            case I2S_DATA_BIT_WIDTH_32BIT: hw_from_info = 32; break;
            default:                       hw_from_info = 0;  break;
            }
            /* 注意两个位宽语义不同，不能混：
             *   cur_hw_bits  = 通道当前的**硬件槽宽**（由应用初始化决定）
             *   cur_bits     = 解码器**源**位宽。本库的解码器统一输出 16-bit，
             *                  所以这里记 16；write() 再按 target_bits_ 决定是否左移补齐。
             * 若把 cur_bits 也设成 32，write() 会误以为源就是 32-bit 而跳过转换，
             * 导致 PCM5102 收到未对齐的数据。 */
            cur_hw_bits  = hw_from_info;
            cur_channels = (std_cfg->slot_cfg.slot_mode == I2S_SLOT_MODE_MONO) ? 1 : 2;
            cur_bits     = 16; /* 解码器源位宽 */
            cur_src_bits = 16;
            AUDIO_LOGI("I2S actual config: %u Hz, %u ch, hw %u-bit; decoder source assumed 16-bit",
                       (unsigned)cur_rate, (unsigned)cur_channels, (unsigned)cur_hw_bits);
        } else {
            AUDIO_LOGW("i2s_channel_get_info failed; will reconfigure on first set_rate/set_format");
        }
        return true;
    }

    /** 把任意位宽的 PCM 提升为 packed 32-bit，供音量缩放使用 */
    static size_t to_32bit(const void *src, size_t samples, uint8_t src_bits, void *dst,
                           size_t dst_cap)
    {
        if (src_bits == 32) {
            size_t n = samples;
            if (n > dst_cap / sizeof(int32_t)) {
                n = dst_cap / sizeof(int32_t);
            }
            memcpy(dst, src, n * sizeof(int32_t));
            return n * sizeof(int32_t);
        }
        return convert_bits(src, samples, src_bits, dst, dst_cap, 32);
    }

    void set_stop_flag(const volatile bool *flag)
    {
        stop_flag_ = flag;
    }

    /* ---------------- AudioSink ---------------- */

    /**
     * @brief 统一 int32 路径（推荐入口，见 D-23）。
     *
     * 解码器负责把 PCM 对齐成 int32（16-bit<<16 / 24-bit<<8 / 32-bit 原值），
     * 这里一步完成：**音量缩放 → 按目标位宽下采样 → 写 I2S**。
     * 全程只在 int32 域做一次遍历，24/32-bit 源不损失精度。
     */
    int write_i32(const int32_t *pcm, size_t pairs, uint8_t src_bits,
                  uint32_t timeout_ms) override
    {
        if (!tx_ || !pcm || pairs == 0 || !buf_a_) {
            return AUDIO_OK;
        }
        const uint8_t ob = target_bits_ ? target_bits_ : (cur_bits ? cur_bits : 32);
        const uint8_t sb = src_bits ? src_bits : 16;
        /* 有效位深比输出位宽窄时，需要右移收窄；否则保持精度 */
        int rshift = 0;
        if (ob < sb) {
            rshift = (int)sb - (int)ob;
        } else if (ob == 16 && sb == 16) {
            rshift = 0; /* int32 已是 <<16，写 16-bit 时下面按 ob 截取 */
        }

        const bool unity = gain_is_unity();
        const uint32_t g = gain_q16();
        const size_t total = pairs * 2; /* 采样点总数 */

        /* 一次性诊断：确认 int32 域收到的数据形状与数值范围 */
        static bool diag_done = false;
        if (!diag_done) {
            diag_done = true;
            AUDIO_LOGI("write_i32#1: %u pairs, src_bits=%u, target=%u-bit, unity=%d | "
                       "pcm[0..3]=%ld,%ld,%ld,%ld",
                       (unsigned)pairs, (unsigned)sb, (unsigned)ob, unity ? 1 : 0,
                       (long)pcm[0], (long)pcm[1], (long)pcm[2], (long)pcm[3]);
        }
        size_t done = 0;

        while (done < total) {
            size_t n = total - done;
            if (n > CONV_SAMPLES) {
                n = CONV_SAMPLES;
            }

            if (ob >= 32) {
                /* 32-bit 输出：直接缩放后写 int32 */
                int32_t *o32 = (int32_t *)buf_a_;
                for (size_t i = 0; i < n; i++) {
                    int64_t v = pcm[done + i];
                    if (!unity) {
                        v = (v * (int64_t)g) >> 16;
                        if (v > INT32_MAX) {
                            v = INT32_MAX;
                        } else if (v < INT32_MIN) {
                            v = INT32_MIN;
                        }
                    }
                    o32[i] = (int32_t)v;
                }
                int rc = raw_write(o32, n * sizeof(int32_t), timeout_ms);
                if (rc != AUDIO_OK) {
                    return rc;
                }
            } else {
                /* 16/24-bit 输出：先缩放到 int32，再收窄到目标位宽 */
                int32_t *o32 = (int32_t *)buf_a_;
                for (size_t i = 0; i < n; i++) {
                    int64_t v = pcm[done + i];
                    if (!unity) {
                        v = (v * (int64_t)g) >> 16;
                        if (v > INT32_MAX) {
                            v = INT32_MAX;
                        } else if (v < INT32_MIN) {
                            v = INT32_MIN;
                        }
                    }
                    o32[i] = (int32_t)v;
                }
                size_t nb = convert_bits(o32, n, 32, buf_b_, CONV_SAMPLES * 4, ob);
                if (nb == 0) {
                    return AUDIO_ERR_IO;
                }
                int rc = raw_write(buf_b_, nb, timeout_ms);
                if (rc != AUDIO_OK) {
                    return rc;
                }
            }
            done += n;
        }

        account_write(pairs * (size_t)(cur_channels ? cur_channels : 2) * (sb / 8), sb);
        (void)rshift;
        return AUDIO_OK;
    }

    /**
     * @brief 逐采样点输出，结构对齐旧封装层 AudioOutputI2S::ConsumeSample()。
     *
     * 旧库（已验证正常）的做法是：每个采样点经过 Amplify() 增益，
     * 再按目标位宽组装，然后立刻写 I2S。这里刻意用同样的逐点结构，
     * 不再做整块 convert_bits —— 块转换引入过重叠缓冲与移位量问题。
     *
     * **位深语义**（对齐旧库的信号约定）：
     *   16-bit 源：解码器给 int16，`<< 16` 对齐到 int32 高位
     *   24-bit 源：解码器给 int32（已左移 8 位到高位），直接用
     *   32-bit 源：解码器给 int32 原值
     * 因此 24/32-bit 源**不经过任何右移**，无损精度得以保留；
     * 只有 target_bits 比源更窄时，才在下面显式右移（调用者明确要求）。
     */
    int write(const void *frames, size_t bytes, uint32_t timeout_ms) override
    {
        if (!tx_ || bytes == 0 || !buf_a_) {
            return AUDIO_OK;
        }

        const uint8_t sb = cur_bits ? cur_bits : 16;
        const uint8_t ob = target_bits_ ? target_bits_ : sb;
        const uint8_t ch = cur_channels ? cur_channels : 2;
        const size_t nsamp = bytes / (sb / 8); /* 交错采样点总数 */
        if (nsamp == 0) {
            return AUDIO_OK;
        }
        /* 超过一块容量就分多次写，保证不截断（否则会丢音频） */
        size_t done_samples = 0;
        while (done_samples < nsamp) {
            size_t n = nsamp - done_samples;
            if (n > CONV_SAMPLES) {
                n = CONV_SAMPLES;
            }

            const int32_t *s32 = nullptr;
            const int16_t *s16 = nullptr;
            if (sb == 16) {
                s16 = (const int16_t *)frames + done_samples;
            } else {
                s32 = (const int32_t *)frames + done_samples;
            }
            int32_t *o32 = (int32_t *)buf_a_;

            const bool unity = gain_is_unity();
            const uint32_t g = gain_q16();

            if (sb != 16) {
                /* 24/32-bit 源：值已在 int32 的高位，直接使用 */
                for (size_t i = 0; i < n; i++) {
                    int32_t v = s32[i];
                    if (!unity) {
                        int64_t t = ((int64_t)v * (int64_t)g) >> 16;
                        if (t > INT32_MAX) {
                            t = INT32_MAX;
                        } else if (t < INT32_MIN) {
                            t = INT32_MIN;
                        }
                        v = (int32_t)t;
                    }
                    o32[i] = (ob < 32) ? (v >> (32 - ob)) : v;
                }
            } else {
                /* 16-bit 源：左移 16 位对齐到 int32 高位（旧库同款） */
                for (size_t i = 0; i < n; i++) {
                    int32_t v = (int32_t)s16[i] << 16;
                    if (!unity) {
                        int64_t t = ((int64_t)v * (int64_t)g) >> 16;
                        if (t > INT32_MAX) {
                            t = INT32_MAX;
                        } else if (t < INT32_MIN) {
                            t = INT32_MIN;
                        }
                        v = (int32_t)t;
                    }
                    o32[i] = (ob < 32) ? (v >> (32 - ob)) : v;
                }
            }

            int rc = raw_write(o32, n * (size_t)(ob / 8), timeout_ms);
            if (rc != AUDIO_OK) {
                return rc;
            }
            done_samples += n;
        }

        account_write(bytes, sb);
        (void)ch;
        return AUDIO_OK;
    }

    /**
     * @brief 把缓冲写进 I2S，直到写完或被 stop 打断。
     *
     * 与旧库 i2s_channel_write(timeout_ms) 的语义一致：
     * 阻塞等待 DMA 腾出空间（不再用 SetTimeout(0) 那种丢样本的做法）。
     */
    int raw_write(const void *data, size_t bytes, uint32_t timeout_ms)
    {
        if (!tx_ || bytes == 0) {
            return AUDIO_OK;
        }
        uint32_t tmo = timeout_ms ? timeout_ms : io_timeout_ms_;
        if (tmo == 0) {
            tmo = portMAX_DELAY;
        }

        const uint8_t *p = (const uint8_t *)data;
        size_t left = bytes;

        while (left > 0) {
            if (stop_flag_ && *stop_flag_) {
                return AUDIO_ERR_NOT_SUPPORTED;
            }
            size_t written = 0;
            esp_err_t err = i2s_channel_write(tx_, p, left, &written, tmo);
            if (err == ESP_OK && written > 0) {
                p += written;
                left -= written;
                continue;
            }
            if (err == ESP_ERR_TIMEOUT) {
                continue; /* DMA 满：继续等（可被 stop 打断） */
            }
            AUDIO_LOGE("i2s_channel_write failed: %s", esp_err_to_name(err));
            return AUDIO_ERR_IO;
        }
        return AUDIO_OK;
    }

    bool can_block() const override
    {
        return true;
    }

    /**
     * @brief 重配采样率。
     *
     * **必须 disable -> reconfig -> enable**：实测 IDF 会直接拒绝（日志
     * "invalid state, I2S should be disabled before reconfiguring the clock"）。
     * 之前以为 reconfig 内部会处理禁/启，是错的。
     *
     * 重要：无论成功失败都要更新 cur_rate，否则 format_changed() 恒为真，
     * 解码循环会**每帧重试一次**，把日志刷爆并卡住播放。
     */
    int set_rate(uint32_t hz) override
    {
        if (!tx_) {
            return AUDIO_ERR_INVALID_ARG;
        }
        if (hz == cur_rate) {
            return AUDIO_OK;
        }

        i2s_std_clk_config_t clk = {};
        clk.sample_rate_hz = hz;
        clk.clk_src        = I2S_CLK_SRC_DEFAULT;
        clk.mclk_multiple  = I2S_MCLK_MULTIPLE_256;

        xSemaphoreTake(lock_, portMAX_DELAY);
        esp_err_t err = i2s_channel_disable(tx_);
        if (err == ESP_OK) {
            err = i2s_channel_reconfig_std_clock(tx_, &clk);
        }
        esp_err_t en = i2s_channel_enable(tx_);
        xSemaphoreGive(lock_);

        if (err != ESP_OK) {
            AUDIO_LOGE("reconfig clock to %u Hz failed: %s", (unsigned)hz, esp_err_to_name(err));
            cur_rate = hz; /* 记住已尝试，避免每帧重试刷屏 */
            return AUDIO_ERR_IO;
        }
        if (en != ESP_OK) {
            AUDIO_LOGE("re-enable after clock reconfig failed: %s", esp_err_to_name(en));
        }
        AUDIO_LOGI("I2S sample rate -> %u Hz", (unsigned)hz);
        cur_rate = hz;
        return AUDIO_OK;
    }

    int set_format(uint8_t bits, uint8_t channels) override
    {
        if (!tx_) {
            return AUDIO_ERR_INVALID_ARG;
        }
        
#if defined(CONFIG_ESPAUDIOCORE_DEBUG_DIAGNOSTICS)
        /* 新格式开始：先把上一段的形状统计打出来（每个文件一条，不刷屏） */
        stat_dump();
#endif

        /* 硬件槽宽在强制目标位宽时使用目标位宽，否则跟随源位宽。
         * 注意 cur_bits 记录的是**源**位宽（解码器输出），这样才能在 write()
         * 里正确判断"是否需要左移补齐"。 */
        uint8_t hw_bits = target_bits_ ? target_bits_ : bits;
        if (hw_bits == cur_hw_bits && channels == cur_channels && bits == cur_src_bits) {
            return AUDIO_OK;
        }

        i2s_slot_bit_width_t slot;
        i2s_data_bit_width_t data;
        switch (hw_bits) {
        case 8:
            slot = I2S_SLOT_BIT_WIDTH_8BIT;
            data = I2S_DATA_BIT_WIDTH_8BIT;
            break;
        case 24:
            slot = I2S_SLOT_BIT_WIDTH_24BIT;
            data = I2S_DATA_BIT_WIDTH_24BIT;
            break;
        case 32:
            slot = I2S_SLOT_BIT_WIDTH_32BIT;
            data = I2S_DATA_BIT_WIDTH_32BIT;
            break;
        case 16:
        default:
            slot = I2S_SLOT_BIT_WIDTH_16BIT;
            data = I2S_DATA_BIT_WIDTH_16BIT;
            break;
        }

        i2s_std_slot_config_t cfg = {};
        cfg.data_bit_width   = data;
        cfg.slot_bit_width   = slot;
        cfg.slot_mode        = (channels == 1) ? I2S_SLOT_MODE_MONO : I2S_SLOT_MODE_STEREO;
        cfg.slot_mask        = (channels == 1) ? I2S_STD_SLOT_LEFT : I2S_STD_SLOT_BOTH;
        cfg.ws_width         = slot;
        cfg.ws_pol           = false;
        cfg.bit_shift        = true;
        cfg.left_align       = false;
        cfg.big_endian       = false;
        cfg.bit_order_lsb    = false;

        /* 与 set_rate 同理：必须 disable -> reconfig -> enable */
        xSemaphoreTake(lock_, portMAX_DELAY);
        esp_err_t err = i2s_channel_disable(tx_);
        if (err == ESP_OK) {
            err = i2s_channel_reconfig_std_slot(tx_, &cfg);
        }
        esp_err_t en = i2s_channel_enable(tx_);
        xSemaphoreGive(lock_);

        if (err != ESP_OK) {
            AUDIO_LOGE("reconfig slot failed: %s", esp_err_to_name(err));
            /* 同样要记下，否则每帧重试 */
            cur_bits     = bits;
            cur_src_bits = bits;
            cur_hw_bits  = hw_bits;
            cur_channels = channels;
            return AUDIO_ERR_IO;
        }
        if (en != ESP_OK) {
            AUDIO_LOGE("re-enable after slot reconfig failed: %s", esp_err_to_name(en));
        }
        cur_bits      = bits; /* 源位宽（解码器输出） */
        cur_src_bits  = bits;
        cur_hw_bits   = hw_bits;
        cur_channels  = channels;
        AUDIO_LOGI("I2S format: src %u-bit -> hw %u-bit, %u ch", (unsigned)bits, (unsigned)hw_bits,
                   (unsigned)channels);
        return AUDIO_OK;
    }

    int flush() override
    {
        /* 停写即可：auto_clear_after_cb 打开时 DMA 空后 I2S 自动发零，
         * 不会卡在最后一个电平上产生爆音。 */
        return AUDIO_OK;
    }

    /**
     * @brief DMA 队列内尚未播出的字节数（用于位置查询扣除）。
     * @note 仅在通道已初始化时有效。
     */
    size_t dma_pending_bytes() const
    {
        if (!tx_) {
            return 0;
        }
        i2s_chan_info_t info = {};
        if (i2s_channel_get_info(tx_, &info) != ESP_OK) {
            return 0;
        }
        return info.total_dma_buf_size;
    }

private:

#if defined(CONFIG_ESPAUDIOCORE_DEBUG_DIAGNOSTICS)
    /**
     * @brief 输出"送去 I2S 的字节"的形状统计。
     *
     * 用途：当听感异常（炸音/破音）时，用客观数字区分两类病因——
     *   - 有削顶 / RMS 顶到 0 dBFS 附近 => 数字增益或移位环节放大了样本；
     *   - 峰值远低于满量程且无削顶     => 数据没问题，问题在时钟/对齐/DAC。
     * 统计覆盖全部已写入样本，因此在每次格式变化与析构时各输出一次。
     */
    void stat_dump()
    {
        if (stat_n_ == 0) {
            return;
        }
        /* 注意单位：stat_rms_acc_ 累加的是 (v^2)>>16，其均值即 (v/2^8)^2。
         * dBFS 基准用 2^23（= 满量程 2^31 除以 2^8）。 */
        int64_t peak_db100 = 0;
        if (stat_peak_ > 0) {
            peak_db100 = (int64_t)(2000.0 * log10((double)stat_peak_ / (double)FT_FULL));
        }
        uint64_t mean_sq = stat_rms_acc_ / stat_n_;
        int64_t rms_db100 = -99999;
        if (mean_sq > 0) {
            rms_db100 = (int64_t)(2000.0 * log10((double)mean_sq / 8388608.0 / 256.0));
        }
        AUDIO_LOGI("I2S data stats: n=%llu peak=%lld (%.2f dBFS) rms=%.2f dBFS clipped=%llu",
                   (unsigned long long)stat_n_, (long long)stat_peak_,
                   (double)peak_db100 / 100.0, (double)rms_db100 / 100.0,
                   (unsigned long long)stat_clip_);
        AUDIO_LOGI("I2S data range: min=%lld max=%lld (full scale +-%lld)",
                   (long long)stat_min_neg_, (long long)stat_max_pos_, (long long)FT_FULL);
        AUDIO_LOGI("I2S amplitude histogram (each bucket = 10%% of full scale):");
        for (int i = 0; i < 10; i++) {
            uint64_t pct10 = (stat_n_ ? (stat_hist_[i] * 1000ULL / stat_n_) : 0);
            AUDIO_LOGI("   [%d%%-%d%%] %8llu  (%llu.%llu%%)", i * 10, (i + 1) * 10,
                       (unsigned long long)stat_hist_[i], (unsigned long long)(pct10 / 10),
                       (unsigned long long)(pct10 % 10));
        }
        if (stat_clip_ > 0) {
            AUDIO_LOGW("  -> %llu samples at/over full scale: 数据层面存在削波",
                       (unsigned long long)stat_clip_);
        } else {
            AUDIO_LOGI("  -> no clipping: 数据层面干净，若仍炸音则问题在时钟/对齐/DAC");
        }
        stat_peak_ = 0;
        stat_rms_acc_ = 0;
        stat_n_ = 0;
        stat_clip_ = 0;
        stat_max_pos_ = 0;
        stat_min_neg_ = 0;
        memset(stat_hist_, 0, sizeof(stat_hist_));
    }
#endif

    i2s_chan_handle_t      tx_ = nullptr;
    SemaphoreHandle_t      lock_ = nullptr;
    uint32_t               io_timeout_ms_;
    uint8_t                target_bits_ = 0;  /**< 强制输出位宽，0 = 跟随源 */
    uint8_t                cur_src_bits = 0;  /**< 当前源位宽 */
    uint8_t                cur_hw_bits = 0;   /**< 当前硬件槽宽 */
    const volatile bool   *stop_flag_ = nullptr;

    /** 一轮写能处理的样本数上限；三块缓冲都按它分配 */
    static const size_t CONV_SAMPLES = 8192;

    /** 32-bit 满量程（int32 范围内） */
    static const int64_t FT_FULL = 2147483647LL;

    
#if defined(CONFIG_ESPAUDIOCORE_DEBUG_DIAGNOSTICS)
    /* 送去 I2S 的字节形状统计（每个格式重置一次，用于客观判断削波） */
    int64_t  stat_peak_ = 0;
    uint64_t stat_rms_acc_ = 0;
    uint64_t stat_n_ = 0;
    uint64_t stat_clip_ = 0;
    int      stat_probe_ = 0;
    uint64_t stat_write_calls_ = 0; /**< write() 调用次数，用于周期采样 */

    /** 每多少次 write 采样一次（文件头是静音段，必须采到中段） */
    static const uint64_t PROBE_INTERVAL = 400;
    static const int      PROBE_MAX = 6;
    int64_t  stat_max_pos_ = 0;  /**< 最大正样本 */
    int64_t  stat_min_neg_ = 0;  /**< 最小负样本 */
    uint64_t stat_hist_[10] = {}; /**< 幅度分布：每 10% 满量程一桶 */

    FILE    *dump_ = nullptr;    /**< 诊断 dump 文件 */
    int      dump_n_ = 0;
#endif

    int32_t *buf_a_ = nullptr; /**< 音量路径的 packed 32-bit 中间缓冲 */
    uint8_t *buf_b_ = nullptr; /**< 位宽转换输出缓冲（字节寻址，兼容 24-bit） */
    // int32_t *buf_c_ = nullptr; /**< 备用（保留以示三级流水线，避免将来再踩重叠） */
};

AudioSink *audio_sink_i2s_create(i2s_chan_handle_t tx, uint32_t io_timeout_ms, uint8_t target_bits)
{
    AudioSinkI2s *s = new (std::nothrow) AudioSinkI2s(tx, io_timeout_ms, target_bits);
    if (!s) {
        return nullptr;
    }
    if (!s->init()) {
        delete s;
        return nullptr;
    }
    return s;
}

/* 供 audio_player 在启动解码任务前绑定停止标志 */
void audio_sink_i2s_bind_stop(AudioSink *sink, const volatile bool *flag)
{
    AudioSinkI2s *i2s = static_cast<AudioSinkI2s *>(sink);
    if (i2s) {
        i2s->set_stop_flag(flag);
    }
}

/* 供位置查询使用 */
size_t audio_sink_i2s_pending_bytes(AudioSink *sink)
{
    AudioSinkI2s *i2s = static_cast<AudioSinkI2s *>(sink);
    return i2s ? i2s->dma_pending_bytes() : 0;
}

/* I2S 有 DMA 残留，ringbuf 输出没有（下游立刻消费） */
size_t audio_sink_pending_bytes(AudioSink *sink)
{
    return audio_sink_i2s_pending_bytes(sink);
}
