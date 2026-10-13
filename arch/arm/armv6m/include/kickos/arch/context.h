// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// armv6m (Cortex-M0/M0+): the register state lives on the thread's own PSP stack, so the
// context holds the saved SP, the privilege posture, and the bounds that SP must stay
// inside. The offsets switch.S reads them at live in armv6m_trap_stack.h, beside the
// figures the guard that reads them enforces.

#ifndef KICKOS_ARCH_CONTEXT_H
#define KICKOS_ARCH_CONTEXT_H

#ifndef __ASSEMBLER__

// Bytes the TLS carve carries on top of .tdata + .tbss: AAPCS TLS is variant 1, tp pointing at
// a reserved two-word TCB with the first thread_local above it.
#define KICKOS_ARCH_TLS_TCB 8

#include <stdint.h>

struct arch_context
{
    // Saved PSP: the lowest word of the PendSV-pushed {r4-r11} block. EXC_RETURN is always
    // 0xFFFFFFFD, rebuilt on switch-in.
    uint32_t sp;

    // CONTROL.nPRIV, saved and restored like a register: a thread blocked mid-syscall runs
    // privileged and must resume so.
    uint32_t npriv;

    // The fixed resting privilege, restored on syscall return so a privileged thread is not
    // demoted.
    uint32_t resting_npriv;

    // The PSP guard's bounds, read as plain words at F_CTX_STACK_LO / F_CTX_STACK_HI in
    // switch.S. The offsets are UNCONDITIONAL: a telemetry-dependent pair would make the guard
    // read trace_tid as a bound in one posture. An unseated pair reads [0, 0), which the upper
    // bound refuses.
    uint32_t stack_lo;
    uint32_t stack_hi;

    // TOP of the kernel stack this thread's privileged dispatch runs on, seated once by
    // thread_create and preserved across arch_ctx_redirect; never saved by a switch. PendSV's
    // guard accepts a PSP inside the block below it as well as one inside the user stack,
    // because a thread preempted mid-dispatch runs there. Zero for a TCB outside the pool.
    uintptr_t kernel_sp;

#if defined(KICKOS_TELEMETRY) && KICKOS_TELEMETRY
    // Owning thread's trace id; switch.S reads it from the physically swapped contexts to
    // emit the SWITCH record.
    uint32_t trace_tid;
#endif
};

#endif // __ASSEMBLER__

#endif
