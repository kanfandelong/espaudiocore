/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 kanfandelong. All rights reserved.
 */

/**
 * @file    espaudiocore.h
 * @brief   Multi-format audio decoder with native Linux (ALSA) output.
 *
 * This header is the public C API of the native Linux build. It has no
 * dependency on ESP-IDF, FreeRTOS or I2S: output goes to ALSA, threads are
 * pthreads, and every function returns 0 or a negative errno value.
 *
 * Quick start (one call starts decoding, the worker thread lives until EOS
 * or until espaudiocore_stop() is called):
 *
 * @code{c}
 *   espaudiocore_cfg_t cfg = {0};
 *   cfg.alsa_device = "plughw:0,0"; // NULL => "default"
 *   cfg.on_event    = my_event_cb;
 *   cfg.on_meta     = my_meta_cb;
 *   cfg.user        = my_ctx;
 *
 *   int dec_err = 0;
 *   int rc = espaudiocore_begin("/music/a.mp3", &cfg, &dec_err);
 *   if (rc != 0) { fprintf(stderr, "%s\n", espaudiocore_strerror(rc)); }
 *   // ... ESPAUDIOCORE_EVT_EOS arrives on the decoder thread
 *   espaudiocore_stop();
 * @endcode
 *
 * Threading contract:
 *  - begin/pause/resume/seek/stop are meant to be called from one control
 *    thread (the player object is a process-wide singleton, exactly like the
 *    ESP-IDF build).
 *  - callbacks run on the decoder thread and must not block.
 *
 * @author  kanfandelong <kanfandelong@outlook.com>
 * @date    2026-11-20
 * @version 2.0.0
 */

#pragma once

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ESPAUDIOCORE_VERSION_MAJOR 2
#define ESPAUDIOCORE_VERSION_MINOR 0
#define ESPAUDIOCORE_VERSION_PATCH 0
#define ESPAUDIOCORE_VERSION_STRING "2.0.0"

/* ===========================================================================
 * Return codes
 * =========================================================================*/

/**
 * @brief Return codes. Success is 0, failures are negative errno values.
 *
 * The values are real errno constants, so callers can also compare against
 * -EINVAL etc. `espaudiocore_strerror()` prints a readable name.
 */
typedef enum {
    ESPAUDIOCORE_OK = 0,
    ESPAUDIOCORE_ERR_INVALID_ARG = -EINVAL,   /**< bad argument */
    ESPAUDIOCORE_ERR_NO_MEM = -ENOMEM,        /**< allocation failed */
    ESPAUDIOCORE_ERR_NOT_FOUND = -ENOENT,     /**< file/path/device not found */
    ESPAUDIOCORE_ERR_NOT_SUPPORTED = -ENOTSUP,/**< capability not available */
    ESPAUDIOCORE_ERR_IO = -EIO,               /**< read/write/driver failure */
    ESPAUDIOCORE_ERR_HEADER = -EBADMSG,       /**< container/header parse failure */
    ESPAUDIOCORE_ERR_STATE = -EPERM,          /**< operation invalid in current state */
    ESPAUDIOCORE_ERR_TIMEOUT = -ETIMEDOUT,    /**< blocking operation timed out */
    ESPAUDIOCORE_ERR_DECODER = -EILSEQ,       /**< decoder reported corrupt data */
} espaudiocore_err_t;

/** @brief Human readable name for a return code (never NULL). */
const char *espaudiocore_strerror(int err);

/** @brief Log levels, ordered from silent to verbose. */
typedef enum {
    ESPAUDIOCORE_LOG_NONE = 0,
    ESPAUDIOCORE_LOG_ERROR = 1,
    ESPAUDIOCORE_LOG_WARN = 2,
    ESPAUDIOCORE_LOG_INFO = 3,
    ESPAUDIOCORE_LOG_DEBUG = 4,
} espaudiocore_log_level_t;

/**
 * @brief Set the process-wide log level (default: INFO).
 *
 * The ESPAUDIOCORE_LOG_LEVEL environment variable is read once at startup and
 * provides the initial value.
 */
void espaudiocore_set_log_level(int level);
int espaudiocore_get_log_level(void);

/* ===========================================================================
 * Events
 * =========================================================================*/

typedef enum {
    ESPAUDIOCORE_EVT_STARTED = 0, /**< begin() succeeded, decoding has started */
    ESPAUDIOCORE_EVT_PAUSED,      /**< pause() took effect */
    ESPAUDIOCORE_EVT_RESUMED,     /**< resume() took effect */
    ESPAUDIOCORE_EVT_SEEKED,      /**< seek_ms() completed */
    ESPAUDIOCORE_EVT_EOS,         /**< end of stream reached normally */
    ESPAUDIOCORE_EVT_STOPPED,     /**< worker stopped (also follows EOS) */
    ESPAUDIOCORE_EVT_ERROR,       /**< error, see msg */
} espaudiocore_event_t;

/**
 * @brief Event callback, runs synchronously on the decoder thread.
 *
 * Contract: do not block (no ALSA writes, network or GUI work here).
 * `msg` points to a stack buffer and is only valid during the callback.
 */
typedef void (*espaudiocore_event_cb_t)(void *user, espaudiocore_event_t ev, const char *msg);

/**
 * @brief One metadata item. Text and binary (cover art) share this struct.
 *
 * @note APIC data points into the parser buffer and is only valid during the
 *       callback. Store it to a file inside the callback if it must live on.
 */
typedef struct {
    const char *type;      /**< "Title"/"Artist"/"Album"/"APIC"/... */
    bool        is_binary; /**< true for APIC and other binary tags */
    const char *data;      /**< text: NUL terminated; binary: raw bytes */
    size_t      len;       /**< text: strlen; binary: byte count */
    const char *mime;      /**< image MIME, e.g. "image/jpeg"; NULL for text */
    int         pic_type;  /**< ID3 APIC picture type 0-20; -1 for non-images */
} espaudiocore_meta_t;

/**
 * @brief Metadata callback, runs synchronously on the decoder thread.
 *
 * `meta->data` for pictures is only valid during the callback. Do not block.
 */
typedef void (*espaudiocore_meta_cb_t)(void *user, const espaudiocore_meta_t *meta);

/* ===========================================================================
 * PCM tap (FFT / waveform visualisation)
 * =========================================================================*/

/**
 * @brief A block of decoded PCM.
 *
 * Values are left-aligned int32 samples, matching what the decoder hands to
 * the output stage:
 *  - `bits == 16`: value = int16 << 16 (use `v >> 16` for the 16-bit sample);
 *  - `bits == 24`: value = int24 << 8;
 *  - `bits == 32`: raw value.
 */
typedef struct {
    const int32_t *samples;  /**< interleaved: samples[frame * channels + ch] */
    size_t         frames;   /**< number of frames */
    uint32_t       rate;     /**< current sample rate in Hz */
    uint8_t        channels; /**< 1 or 2 */
    uint8_t        bits;     /**< effective bit depth 16/24/32 */
} espaudiocore_pcm_t;

/**
 * @brief PCM tap callback, fired before volume scaling and before the write
 *        to the device.
 *
 * Only available for device (ALSA) output; with ring-buffer output the
 * application already owns the PCM. The callback runs on the decoder thread
 * and must not block. `pcm->samples` is only valid during the callback.
 */
typedef void (*espaudiocore_pcm_cb_t)(void *user, const espaudiocore_pcm_t *pcm);

/* ===========================================================================
 * Formats
 * =========================================================================*/

typedef enum {
    ESPAUDIOCORE_FMT_UNKNOWN = 0,
    ESPAUDIOCORE_FMT_WAV,
    ESPAUDIOCORE_FMT_MP3,
    ESPAUDIOCORE_FMT_AAC,
    ESPAUDIOCORE_FMT_FLAC,
    ESPAUDIOCORE_FMT_VORBIS,
    ESPAUDIOCORE_FMT_OPUS,
    ESPAUDIOCORE_FMT_M4A,
    ESPAUDIOCORE_FMT_WAVPACK,
} espaudiocore_format_t;

/* ===========================================================================
 * Configuration
 * =========================================================================*/

/**
 * @brief Player configuration. Zero-initialise the struct and set what you
 *        need; every field has a documented default.
 */
typedef struct {
    /**
     * ALSA PCM device for `espaudiocore_begin()` / `espaudiocore_begin_stream()`.
     * NULL or "" selects "default". The board usually wants "plughw:0,0".
     */
    const char *alsa_device;

    /** ALSA target latency in milliseconds. 0 = 200 ms. Clamped to 20..2000. */
    uint32_t alsa_latency_ms;

    /** Blocking write timeout for device/ring output; 0 = wait forever. */
    uint32_t io_timeout_ms;

    /**
     * Output bit depth:
     *  - `0`: follow the PCM bit depth (no conversion);
     *  - `16/24/32`: fixed output depth. Narrower samples are shifted down,
     *    wider ones are left-aligned exactly like the ESP-IDF build.
     */
    uint8_t output_bits;

    /** Initial volume in dB. 0 = unity; -60 or below is treated as silence. */
    float volume_db;

    espaudiocore_event_cb_t on_event; /**< may be NULL */
    espaudiocore_meta_cb_t  on_meta;  /**< may be NULL */
    espaudiocore_pcm_cb_t   on_pcm;   /**< may be NULL; device output only */
    void                   *user;     /**< callback context */
} espaudiocore_cfg_t;

/* ===========================================================================
 * Ring buffer
 * =========================================================================*/

/** @brief Opaque, thread-safe byte ring buffer used by the stream APIs. */
typedef struct espaudiocore_ringbuf espaudiocore_ringbuf_t;

/**
 * @brief Allocate a ring buffer of `capacity` bytes.
 * @return NULL on invalid size or out of memory.
 */
espaudiocore_ringbuf_t *espaudiocore_ringbuf_create(size_t capacity);

/** @brief Free a ring buffer. Stop any session using it first. */
void espaudiocore_ringbuf_destroy(espaudiocore_ringbuf_t *rb);

/**
 * @brief Write bytes, waiting for space.
 *
 * @param timeout_ms <0 wait forever, 0 do not wait, >0 wait at most this long.
 * @return number of bytes written; may be less than `len` on timeout.
 */
size_t espaudiocore_ringbuf_write(espaudiocore_ringbuf_t *rb, const void *data, size_t len,
                                  int timeout_ms);

/**
 * @brief Read bytes, waiting for data.
 *
 * Copies at most `len` bytes and returns as soon as some data is available.
 * @return number of bytes read; 0 on timeout.
 */
size_t espaudiocore_ringbuf_read(espaudiocore_ringbuf_t *rb, void *data, size_t len,
                                 int timeout_ms);

/** @brief Bytes currently queued. */
size_t espaudiocore_ringbuf_used(const espaudiocore_ringbuf_t *rb);

/** @brief Bytes currently free. */
size_t espaudiocore_ringbuf_space(const espaudiocore_ringbuf_t *rb);

/** @brief Capacity in bytes. */
size_t espaudiocore_ringbuf_capacity(const espaudiocore_ringbuf_t *rb);

/** @brief Drop all queued bytes. */
void espaudiocore_ringbuf_clear(espaudiocore_ringbuf_t *rb);

/* ===========================================================================
 * Lifecycle
 * =========================================================================*/

/**
 * @brief Open a local file and start playing through ALSA immediately.
 *
 * @param path    Path to the audio file. Relative and absolute paths work.
 * @param cfg     Optional configuration (NULL = defaults).
 * @param dec_err [out] Decoder-native error code; may be NULL.
 *
 * @return 0 on success, or one of:
 *  - ESPAUDIOCORE_ERR_INVALID_ARG
 *  - ESPAUDIOCORE_ERR_NOT_FOUND   file cannot be opened
 *  - ESPAUDIOCORE_ERR_HEADER      header/container parse failure
 *  - ESPAUDIOCORE_ERR_NOT_SUPPORTED no decoder for this file
 *  - ESPAUDIOCORE_ERR_NO_MEM
 *  - ESPAUDIOCORE_ERR_IO          ALSA device cannot be opened/configured
 */
int espaudiocore_begin(const char *path, const espaudiocore_cfg_t *cfg, int *dec_err);

/**
 * @brief Open a local file and write decoded PCM into a ring buffer.
 *
 * Nothing touches ALSA in this mode. The application owns the ring buffer and
 * is responsible for its lifetime.
 */
int espaudiocore_begin_rb(const char *path, espaudiocore_ringbuf_t *out,
                          const espaudiocore_cfg_t *cfg, int *dec_err);

/**
 * @brief Play a compressed stream from a ring buffer through ALSA.
 *
 * The source is a pure sequential stream: seeking is not supported and the
 * format must be supplied by the caller (no probing).
 *
 * @note The following capabilities return ESPAUDIOCORE_ERR_NOT_SUPPORTED:
 *       - espaudiocore_seek_ms()
 *       - duration estimation that depends on seeking (get_duration_ms() < 0)
 */
int espaudiocore_begin_stream(espaudiocore_ringbuf_t *in, espaudiocore_format_t fmt,
                              const espaudiocore_cfg_t *cfg, int *dec_err);

/** @brief Compressed stream in, PCM stream out (no hardware involved). */
int espaudiocore_begin_stream_rb(espaudiocore_ringbuf_t *in, espaudiocore_format_t fmt,
                                 espaudiocore_ringbuf_t *out, const espaudiocore_cfg_t *cfg,
                                 int *dec_err);

/**
 * @brief Stop playback and release every resource. Blocks until the decoder
 *        thread has exited and the ALSA device has been drained/closed.
 */
int espaudiocore_stop(void);

/**
 * @brief Pause output.
 *
 * On ALSA output the device is paused when the driver supports it (the tail
 * already queued is not cut off). On ring-buffer output no new PCM is written.
 */
int espaudiocore_pause(void);

/** @brief Resume after pause. */
int espaudiocore_resume(void);

/**
 * @brief Seek to a position in milliseconds. Blocks until the seek took effect.
 *
 * @return ESPAUDIOCORE_ERR_NOT_SUPPORTED for stream (ring-buffer) input,
 *         ESPAUDIOCORE_ERR_STATE when nothing is playing.
 */
int espaudiocore_seek_ms(int64_t ms);

/** @brief Set volume in dB (applied to both ALSA and ring-buffer output). */
int espaudiocore_set_volume_db(float db);

/** @brief Current volume in dB. */
float espaudiocore_get_volume_db(void);

/* ===========================================================================
 * Queries
 * =========================================================================*/

/**
 * @brief Current playback position in milliseconds.
 *
 * The number of frames still buffered in ALSA is subtracted, so this reports
 * what the listener actually hears. Frozen while paused; 0 when idle.
 */
int64_t espaudiocore_get_position_ms(void);

/** @brief Total duration in milliseconds; <0 when unknown (streams, no header). */
int64_t espaudiocore_get_duration_ms(void);

/**
 * @brief Current output format.
 *
 * For ALSA output the library handles rate changes internally; this is for
 * display and diagnostics. For ring-buffer output this is the application's
 * only source of format information and must be handled downstream.
 *
 * Any out parameter may be NULL.
 */
int espaudiocore_get_format(uint32_t *rate, uint8_t *channels, uint8_t *bits);

/** @brief True while a session exists and the worker has not stopped. */
bool espaudiocore_is_running(void);

#ifdef __cplusplus
}
#endif
