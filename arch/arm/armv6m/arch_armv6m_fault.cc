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

void arch_fault_redirect_to_exit(void* frame)
{
    uint32_t* const f = static_cast<uint32_t*>(frame);
    // v6-M has no fault-status and no fault-address register, so the dump is the PC
    // alone; a null name is what tells the printer there is no status word to name.
    kickos_fault_record(nullptr, 0, f[6], 0, 0);

    f[6] = reinterpret_cast<uint32_t>(&kickos_thread_fault_exit) & ~1u; // drop the Thumb bit
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
