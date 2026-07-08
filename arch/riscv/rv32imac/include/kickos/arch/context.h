// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// RISC-V RV32IMAC: struct arch_context is the minimal state the switcher needs to
// resume a thread. Unlike the windowed Xtensa port, RISC-V keeps ALL register
// state in a flat save frame on the thread's own stack (switch.S), so a thread is
// fully described by one pointer -- the top of that frame. There is ONE frame
// format for both a voluntary block and a preemptive wake (the RX single-format
// SWINT model): the msip switcher always saves the complete interrupted context
// (all GPRs + mepc + mstatus), so a thread preempted at an arbitrary PC and one
// that blocked in a syscall are indistinguishable to the resume path.
//
// No npriv/resting_npriv field (unlike the ARM ports): a thread's privilege lives
// in the saved frame's mstatus.MPP, restored by the mret at frame-restore -- so a
// syscall's resting privilege is carried by the frame, not a side field.

#ifndef KICKOS_ARCH_CONTEXT_H
#define KICKOS_ARCH_CONTEXT_H

#include <stdint.h>

struct arch_context
{
    // Saved stack pointer: the base (lowest address) of the thread's current save
    // frame -- switch.S restores every register + mepc + mstatus from here and
    // mret's back in. switch.S hard-codes this at offset 0.
    uint32_t sp;

#if defined(KICKOS_TELEMETRY) && KICKOS_TELEMETRY
    // Telemetry only: the owning thread's trace id, stamped once by
    // arch_trace_stamp_id (thread_create) and read by the switch path to emit the
    // {from,to} SWITCH record from the physically-swapped contexts. OFFSET 4 --
    // switch.S hard-codes it. Elided with the whole telemetry path.
    uint32_t trace_tid;
#endif
};

#endif
