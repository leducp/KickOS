// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include <stddef.h>
#include <string.h>

#include <kickos/arch/arch.h>

#include "dcache_seam.h"

namespace kickos
{
    namespace testfix
    {
        namespace
        {
            DcacheOp g_ops[DCACHE_OPS_MAX];
            size_t g_count = 0;
            unsigned char const* g_expect = nullptr;
            void const* g_watch = nullptr;
        }

        void dcache_reset(unsigned char const* expect, void const* watch)
        {
            g_count = 0;
            g_expect = expect;
            g_watch = watch;
        }

        size_t dcache_count()
        {
            return g_count;
        }

        DcacheOp const& dcache_op(size_t i)
        {
            return g_ops[i];
        }
    }
}

extern "C"
{
    void arch_dcache_invalidate(void* addr, size_t bytes)
    {
        using namespace kickos::testfix;
        if (g_count < DCACHE_OPS_MAX)
        {
            bool landed = false;
            if (g_expect != nullptr and g_watch != nullptr)
            {
                landed = memcmp(g_watch, g_expect, bytes) == 0;
            }
            else if (g_expect != nullptr)
            {
                landed = memcmp(addr, g_expect, bytes) == 0;
            }
            g_ops[g_count] = {addr, bytes, landed};
        }
        g_count++;
    }
}
