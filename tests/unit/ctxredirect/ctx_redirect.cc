// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A slain thread with a kernel block is rebuilt privileged at the block's top, and keeps the
// stack bounds and kernel_sp it had, over an arch_context_init that derives all three as a
// backend's does.

#include <kickos/arch/arch.h>
#include <kickos/board_config.h>

#include <gtest/gtest.h>

#include <stddef.h>
#include <stdint.h>

#undef KICKOS_KERNEL_STACKS
#define KICKOS_KERNEL_STACKS 1
#undef KICKOS_KERNEL_STACK_SIZE
#define KICKOS_KERNEL_STACK_SIZE 0x800u
#define arch_context ctxr_context
#define arch_context_init ctxr_context_init

struct ctxr_context
{
    uintptr_t kernel_sp;
    uintptr_t stack_lo;
    uintptr_t stack_hi;
};

namespace
{
    struct
    {
        int calls;
        void* base;
        size_t size;
        int privileged;
    } g_init;
}

void ctxr_context_init(ctxr_context* ctx, void (*)(void*), void*, void* base, size_t size,
                       int privileged)
{
    g_init = {g_init.calls + 1, base, size, privileged};
    ctx->stack_lo = reinterpret_cast<uintptr_t>(base);
    ctx->stack_hi = reinterpret_cast<uintptr_t>(base) + size;
    ctx->kernel_sp = 0;
}

#include "ctx_redirect.h"

namespace
{
    void entry(void*) {}

    TEST(CtxRedirect, ABlockedThreadIsRebuiltPrivilegedAtItsBlockTopAndKeepsItsBounds)
    {
        ctxr_context ctx{0x9000u, 0x1000u, 0x2000u};
        arch_ctx_redirect_to_block(&ctx, entry, reinterpret_cast<void*>(0x1000u), 0x1000u);
        EXPECT_EQ(g_init.calls, 1);
        EXPECT_EQ(g_init.base, reinterpret_cast<void*>(0x9000u - KICKOS_KERNEL_STACK_SIZE));
        EXPECT_EQ(g_init.size, KICKOS_KERNEL_STACK_SIZE);
        EXPECT_EQ(g_init.privileged, 1);
        EXPECT_EQ(ctx.kernel_sp, 0x9000u);
        EXPECT_EQ(ctx.stack_lo, 0x1000u);
        EXPECT_EQ(ctx.stack_hi, 0x2000u);
    }
}
