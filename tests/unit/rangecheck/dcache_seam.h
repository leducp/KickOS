// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// arch_dcache_invalidate for the copy-seam gates: every call recorded, none performed.
//
// Keep this header GTEST-FREE, for the reason kfixture.h states.

#ifndef KICKOS_TESTS_UNIT_RANGECHECK_DCACHE_SEAM_H
#define KICKOS_TESTS_UNIT_RANGECHECK_DCACHE_SEAM_H

#include <stddef.h>

namespace kickos
{
    namespace testfix
    {
        struct DcacheOp
        {
            void const* at;
            size_t n;
            // Whether the watched bytes already held the expected ones when the call was made.
            bool landed;
        };

        constexpr size_t DCACHE_OPS_MAX = 8;

        // Forget every call, and compare against `expect` (null compares nothing) the bytes at
        // `watch` at each later one, or with a null `watch` the bytes the call names.
        void dcache_reset(unsigned char const* expect, void const* watch = nullptr);
        // Calls since the last reset, recorded or not.
        size_t dcache_count();
        // The i-th call since the last reset, for i below DCACHE_OPS_MAX.
        DcacheOp const& dcache_op(size_t i);
    }
}

#endif
