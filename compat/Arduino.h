/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 kanfandelong. All rights reserved.
 *
 * Third-party legacy compatibility header.
 *
 * libhelix-aac's aaccommon.h includes <Arduino.h> unconditionally. This
 * project does not use the Arduino framework; the file only needs the basic C
 * headers and pgmspace macros, so provide exactly that.
 *
 * ARDUINO is deliberately not defined: the helix platform branches select
 * their portable C path through __x86_64__/__aarch64__/ESP_PLATFORM, and
 * defining ARDUINO would only add unrelated branches.
 */

#pragma once

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "pgmspace.h"
