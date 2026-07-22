// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Xtensa LX6 (windowed ABI): struct arch_context is the minimal state needed to
// resume a thread that suspended inside the cooperative switch (switch.S). The
// windowed ABI keeps the bulk of a thread's register state in the physical AR
// window file; the switch flushes ALL live windows of the outgoing thread to its
// OWN stack (SPILL_ALL_WINDOWS) before the SP swap, so on resume only three words
// are needed -- the rest is reloaded lazily from the stack by the window-underflow
// handler (startup.S) as the thread returns up its call chain. See switch.S.
//
// No npriv/resting_npriv field (unlike the ARM ports): the classic ESP32 core has
// no privileged/unprivileged CPU split, so KickOS runs all-privileged and a
// syscall is a plain call. Privilege is a no-op on this arch.

#ifndef KICKOS_ARCH_CONTEXT_H
#define KICKOS_ARCH_CONTEXT_H

#include <stdint.h>

// resume_kind discriminator (below). A cooperatively-suspended thread is resumed
// by the windowed `retw` tail (sp/ps/pc + spilled stack); a thread preempted by
// an interrupt is resumed by `rfe` from its full 256-byte interrupt frame. The
// two save formats are irreconcilable (a preempted thread holds live caller-saved
// registers at an arbitrary PC that the 3-word form cannot capture), so the
// resume path is selected per thread by this field. switch.S / startup.S hard-
// code the value AND the offset (12); keep KICKOS_RESUME_* in sync with the asm.
#define KICKOS_RESUME_COOP 0 /* retw tail (xtensa_switch save format)         */
#define KICKOS_RESUME_IRQ  1 /* rfe from the interrupt frame (startup.S save) */

struct arch_context
{
    // Saved a1 (stack pointer). For a COOP thread: the suspended cooperative-
    // switch frame pointer (retw resumes from here). For an IRQ thread: the base
    // of the 256-byte interrupt frame on the thread's own stack. switch.S/startup.S
    // hard-code this at offset 0.
    uint32_t sp;

    // Saved PS (INTLEVEL / EXCM / UM / WOE). COOP only: restored on switch-in like
    // a register, so a thread that blocked inside an IrqLock resumes with interrupts
    // still masked, then its arch_irq_restore lowers the level. (An IRQ thread's PS
    // lives in its interrupt frame, not here.) Offset 4.
    uint32_t ps;

    // Saved return address (a0) WITH its top-2-bit window CALLINC field intact --
    // the value the resuming `retw` uses to rotate the window back to the caller
    // and drive the underflow reload. COOP only. Offset 8.
    uint32_t pc;

    // Which resume path reconstitutes this thread: KICKOS_RESUME_COOP (set by the
    // xtensa_switch save when a running thread blocks) or KICKOS_RESUME_IRQ (set by
    // the level-1 interrupt exit when it preempts this thread, AND by
    // arch_context_init for a FRESH thread -- a fresh thread is started via the same
    // rfe restore path as a preempted one, from a fabricated interrupt frame, so its
    // outermost trampoline frame is a real `entry`-established window rather than a
    // phantom retw target). Offset 12.
    uint32_t resume_kind;

#if defined(KICKOS_TELEMETRY) && KICKOS_TELEMETRY
    // Telemetry only: the owning thread's trace id, stamped once by
    // arch_trace_stamp_id (thread_create) and read by the switch path to emit the
    // {from,to} SWITCH record from the physically-swapped contexts. OFFSET 16 --
    // switch.S / startup.S hard-code it. Elided with the whole telemetry path.
    uint32_t trace_tid;
#endif
};

#endif
