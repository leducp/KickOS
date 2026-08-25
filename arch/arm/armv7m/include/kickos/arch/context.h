// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// armv7m: on Cortex-M the register state lives on the thread's own PSP stack (hardware
// exception frame + the PendSV-saved callee registers), so the context holds the
// top-of-saved-frame pointer, the CONTROL.nPRIV posture, and the bounds that pointer must
// stay inside.

#ifndef KICKOS_ARCH_CONTEXT_H
#define KICKOS_ARCH_CONTEXT_H

#ifndef __ASSEMBLER__

// Bytes the ABI reserves BELOW the thread pointer, which the TLS carve has to
// carry on top of .tdata + .tbss.
// M-profile AAPCS TLS is variant 1: tp points at a reserved two-word TCB
// and the first thread_local sits above it. __aeabi_read_tp returns tp, and the
// compiler adds the offset the linker computed.
#define KICKOS_ARCH_TLS_TCB 8


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

    // TOP of the kernel stack this thread's privileged dispatch runs on, seated once per pool
    // thread by thread_create and preserved across arch_ctx_redirect. NOT saved by a switch:
    // it is write-once per slot and every switch-path reference is a load, `sp` above being
    // the one field that tracks where the thread is. svc_trampoline relocates sp onto this
    // before it calls anything, and PendSV's guard accepts a PSP inside the block below it as
    // well as one inside the user stack, because a thread preempted mid-dispatch is running
    // there. Read at F_CTX_KERNEL_SP in switch.S. Zero for a TCB outside the pool, which
    // reaches neither site.
    uintptr_t kernel_sp;

#if defined(KICKOS_TELEMETRY) && KICKOS_TELEMETRY
    // Owning thread's trace id (stamped once in thread_create). switch.S reads it
    // at offset 20 from the PHYSICALLY-swapped contexts to emit the SWITCH record
    // without re-reading g_arch_next.
    uint32_t trace_tid;
#endif
};

#endif // __ASSEMBLER__

#endif
