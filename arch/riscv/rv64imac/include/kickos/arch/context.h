// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#ifndef KICKOS_ARCH_CONTEXT_H
#define KICKOS_ARCH_CONTEXT_H

// RISC-V TLS is variant 2: tp IS the block start and the first thread_local sits AT it, so
// reserving bytes below the thread pointer would put every offset wrong.
#define KICKOS_ARCH_TLS_TCB 0

#include <stdint.h>

struct arch_context
{
    // Saved stack pointer: the base (lowest address) of the thread's current save frame.
    uintptr_t sp;

#if defined(KICKOS_TELEMETRY) && KICKOS_TELEMETRY
    uint32_t trace_tid;
#endif

    uintptr_t stack_lo;
    uintptr_t stack_hi;

#if defined(KICKOS_TLS) && KICKOS_TLS
    // tp while this thread runs. Written once per thread, so no switch saves it.
    uintptr_t tls_base;
#endif

    // TOP of this thread's kernel stack, seated by thread_create BEFORE arch_context_init and
    // preserved across arch_ctx_redirect. Zero for a TCB outside the pool.
    uintptr_t kernel_sp;
};

#endif
