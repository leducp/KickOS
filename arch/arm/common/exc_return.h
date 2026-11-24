// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// What an M-profile EXC_RETURN says about the context an exception interrupted.

#ifndef KICKOS_ARCH_ARM_COMMON_EXC_RETURN_H
#define KICKOS_ARCH_ARM_COMMON_EXC_RETURN_H

#include <stdint.h>

// EXC_RETURN's mode and stack bits, and on v8-M the stack's (S) and the handler's (ES)
// security state.
#define KICKOS_ARM_EXC_RETURN_THREAD 0x8u
#define KICKOS_ARM_EXC_RETURN_PSP 0x4u
#define KICKOS_ARM_EXC_RETURN_S 0x40u
#define KICKOS_ARM_EXC_RETURN_ES 0x1u

// The only context a fault may be a thread's own: thread mode, on the process stack.
static inline bool kickos_arm_exc_return_thread_psp(uint32_t exc_return)
{
    uint32_t const both = KICKOS_ARM_EXC_RETURN_THREAD | KICKOS_ARM_EXC_RETURN_PSP;
    return (exc_return & both) == both;
}

// A Secure handler took the exception from Non-secure state. Never on v7-M, where both bits
// read as one, nor on a v8-M core without the Security Extension, where ES reads as zero.
static inline bool kickos_arm_exc_return_from_nonsecure(uint32_t exc_return)
{
    uint32_t const both = KICKOS_ARM_EXC_RETURN_S | KICKOS_ARM_EXC_RETURN_ES;
    return (exc_return & both) == KICKOS_ARM_EXC_RETURN_ES;
}

#endif
