/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 kanfandelong. All rights reserved.
 */

/**
 * @file    pgmspace.h
 * @brief   ESP-IDF 下的 AVR pgmspace 兼容层。
 *
 * ESP-IDF **不提供** pgmspace.h，
 * 而本组件内的第三方库（libhelix-mp3/aac、libogg、libopus、libmad 等）
 * 大量使用 PROGMEM / PSTR / pgm_read_*。
 *
 * ESP32 上 .rodata 本就在可寻址的 flash/PSRAM 映射区，没有 AVR 那种
 * "必须用特殊指令读"的约束，所以这些宏全部退化为普通操作——
 * 与 Arduino 侧 cores/esp32/pgmspace.h 的语义一致，无运行时差异。
 */

#pragma once

#include <string.h>

#ifndef PROGMEM
#define PROGMEM
#endif

#ifndef PGM_P
#define PGM_P const char *
#endif

#ifndef PGM_VOID_P
#define PGM_VOID_P const void *
#endif

#ifndef PSTR
#define PSTR(s) (s)
#endif

/* 带 _P 后缀的字符串/内存函数退化为标准版本 */
#ifndef memcpy_P
#define memcpy_P memcpy
#endif

#ifndef memcmp_P
#define memcmp_P memcmp
#endif

#ifndef strcpy_P
#define strcpy_P strcpy
#endif

#ifndef strncpy_P
#define strncpy_P strncpy
#endif

#ifndef strcat_P
#define strcat_P strcat
#endif

#ifndef strlen_P
#define strlen_P strlen
#endif

#ifndef strcmp_P
#define strcmp_P strcmp
#endif

#ifndef strncmp_P
#define strncmp_P strncmp
#endif

#ifndef sprintf_P
#define sprintf_P sprintf
#endif

#ifndef snprintf_P
#define snprintf_P snprintf
#endif

#ifndef vsnprintf_P
#define vsnprintf_P vsnprintf
#endif

/* 读 flash/rodata：普通解引用 */
#ifndef pgm_read_byte
#define pgm_read_byte(addr) (*(const unsigned char *)(addr))
#endif
#ifndef pgm_read_word
#define pgm_read_word(addr) (*(const unsigned short *)(addr))
#endif
#ifndef pgm_read_dword
#define pgm_read_dword(addr) (*(const unsigned long *)(addr))
#endif
#ifndef pgm_read_float
#define pgm_read_float(addr) (*(const float *)(addr))
#endif
#ifndef pgm_read_ptr
#define pgm_read_ptr(addr) (*(void *const *)(addr))
#endif

/* 近/远地址变体在 ESP32 上等价 */
#define pgm_read_byte_near(addr) pgm_read_byte(addr)
#define pgm_read_word_near(addr) pgm_read_word(addr)
#define pgm_read_dword_near(addr) pgm_read_dword(addr)
#define pgm_read_float_near(addr) pgm_read_float(addr)
#define pgm_read_ptr_near(addr) pgm_read_ptr(addr)

#define pgm_read_byte_far(addr) pgm_read_byte(addr)
#define pgm_read_word_far(addr) pgm_read_word(addr)
#define pgm_read_dword_far(addr) pgm_read_dword(addr)
#define pgm_read_float_far(addr) pgm_read_float(addr)
#define pgm_read_ptr_far(addr) pgm_read_ptr(addr)

/* 老代码里偶见的宏 */
#ifndef PROGMEM_ATTR
#define PROGMEM_ATTR
#endif
#ifndef ICACHE_RODATA_ATTR
#define ICACHE_RODATA_ATTR
#endif
