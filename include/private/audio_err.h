/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 kanfandelong. All rights reserved.
 */

/**
 * @file    audio_err.h
 * @brief   内部错误码。纯 C，不依赖 C++。
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/** 解码器原生错误码哨兵：公共 API 的 dec_err 出参在非解码失败时写 AUDIO_DEC_ERR_NONE */
#define AUDIO_DEC_ERR_NONE    0
#define AUDIO_DEC_ERR_OPEN   -1 /**< 解码器 open/begin 失败 */
#define AUDIO_DEC_ERR_PARSE  -2 /**< 头部解析失败 */

/** 内部错误码（不对外暴露；对外的返回码见 espaudiocore.h） */
typedef enum {
    AUDIO_OK = 0,
    AUDIO_FAIL,
    AUDIO_ERR_INVALID_ARG,
    AUDIO_ERR_NO_MEM,
    AUDIO_ERR_NOT_SUPPORTED, /**< 能力不支持（如 ringbuf 源上的 seek） */
    AUDIO_ERR_PARSE,         /**< 头部/容器解析失败 */
    AUDIO_ERR_IO,            /**< 读写失败 */
    AUDIO_ERR_DECODE,        /**< 解码器报错 */
    AUDIO_ERR_EOF,           /**< 数据正常结束 */
} audio_err_t;

#ifdef __cplusplus
}
#endif
