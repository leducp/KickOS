// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// armv7m: struct arch_context is just the saved thread stack pointer plus the
// thread's CONTROL.nPRIV posture. On Cortex-M the register state lives on the
// thread's own PSP stack (hardware exception frame + the PendSV-saved callee
// registers); the TCB only needs the top-of-saved-frame pointer to resume it.
//
// This is intentionally tiny (8 bytes) versus the sim's 2 KiB ucontext blob:
// the ARM "context" is the stack, not a separate blob.

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
    // (user). Saved on switch-out and restored on switch-in like a register --
    // NOT just the resting privilege: a thread blocked mid-syscall runs
    // privileged (the SVC trampoline raised it), so its saved nPRIV is 0 and it
    // must resume privileged (the ARM twin of the sim's SimContext::raised).
    // arch_context_init seeds it with the thread's resting privilege. SPSEL is
    // always 1 (threads run on PSP) and FPCA is left to hardware.
    uint32_t npriv;

    // The thread's fixed resting privilege (set once at init, never changed by a
    // switch). The SVC trampoline restores this on syscall return so a privileged
    // thread issuing a syscall is NOT demoted to unprivileged -- it drops back to
    // exactly its entry posture, matching the sim's resting-posture restore.
    uint32_t resting_npriv;
};

#endif
