// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Xtensa LX6 (windowed ABI): the state that resumes a thread. The cooperative switch
// (switch.S) spills every live window of the outgoing thread to its own stack first, so a
// COOP resume needs only sp/ps/pc and the window-underflow handler (startup.S) reloads the
// rest from the stack as the thread returns up its call chain.
//
// switch.S and startup.S hard-code every field offset and both KICKOS_RESUME_* values below;
// arch_xtensa.cc static_asserts the offsets.

#ifndef KICKOS_ARCH_CONTEXT_H
#define KICKOS_ARCH_CONTEXT_H

// Xtensa TLS is variant 1: THREADPTR points at a reserved two-word TCB and the first
// thread_local sits above it, so the carve owes these bytes on top of .tdata + .tbss.
#define KICKOS_ARCH_TLS_TCB 8

#include <stdint.h>

// A COOP thread resumes through the windowed `retw` tail from sp/ps/pc; an IRQ thread resumes
// through `rfe` from its full 256-byte interrupt frame. A fresh thread is IRQ: it starts from a
// fabricated interrupt frame, so its outermost trampoline frame is a real `entry` window.
#define KICKOS_RESUME_COOP 0
#define KICKOS_RESUME_IRQ  1

struct arch_context
{
    // COOP: the suspended switch frame. IRQ: the base of the interrupt frame.
    uint32_t sp;

    // COOP only, restored like a register: a thread that blocked inside an IrqLock resumes
    // still masked, and its arch_irq_restore lowers the level.
    uint32_t ps;

    // COOP only: a0 WITH its top-2-bit CALLINC field, which the resuming `retw` needs to
    // rotate the window back to the caller.
    uint32_t pc;

    uint32_t resume_kind;

#if defined(KICKOS_TELEMETRY) && KICKOS_TELEMETRY
    // Stamped once by arch_trace_stamp_id; the switch path reads it to emit the SWITCH record.
    uint32_t trace_tid;
#endif

#if defined(KICKOS_TLS) && KICKOS_TLS
    // THREADPTR while this thread runs, written by every switch-in and never read back from
    // the register, which any code can write. Write-once per thread.
    uint32_t tls_base;
#endif
};

#endif
