// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The resume half of arch_fault_redirect_to_exit on ARMv6-M and ARMv7-M. The frame stays where
// the hardware stacked it and the stub moves SP after the pop: on v7-M the EXC_RETURN in the
// handler's LR decides whether a basic or an extended frame is popped (ARMv7-M ARM B1.5.7), so
// a relocated frame would be popped at the wrong size out of the wrong memory.

#ifndef KICKOS_ARCH_ARM_COMMON_FAULT_RESUME_H
#define KICKOS_ARCH_ARM_COMMON_FAULT_RESUME_H

#include <kickos/arch/arch.h>

#include <stdint.h>

// The exception return lands in `stub` with r0 = the top of this thread's stack, which the
// stub moves SP to; r0 carries it because it is the frame's own first word and this thread is
// dying. With no stack top to run on, the return lands in the exit directly.
static inline void kickos_arm_fault_resume_at(uint32_t* f, void (*stub)(void))
{
    uint32_t const top = static_cast<uint32_t>(kickos_fault_stack_top());
    if (top != 0)
    {
        f[0] = top & ~7u; // AAPCS wants 8-byte alignment at a public interface
        f[6] = reinterpret_cast<uint32_t>(stub) & ~1u;
    }
    else
    {
        f[6] = reinterpret_cast<uint32_t>(&kickos_thread_fault_exit) & ~1u; // drop the Thumb bit
    }
}

// Exception return does not restore CONTROL, so clearing nPRIV here is what makes the stub
// privileged. SPSEL is the bit handler mode ignores; nPRIV is not.
static inline void kickos_arm_fault_resume_privileged(void)
{
    uint32_t control;
    __asm volatile("mrs %0, control" : "=r"(control));
    __asm volatile("msr control, %0" ::"r"(control & ~1u));
    __asm volatile("isb" ::: "memory");
}

#endif
