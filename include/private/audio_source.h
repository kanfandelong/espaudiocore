/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 kanfandelong. All rights reserved.
 */

/**
 * @file    audio_source.h
 * @brief   Concrete sources: local file / ring buffer. Internal use.
 */

#pragma once

#include "audio_types.h"

#include "espaudiocore.h"

/* This header contains C++ types and is for .cpp files only. */

/**
 * @brief Local file source (POSIX read/pread/lseek).
 *
 * @param path file path; relative and absolute paths are accepted
 * @return NULL when the file cannot be opened or is not a regular file
 */
AudioInput *audio_source_fs_create(const char *path);

/**
 * @brief Ring-buffer source (pure sequential stream, no seek).
 *
 * @param rb caller-owned input ring buffer
 */
AudioInput *audio_source_ringbuf_create(espaudiocore_ringbuf_t *rb);
