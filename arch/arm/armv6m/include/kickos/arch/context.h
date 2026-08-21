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

#include <stdint.h>

struct arch_context
{
    // Saved PSP: lowest word of this thread's saved frame (the PendSV-pushed
    // {r4-r11}, below the hardware exception frame). Thread returns are always the
    // non-FP 0xFFFFFFFD, reconstructed on switch-in rather than kept in the frame.
    uint32_t sp;

    // CONTROL.nPRIV: 0 = privileged (kernel), 1 = unprivileged (user). Saved on
    // switch-out and restored on switch-in like a register (a thread blocked
    // mid-syscall runs privileged and must resume so).
    uint32_t npriv;

    // Fixed resting privilege (set once at init); the SVC trampoline restores it
    // on syscall return so a privileged thread issuing a syscall is not demoted.
    uint32_t resting_npriv;

    // The thread's stack, checked by PendSV and by the SVC fastpath before either
    // pushes {r4-r11} through the live PSP. Read as plain words at F_CTX_STACK_LO /
    // F_CTX_STACK_HI in switch.S, which are UNCONDITIONAL offsets: a telemetry-dependent
    // pair would make the guard read trace_tid as a bound in one build posture and pass a
    // PSP it must refuse. An unseated pair reads as [0, 0), which the upper bound refuses.
    uint32_t stack_lo;
    uint32_t stack_hi;

    // The kernel stack this thread's privileged dispatch runs on, saved across a switch the
    // way `sp` above saves the USER one. `sp` is and stays the user stack pointer on every
    // backend, so no second field names it; these two are the pair a trusted entry swaps
    // between. Unused until the trap entry transfers to it: the field exists here first so
    // the allocation and the geometry can be reviewed apart from the execution change.
    uint32_t kernel_sp;

#if defined(KICKOS_TELEMETRY) && KICKOS_TELEMETRY
    // Owning thread's trace id (stamped in thread_create). switch.S reads it at
    // F_CTX_TRACE_TID from the physically-swapped contexts to emit the SWITCH record.
    uint32_t trace_tid;
#endif
};

#endif // __ASSEMBLER__

#endif
