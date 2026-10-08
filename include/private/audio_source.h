/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 kanfandelong. All rights reserved.
 */

/**
 * @file    audio_source.h
 * @brief   数据源具体实现：本地文件 / ringbuf。内部使用。
 */

#pragma once

#include "audio_types.h"

/* 本头文件含 C++ 类型，仅供 .cpp 包含 */

/**
 * @brief 本地文件源（标准 C 库 + 预读窗口）。
 *
 * @param path 必须是含挂载点的绝对路径
 * @return NULL 表示路径不合法或文件打不开（调用者用 errno 之外的信息区分）
 */
AudioInput *audio_source_fs_create(const char *path);

/**
 * @brief ringbuf 源（纯顺序流，无 seek）。
 *
 * @param handle 调用者持有的输入 ringbuf，本类不负责删除
 */
AudioInput *audio_source_ringbuf_create(RingbufHandle_t handle);

