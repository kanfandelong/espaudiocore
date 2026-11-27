/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 kanfandelong. All rights reserved.
 */

/**
 * @file    audio_picture.h
 * @brief   Cover-art helpers shared by the container decoders.
 *
 * FLAC stores pictures in a binary METADATA_BLOCK_PICTURE block; Vorbis and
 * Opus put the same block in a base64 comment.  The helpers below decode that
 * block (or a legacy COVERART base64 value) and report it through the normal
 * espaudiocore metadata callback.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "audio_types.h"

#ifdef __cplusplus

/** Sniff an image MIME type from its signature; NULL when unknown. */
const char *audio_picture_mime(const uint8_t *data, size_t len);

/**
 * @brief Parse one FLAC METADATA_BLOCK_PICTURE payload and emit it.
 *
 * @param data  Binary picture block (NOT base64).
 * @param len   Payload length.
 * @return true when the block was valid and a picture was reported.
 */
bool audio_emit_flac_picture(const uint8_t *data, size_t len,
                             espaudiocore_meta_cb_t cb, void *user);

/**
 * @brief Handle a Vorbis/Opus picture comment.
 *
 * Recognized keys (case-insensitive):
 *   METADATA_BLOCK_PICTURE  base64 FLAC picture block
 *   COVERART                base64 image (legacy)
 *   COVERARTMIME            value is consumed but does not emit a picture
 *
 * @return true when the key belongs to this helper, so the caller should not
 *         forward it as ordinary text metadata.
 */
bool audio_emit_picture_comment(const char *key, const char *value, size_t len,
                                espaudiocore_meta_cb_t cb, void *user);

#endif /* __cplusplus */
