// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The rebuild every block-carving backend's arch_ctx_redirect performs.

#ifndef KICKOS_ARCH_COMMON_CTX_REDIRECT_H
#define KICKOS_ARCH_COMMON_CTX_REDIRECT_H

#include <kickos/arch/arch.h>
#include <kickos/board_config.h>

#include <stddef.h>
#include <stdint.h>

// Rebuilds ctx privileged at entry at the TOP of the thread's own kernel block, discarding
// whatever dispatch frames it held, so no privileged frame lands on memory the thread or a
// domain sibling can write and the block requirement is the MAX of the dispatch and exit
// classes rather than their sum. A TCB outside the pool has no block and is rebuilt on the
// stack it is handed.
//
// kernel_sp is put back explicitly: lost, the thread carries 0 through its own teardown and
// every trap on the way takes the entry's refusal path. stack_lo and stack_hi are put back
// because arch_context_init derives them from what it is handed, and handing it the block
// would leave the context describing kernel .bss as this thread's stack. Any other field
// arch_context_init derives from the stack is the backend's to keep around this call.
inline void arch_ctx_redirect_to_block(struct arch_context* ctx, void (*entry)(void* arg),
                                       void* stack_base, size_t stack_size)
{
    uintptr_t const kernel_sp = ctx->kernel_sp;
#if KICKOS_KERNEL_STACKS
    if (kernel_sp != 0)
    {
        auto const lo = ctx->stack_lo;
        auto const hi = ctx->stack_hi;
        void* const block = reinterpret_cast<void*>(kernel_sp - KICKOS_KERNEL_STACK_SIZE);
        arch_context_init(ctx, entry, nullptr, block, KICKOS_KERNEL_STACK_SIZE, 1);
        ctx->stack_lo = lo;
        ctx->stack_hi = hi;
        ctx->kernel_sp = kernel_sp;
        return;
    }
#endif
    arch_context_init(ctx, entry, nullptr, stack_base, stack_size, 1);
    ctx->kernel_sp = kernel_sp;
}

#endif
