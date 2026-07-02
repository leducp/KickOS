// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Freestanding string/mem primitives. Standard names + C linkage so the
// compiler's implicit memcpy/memset lowering resolves against these.

#ifndef KICKOS_LIBC_STRING_H
#define KICKOS_LIBC_STRING_H

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

void* memcpy(void* dst, void const* src, size_t n);
void* memset(void* dst, int c, size_t n);
void* memmove(void* dst, void const* src, size_t n);
int memcmp(void const* a, void const* b, size_t n);
size_t strlen(char const* s);
size_t strnlen(char const* s, size_t maxlen);

#ifdef __cplusplus
}
#endif

#endif
