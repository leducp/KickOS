// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The threads model's types, as the KickOS toolchain's gthr-kickos.h states them.

#ifndef KICKOS_TESTS_UNIT_GTHRTIMED_TOOLCHAIN_BITS_GTHR_H
#define KICKOS_TESTS_UNIT_GTHRTIMED_TOOLCHAIN_BITS_GTHR_H

#include <time.h>

typedef struct
{
    __UINT32_TYPE__ __word;
} __gthread_mutex_t;

typedef struct
{
    __UINT32_TYPE__ __word;
    __UINT32_TYPE__ __depth;
} __gthread_recursive_mutex_t;

typedef struct
{
    __UINT32_TYPE__ __seq;
} __gthread_cond_t;

typedef __UINT32_TYPE__ __gthread_key_t;

typedef __UINT32_TYPE__ __gthread_t;

typedef struct timespec __gthread_time_t;

#endif
