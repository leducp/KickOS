// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

// Linked only when KICKOS_FAULT_ISOLATION is set; otherwise the declining fallbacks in
// arch/common define these two symbols instead.

#include <kickos/arch/arch.h>
#include <kickos/arch/armv6m_fault_frame.h>

#include "../common/fault_resume.h"

extern "C"
{

// NOT ctx.resting_npriv: that says the thread is a user thread, while syscall dispatch
// runs PRIVILEGED in thread mode on the thread's KERNEL block (switch.S svc_trampoline),
// so a fault there is a kernel bug and CONTROL.nPRIV says so. Exception entry does not
// modify CONTROL.nPRIV, so reading it here gives the privilege at fault time. A non-zero
// stacked IPSR means the fault escalated from inside another handler: also a kernel bug.
//
// THE FRAME IS ALWAYS ON THE USER STACK on this arch, which is why the test below is the
// thread-stack one and not its kernel-block twin: the hardware stacks it at the PSP with
// the PRE-exception privilege, and an accepted fault here is by definition one taken with
// nPRIV set, which the dispatch never is.
//
// v6-M has NO CFSR, so armv7m's stacking-abort early-out has no equivalent and the
// stack-bounds test is the whole of frame validity here. Hardware stacking decrements SP
// before it writes, so a stacking abort hands over a frame below the thread's own stack,
// and a wild SP hands over one outside it.
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

// Entered by the exception return with r0 = the SP the stub must run on. Naked, and the
// SP move is the first instruction: anything the compiler put before it would run on the
// stack this exists to leave. Thread mode does not change SPSEL, so this writes the PSP,
// which is the stack the killed thread was running its own code on.
__attribute__((naked, noreturn)) void kickos_armv6m_fault_stack_reset(void)
{
    // Reached through a LITERAL, not `b`: the T2 `b` range is +/-2 KB and the target sits in a
    // different archive member (R_ARM_THM_JUMP11 relocation truncation). `bl` reaches +/-4 MB,
    // still a range the layout could outgrow silently; a PC-relative word plus `bx` has none.
    // r1 is free: this thread is dying and the frame has already been read.
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
    // v6-M has no fault-status and no fault-address register, so the dump is the PC
    // alone; a null name is what tells the printer there is no status word to name.
    kickos_fault_record(nullptr, 0, f[6], 0, 0);
    kickos_arm_fault_resume_at(f, &kickos_armv6m_fault_stack_reset);
    // v6-M has no IT/ICI state to clear. Forcing T is not redundant: a cleared T bit is
    // itself one of the ways a thread arrives here.
    f[7] = f[7] | (1u << 24);
    kickos_arm_fault_resume_privileged();
}

}
