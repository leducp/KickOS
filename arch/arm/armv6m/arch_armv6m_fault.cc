// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

// Linked only when KICKOS_FAULT_ISOLATION is set; otherwise the declining fallbacks in
// arch/common define these two symbols instead.

#include <kickos/arch/arch.h>
#include <kickos/arch/armv6m_fault_frame.h>

#include "../common/aeabi_div0.h"
#include "../common/fault_resume.h"

extern "C"
{

// CONTROL.nPRIV and NOT ctx.resting_npriv: syscall dispatch runs PRIVILEGED in thread mode on
// the thread's KERNEL block (switch.S svc_trampoline), so a fault there is a kernel bug. Thread
// mode on the PSP is the reporter's EXC_RETURN test, never the stacked IPSR, which a task-mate
// writes.
//
// The frame is never what decides: the exit is built whole at kickos_fault_stack_top.
bool arch_fault_is_user_thread(void* frame)
{
    uint32_t control;
    __asm volatile("mrs %0, control" : "=r"(control));
    if ((control & 1u) == 0u)
    {
        return false;
    }
    (void)frame;
    return kickos_arm_fault_exit_top() != 0;
}

void arch_fault_redirect_to_exit(void* frame)
{
    uint32_t* const f = static_cast<uint32_t*>(frame);
    // A null name tells the printer there is no status word: v6-M latches none, and a divide by
    // zero is known by the hook's UDF alone. v6-M has NO CFSR, so the bounds test is the whole
    // of frame validity: a stacking abort hands over a frame below the thread's own stack, a
    // wild SP one outside it.
    if (not kickos_fault_frame_trusted(frame, ARMV6M_BASIC_FRAME_BYTES))
    {
        kickos_fault_record_no_pc(nullptr, 0, 0, 0);
    }
    else if (kickos_arm_div0_trap_at(f[6]))
    {
        kickos_fault_record("DIVBYZERO", 1, f[6], 0, 0);
    }
    else
    {
        kickos_fault_record(nullptr, 0, f[6], 0, 0);
    }
    // An SVC whose entry stacking faulted is taken on the exit's frame as this handler returns,
    // and SHCSR cannot drop it here: on the RP2040 it reads 0 and ignores a write from software.
    // SVC_Handler drops it instead (switch.S, .Lpsp_wild_svc).
    kickos_arm_fault_frame_at(kickos_arm_fault_exit_top());
    kickos_arm_fault_resume_privileged();
}

}
