// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The resume half of arch_fault_redirect_to_exit on ARMv6-M and ARMv7-M. The return into
// kickos_thread_fault_exit pops a basic frame the handler builds at the top of the dying thread's
// stack, its kernel block where one is seated, and never the frame the fault left in user memory:
// a task-mate can rewrite that frame while the return is pending, and this return is privileged.

#ifndef KICKOS_ARCH_ARM_COMMON_FAULT_RESUME_H
#define KICKOS_ARCH_ARM_COMMON_FAULT_RESUME_H

#include <kickos/arch/arch.h>

#include "exc_return.h"

#include <stdint.h>

// The EXC_RETURN a built frame is popped with: thread mode, PSP, basic frame.
#define KICKOS_ARM_FAULT_EXIT_EXC_RETURN 0xFFFFFFFDu

// The SP kickos_thread_fault_exit starts on, 8-aligned for AAPCS. 0 declines the kill.
static inline uint32_t kickos_arm_fault_exit_top(void)
{
    return static_cast<uint32_t>(kickos_fault_stack_top()) & ~7u;
}

// No register of the dying thread's survives into the exit.
static inline void kickos_arm_fault_frame_at(uint32_t top)
{
    uint32_t* const f = reinterpret_cast<uint32_t*>(top) - 8;
    for (int i = 0; i < 6; i++)
    {
        f[i] = 0;
    }
    f[6] = reinterpret_cast<uint32_t>(&kickos_thread_fault_exit) & ~1u;
    f[7] = 1u << 24; // T, and no IPSR, IT or realign bit
    __asm volatile("msr psp, %0" ::"r"(f) : "memory");
}

// Exception return does not restore CONTROL, so clearing nPRIV here is what makes the exit
// privileged. SPSEL is the bit handler mode ignores; nPRIV is not.
static inline void kickos_arm_fault_resume_privileged(void)
{
    uint32_t control;
    __asm volatile("mrs %0, control" : "=r"(control));
    __asm volatile("msr control, %0" ::"r"(control & ~1u));
    __asm volatile("isb" ::: "memory");
}

#endif
