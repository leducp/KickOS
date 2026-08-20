// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// armv6m (Cortex-M0/M0+): saved thread SP, privilege posture, and the bounds the
// saved SP must stay inside (the register state lives on the thread's PSP stack).
// Kept as its own per-arch header, and included by switch.S for the field offsets.

#ifndef KICKOS_ARCH_CONTEXT_H
#define KICKOS_ARCH_CONTEXT_H

// The offsets switch.S reads as plain displacements, and the width of the block its two
// software pushes write below the live PSP. switch.S .equ's from these and arch_armv6m.cc
// static_asserts offsetof against them, so neither side can drift alone.
#define KICKOS_ARMV6M_CTX_OFF_STACK_LO 12
#define KICKOS_ARMV6M_CTX_OFF_STACK_HI 16
#define KICKOS_ARMV6M_CTX_OFF_TRACE_TID 20
#define KICKOS_ARMV6M_TRAP_FRAME 32

#ifndef __ASSEMBLER__

#include <stdint.h>

struct arch_context
{
    // Saved PSP: lowest word of this thread's saved frame (the PendSV-pushed
    // {r4-r11}, below the hardware exception frame). v6-M has no FPU, so no
    // EXC_RETURN is saved here: thread returns are always the non-FP 0xFFFFFFFD,
    // reconstructed on switch-in.
    uint32_t sp;

    // CONTROL.nPRIV: 0 = privileged (kernel), 1 = unprivileged (user). Saved on
    // switch-out and restored on switch-in like a register (a thread blocked
    // mid-syscall runs privileged and must resume so).
    uint32_t npriv;

    // Fixed resting privilege (set once at init); the SVC trampoline restores it
    // on syscall return so a privileged thread issuing a syscall is not demoted.
    uint32_t resting_npriv;

    // The thread's stack, checked by PendSV and by the SVC fastpath before either
    // pushes {r4-r11} through the live PSP. Exception entry stacks the hardware frame
    // ABOVE the PSP with the pre-exception privilege, so the MPU refuses that half; the
    // block below it is pushed in handler mode and is refused by nothing, and r10/r11
    // land on the stacked PC and xPSR of whatever sits under the stack. Read as plain
    // words at F_CTX_STACK_LO / F_CTX_STACK_HI in switch.S, which are UNCONDITIONAL
    // offsets: a telemetry-dependent pair would make the guard read trace_tid as a bound
    // in one build posture and pass a PSP it must refuse. An unseated pair reads as
    // [0, 0), which the upper bound refuses.
    uint32_t stack_lo;
    uint32_t stack_hi;

#if defined(KICKOS_TELEMETRY) && KICKOS_TELEMETRY
    // Owning thread's trace id (stamped in thread_create). switch.S reads it at
    // F_CTX_TRACE_TID from the physically-swapped contexts to emit the SWITCH record.
    // Elided when telemetry is off (OFF layout byte-unchanged).
    uint32_t trace_tid;
#endif
};

#endif // __ASSEMBLER__

#endif
