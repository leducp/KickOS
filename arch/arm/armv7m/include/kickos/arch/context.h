// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// armv7m: on Cortex-M the register state lives on the thread's own PSP stack (hardware
// exception frame + the PendSV-saved callee registers), so the context holds the
// top-of-saved-frame pointer, the CONTROL.nPRIV posture, and the bounds that pointer must
// stay inside.

#ifndef KICKOS_ARCH_CONTEXT_H
#define KICKOS_ARCH_CONTEXT_H

#include <stdint.h>

struct arch_context
{
    // Saved PSP: points at the lowest word of this thread's saved frame
    // (the PendSV-pushed {r4-r11, EXC_RETURN}, below the hardware frame).
    // Updated by PendSV on switch-out; read by PendSV/arch_start on switch-in.
    uint32_t sp;

    // CONTROL.nPRIV for this thread: 0 = privileged (kernel), 1 = unprivileged
    // (user). Saved on switch-out and restored on switch-in like a register, and
    // distinct from the resting privilege: a thread blocked mid-syscall runs
    // privileged (the SVC trampoline raised it), so its saved nPRIV is 0 and it
    // must resume privileged. arch_context_init seeds it with the thread's resting
    // privilege. SPSEL is always 1 (threads run on PSP) and FPCA is left to hardware.
    uint32_t npriv;

    // The thread's fixed resting privilege (set once at init, never changed by a
    // switch). The SVC trampoline restores this on syscall return so a privileged
    // thread issuing a syscall is NOT demoted to unprivileged.
    uint32_t resting_npriv;

    // The thread's stack, checked by PendSV and by SVC_Handler before either pushes
    // {r4-r11, EXC_RETURN} through the live PSP. Set once by arch_context_init; read as
    // plain words at F_CTX_STACK_LO / F_CTX_STACK_HI in switch.S, which are
    // UNCONDITIONAL offsets: a telemetry-dependent pair would make the guard read
    // trace_tid as a bound in one build posture and pass a PSP it must refuse.
    uint32_t stack_lo;
    uint32_t stack_hi;

#if defined(KICKOS_TELEMETRY) && KICKOS_TELEMETRY
    // Owning thread's trace id (stamped once in thread_create). switch.S reads it
    // at offset 20 from the PHYSICALLY-swapped contexts to emit the SWITCH record
    // without re-reading g_arch_next.
    uint32_t trace_tid;
#endif
};

#endif
