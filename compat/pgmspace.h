/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 kanfandelong. All rights reserved.
 *
 * Third-party legacy compatibility header for libhelix-mp3/aac, libogg and
 * libopus. These vendored libraries were written for AVR/Arduino and still say
 * #include <pgmspace.h>. On Linux .rodata and .text are in the same address
 * space, so PROGMEM and pgm_read_* collapse to plain memory accesses.
 *
 * This is NOT an operating-system compatibility layer: no ESP-IDF, FreeRTOS
 * or I2S API is emulated here.
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

/* AVR _P string/memory helpers degrade to the standard functions. */
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

/* Reading "program memory" is a normal dereference on Linux. */
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

#ifndef PROGMEM_ATTR
#define PROGMEM_ATTR
#endif
#ifndef ICACHE_RODATA_ATTR
#define ICACHE_RODATA_ATTR
#endif
