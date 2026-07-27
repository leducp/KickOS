// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// KICKOS_DEBUG_ASSERT: an internal-consistency check that is NOT carried in a shipped
// image. Distinct from KICKOS_ASSERT (kernel.h), which is always live because it guards
// an invariant whose violation is unrecoverable at runtime -- these guard mistakes a
// caller could make, where the value is catching them AT the mistake in a build set up
// to look, not paying for the check on every board forever.
//
// A deliberately tiny leaf: it declares kpanic itself rather than including kernel.h,
// because kernel.h pulls thread.h which pulls list.h -- one of this header's own users.

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
