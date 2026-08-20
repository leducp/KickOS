// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// RISC-V RV32IMAC: struct arch_context is the minimal state the switcher needs to
// resume a thread. ALL register state lives in a flat save frame on the thread's own
// stack (switch.S), so a thread is fully described by one pointer, the top of that
// frame. There is ONE frame format for both a voluntary block and a preemptive wake:
// the msip switcher always saves the complete interrupted context (every GPR except
// gp/tp, plus mepc + mstatus; sp is the frame base held in ctx.sp), so a thread
// preempted at an arbitrary PC and one that blocked in a syscall are indistinguishable
// to the resume path.
//
// There is no npriv/resting_npriv field: a thread's privilege lives in the saved
// frame's mstatus.MPP, restored by the mret at frame-restore.

#ifndef KICKOS_ARCH_CONTEXT_H
#define KICKOS_ARCH_CONTEXT_H

#include <stdint.h>

struct arch_context
{
    // Saved stack pointer: the base (lowest address) of the thread's current save
    // frame. switch.S restores every register + mepc + mstatus from here and mret's
    // back in, and hard-codes this field at offset 0.
    uint32_t sp;

#if defined(KICKOS_TELEMETRY) && KICKOS_TELEMETRY
    // Telemetry only: the owning thread's trace id, stamped once by
    // arch_trace_stamp_id (thread_create) and read by the switch path to emit the
    // {from,to} SWITCH record from the physically-swapped contexts. switch.S
    // hard-codes it at OFFSET 4.
    uint32_t trace_tid;
#endif

    // Stack bounds trap_entry (switch.S) checks the interrupted U-mode sp against
    // before it stores a frame through it: a U-mode thread owns its sp and can aim it
    // at kernel memory, which the software M-mode prologue would otherwise write. Set
    // once by arch_context_init; read as plain words at the offsets F_CTX_STACK_LO /
    // F_CTX_STACK_HI hard-coded in switch.S. A sp outside [stack_lo, stack_hi] routes
    // the trap to the fault reporter instead of storing.
    uint32_t stack_lo;
    uint32_t stack_hi;
};

#endif
