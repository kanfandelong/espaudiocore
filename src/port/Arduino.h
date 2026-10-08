/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 kanfandelong. All rights reserved.
 */

/**
 * @file    Arduino.h
 * @brief   极简兼容层：仅用于满足第三方库里的 #include <Arduino.h>。
 *
 * 背景：libhelix-aac 的 aaccommon.h 是第三方库中唯一直接 include
 *       Arduino.h 的文件。本组件不依赖 Arduino 核心，因此这里只提供
 *       该文件实际需要的那一点东西，绝不引入 Arduino 框架。
 *
 * 注意：本文件**不定义** ARDUINO 宏。第三方库里的
 *       `#elif defined(ARDUINO)` 平台分支已由 ESP_PLATFORM 分支覆盖，
 *       若在此定义 ARDUINO 会引入冗余分支并掩盖平台差异。
 */

#pragma once

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* libhelix-aac 用 `#ifndef ESP8266` 判断是否启用 SBR。
 * ESP32 系列（本组件目标）始终启用 SBR；该宏保持未定义即为启用。 */

#include "esp_pgmspace.h"