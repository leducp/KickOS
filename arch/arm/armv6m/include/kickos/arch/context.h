// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// armv6m (Cortex-M0/M0+): saved thread SP + privilege posture, identical shape
// to the armv7m context (the register state lives on the thread's PSP stack).
// Kept as its own per-arch header so switch.S can hard-code the field offsets.

#ifndef KICKOS_ARCH_CONTEXT_H
#define KICKOS_ARCH_CONTEXT_H

#include <stdint.h>

struct arch_context
{
    // Saved PSP: lowest word of this thread's saved frame (the PendSV-pushed
    // {r4-r11}, below the hardware exception frame). v6-M has no FPU, so -- unlike
    // armv7m -- no EXC_RETURN is saved here (thread returns are always the
    // non-FP 0xFFFFFFFD, reconstructed on switch-in).
    uint32_t sp;

    // CONTROL.nPRIV: 0 = privileged (kernel), 1 = unprivileged (user). Saved on
    // switch-out and restored on switch-in like a register (a thread blocked
    // mid-syscall runs privileged and must resume so).
    uint32_t npriv;

    // Fixed resting privilege (set once at init); the SVC trampoline restores it
    // on syscall return so a privileged thread issuing a syscall is not demoted.
    uint32_t resting_npriv;

#if defined(KICKOS_TELEMETRY) && KICKOS_TELEMETRY
    // Owning thread's trace id (stamped in thread_create). switch.S reads it at
    // offset 12 from the physically-swapped contexts to emit the SWITCH record.
    // Elided when telemetry is off (OFF layout byte-unchanged).
    uint32_t trace_tid;
#endif
};

#endif
