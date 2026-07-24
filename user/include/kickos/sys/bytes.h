// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Freestanding byte-copy / byte-zero primitives, static inline so a caller gets the
// same open-coded loop it would have written by hand (no libc, no call into the
// implicit memcpy/memset lowering). Shared by the userspace bus drivers and the
// chip-neutral client so the tiny loop is defined once. Distinct from
// <kickos/libc/string.h> (the standard-named memcpy/memset the compiler resolves
// against): these carry the kos-local names the drivers marshal with.

#ifndef KICKOS_SYS_BYTES_H
#define KICKOS_SYS_BYTES_H

#include <stddef.h>

static inline void mem_copy(void* dst, void const* src, size_t n)
{
    unsigned char* d = static_cast<unsigned char*>(dst);
    unsigned char const* s = static_cast<unsigned char const*>(src);
    for (size_t i = 0; i < n; i++)
    {
        d[i] = s[i];
    }
}

static inline void mem_zero(void* dst, size_t n)
{
    unsigned char* d = static_cast<unsigned char*>(dst);
    for (size_t i = 0; i < n; i++)
    {
        d[i] = 0;
    }
}

#endif
