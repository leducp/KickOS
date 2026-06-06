// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Small freestanding formatter. Supports: %s %c %d %i %u %x %p %% plus the
// 'l'/'z' length modifiers for the integer conversions. No floats, no width.
// Always NUL-terminates when size > 0. Returns the number of chars that would
// have been written (C99 vsnprintf semantics), never touching the console.

#ifndef KICKOS_LIBC_FMT_H
#define KICKOS_LIBC_FMT_H

#include <stddef.h>
#include <stdarg.h>

#ifdef __cplusplus
extern "C"
{
#endif

int kvsnprintf(char* buf, size_t size, char const* fmt, va_list ap);
int ksnprintf(char* buf, size_t size, char const* fmt, ...)
    __attribute__((format(printf, 3, 4)));

#ifdef __cplusplus
}
#endif

#endif
