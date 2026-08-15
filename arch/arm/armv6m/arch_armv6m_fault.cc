// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

// Linked only when KICKOS_FAULT_ISOLATION is set; otherwise the declining fallbacks in
// arch/common define these two symbols instead.

#include <kickos/arch/arch.h>

extern "C"
{

// NOT ctx.resting_npriv: that says the thread is a user thread, while syscall dispatch
// runs PRIVILEGED in thread mode on the thread's own stack (switch.S svc_trampoline), so
// a fault there is a kernel bug. Exception entry does not modify CONTROL.nPRIV, so
// reading it here gives the privilege at fault time. A non-zero stacked IPSR means the
// fault escalated from inside another handler: also a kernel bug.
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
    if (not kickos_fault_frame_trusted(frame, 32))
    {
        return false;
    }
    uint32_t const* const f = static_cast<uint32_t const*>(frame);
    return (f[7] & 0x3Fu) == 0u; // stacked xPSR IPSR field (6 bits on v6-M)
}

// Entered by the exception return with r0 = the SP the stub must run on. Naked, and the
// SP move is the first instruction: anything the compiler put before it would run on the
// stack this exists to leave. Thread mode does not change SPSEL, so this writes the PSP
// the switcher gave the thread, which is where svc_trampoline runs privileged too.
__attribute__((naked, noreturn)) void kickos_armv6m_fault_stack_reset(void)
{
    // Reached through a LITERAL, not `b`. On v6-M an unconditional `b` is the T2 encoding,
    // whose range is +/-2 KB, and the target sits in a different archive member: the link
    // failed with "relocation truncated to fit: R_ARM_THM_JUMP11". `bl` would widen it to
    // +/-4 MB and would work today, but it is still a range the layout could outgrow without
    // anything saying so. A PC-relative word plus `bx` cannot run out of range at all.
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

    // The stub runs at the top of this thread's stack, not at the depth the fault reached.
    // The frame stays where the hardware stacked it and the shim moves SP after the pop:
    // v6-M has no FP unit and so no extended frame, but relocating would still have to
    // match the pop's expectations, and there is nothing to gain by it. r0 carries the new
    // SP because it is the frame's own first word and this thread is dying.
    uint32_t const top = static_cast<uint32_t>(kickos_fault_stack_top());
    if (top != 0)
    {
        f[0] = top & ~7u; // AAPCS wants 8-byte alignment at a public interface
        f[6] = reinterpret_cast<uint32_t>(&kickos_armv6m_fault_stack_reset) & ~1u;
    }
    else
    {
        f[6] = reinterpret_cast<uint32_t>(&kickos_thread_fault_exit) & ~1u; // drop the Thumb bit
    }
    // v6-M has no IT/ICI state to clear. Forcing T is not redundant: a cleared T bit is
    // itself one of the ways a thread arrives here.
    f[7] = f[7] | (1u << 24);
    // Exception return does not restore CONTROL, so clearing nPRIV here is what makes the
    // stub privileged. SPSEL is the bit handler mode ignores; nPRIV is not.
    uint32_t control;
    __asm volatile("mrs %0, control" : "=r"(control));
    __asm volatile("msr control, %0" ::"r"(control & ~1u));
    __asm volatile("isb" ::: "memory");
}

}
