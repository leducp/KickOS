// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// KICKOS_DEBUG_ASSERT: an internal-consistency check compiled out of a shipped image.
// KICKOS_ASSERT (kernel.h) is always live because it guards an invariant that cannot be
// recovered from at runtime. These guard mistakes a caller could make instead.
//
// Declares kpanic rather than including kernel.h, which pulls thread.h and then list.h,
// one of this header's own users.

#ifndef KICKOS_DEBUG_H
#define KICKOS_DEBUG_H

#ifndef KICKOS_DEBUG
#define KICKOS_DEBUG 0
#endif

#if KICKOS_DEBUG
namespace kickos
{
    // Same declaration as kernel.h; see the header comment for why it is repeated.
    void kpanic(char const* msg) __attribute__((noreturn));
}
#define KICKOS_DEBUG_ASSERT(cond)                     \
    do                                                \
    {                                                 \
        if (not(cond))                                \
        {                                             \
            ::kickos::kpanic("debug assert: " #cond); \
        }                                             \
    } while (0)
#else
#define KICKOS_DEBUG_ASSERT(cond) ((void)0)
#endif

#endif
