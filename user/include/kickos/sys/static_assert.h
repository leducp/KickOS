// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// One static assertion spelling for the headers that serve both C and C++.

#ifndef KICKOS_SYS_STATIC_ASSERT_H
#define KICKOS_SYS_STATIC_ASSERT_H

#ifdef __cplusplus
#define KOS_STATIC_ASSERT(cond, why) static_assert(cond, why)
#define KOS_ALIGNOF(type) alignof(type)
#else
#define KOS_STATIC_ASSERT(cond, why) _Static_assert(cond, why)
#define KOS_ALIGNOF(type) _Alignof(type)
#endif

#endif
