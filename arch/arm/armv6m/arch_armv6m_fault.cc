// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

// Linked only when KICKOS_FAULT_ISOLATION is set; otherwise the declining fallbacks in
// arch/common define these two symbols instead.

#include <kickos/arch/arch.h>
#include <kickos/arch/armv6m_fault_frame.h>

#include "../common/fault_resume.h"

extern "C"
{

// CONTROL.nPRIV and NOT ctx.resting_npriv: syscall dispatch runs PRIVILEGED in thread mode on
// the thread's KERNEL block (switch.S svc_trampoline), so a fault there is a kernel bug. A
// non-zero stacked IPSR means the fault escalated from inside another handler: also a kernel
// bug.
//
// THE FRAME IS ALWAYS ON THE USER STACK on this arch, so the test below is the thread-stack
// one, not its kernel-block twin.
//
// v6-M has NO CFSR, so the stack-bounds test is the whole of frame validity: a stacking abort
// hands over a frame below the thread's own stack, a wild SP one outside it.
bool arch_fault_is_user_thread(void* frame)
{
    uint32_t control;
    __asm volatile("mrs %0, control" : "=r"(control));
    if ((control & 1u) == 0u)
    {
        return false;
    }
    if (not kickos_fault_frame_trusted(frame, ARMV6M_BASIC_FRAME_BYTES))
    {
        return false;
    }
    uint32_t const* const f = static_cast<uint32_t const*>(frame);
    return (f[7] & 0x3Fu) == 0u; // stacked xPSR IPSR field (6 bits on v6-M)
}

// Entered by the exception return with r0 = the SP the stub must run on. The SP move must be
// the first instruction: anything before it would run on the stack this exists to leave.
// Thread mode does not change SPSEL, so this writes the PSP.
__attribute__((naked, noreturn)) void kickos_armv6m_fault_stack_reset(void)
{
    // A literal and `bx`, not `b`: the T2 `b` reaches +/-2 KB and the target is in another
    // archive member (R_ARM_THM_JUMP11 truncation).
    __asm volatile("mov  sp, r0\n\t"
                   "ldr  r1, 1f\n\t"
                   "bx   r1\n\t"
                   ".align 2\n"
                   "1:\n\t"
                   ".word kickos_thread_fault_exit");
}

void arch_fault_redirect_to_exit(void* frame)
{
    uint32_t* const f = static_cast<uint32_t*>(frame);
    // A null name tells the printer there is no status word.
    kickos_fault_record(nullptr, 0, f[6], 0, 0);
    kickos_arm_fault_resume_at(f, &kickos_armv6m_fault_stack_reset);
    // v6-M has no IT/ICI state to clear. Forcing T is not redundant: a cleared T bit is
    // itself one of the ways a thread arrives here.
    f[7] = f[7] | (1u << 24);
    kickos_arm_fault_resume_privileged();
}

}
