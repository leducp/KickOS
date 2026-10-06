// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The v6-M reporter reads a frame on MSP, inside the running thread's own stack, or inside its
// kernel block, and no other: a wild PSP's frame read from HardFault locks the core up.

#include <kickos/arch/arch.h>
#include <kickos/instance.h>
#include <kickos/kernel.h>
#include <kickos/sched.h>
#include <kickos/thread.h>

#include KICKOS_V6M_FAULT_FRAME_H

#include "kseam_test.h"

namespace
{
    // The top of the running thread's kernel block, ctx.kernel_sp on the chip.
    uintptr_t g_kernel_sp = 0;
}

// kernel/init/fault.cc's bound over g_kernel_sp.
extern "C" bool kickos_fault_frame_on_kernel_stack(void const* frame, size_t bytes)
{
    kickos::Thread* const c = kickos::sched::current();
    if (c == nullptr or c == kickos::sched::idle() or g_kernel_sp == 0)
    {
        return false;
    }
    uintptr_t const lo = g_kernel_sp - KICKOS_KERNEL_STACK_SIZE;
    uintptr_t const f = reinterpret_cast<uintptr_t>(frame);
    return f >= lo and f < g_kernel_sp and bytes <= (g_kernel_sp - f);
}

namespace kickos
{
    namespace testfix
    {
        namespace
        {
            class V6mFaultFrame : public KSeam
            {
            };

            constexpr uint8_t PRIO = 5;
            constexpr uintptr_t STACK_BASE = 0x20010000u;
            constexpr size_t STACK_SIZE = 0x800u;
            constexpr uint32_t EXC_MSP = 0xFFFFFFF9u;
            constexpr uint32_t EXC_PSP = 0xFFFFFFFDu;

            void const* at(uintptr_t a)
            {
                return reinterpret_cast<void const*>(a);
            }

            constexpr uintptr_t KERNEL_SP = 0x20020000u;
            constexpr size_t FRAME = ARMV6M_BASIC_FRAME_BYTES;

            void seat_current()
            {
                Thread* const t = spawn(0, PRIO);
                t->stack_base = reinterpret_cast<void*>(STACK_BASE);
                t->stack_size = STACK_SIZE;
                kernel().current[kickos_kernel_core()] = t;
                g_kernel_sp = 0;
            }

            // A kernel bug in syscall dispatch faults with its frame on the kernel block.
            TEST_F(V6mFaultFrame, a_frame_inside_the_kernel_block_is_read)
            {
                seat_current();
                g_kernel_sp = KERNEL_SP;
                EXPECT_TRUE(armv6m_fault_frame_readable(at(KERNEL_SP - FRAME), EXC_PSP));
                EXPECT_TRUE(
                    armv6m_fault_frame_readable(at(KERNEL_SP - KICKOS_KERNEL_STACK_SIZE), EXC_PSP));
                EXPECT_FALSE(armv6m_fault_frame_readable(at(KERNEL_SP - FRAME / 2u), EXC_PSP));
                EXPECT_FALSE(armv6m_fault_frame_readable(
                    at(KERNEL_SP - KICKOS_KERNEL_STACK_SIZE - FRAME), EXC_PSP));
            }

            TEST_F(V6mFaultFrame, a_frame_inside_the_thread_stack_is_read)
            {
                seat_current();
                EXPECT_TRUE(armv6m_fault_frame_readable(at(STACK_BASE), EXC_PSP));
                EXPECT_TRUE(
                    armv6m_fault_frame_readable(at(STACK_BASE + STACK_SIZE - FRAME), EXC_PSP));
            }

            TEST_F(V6mFaultFrame, a_wild_psp_frame_is_not_read)
            {
                seat_current();
                EXPECT_FALSE(armv6m_fault_frame_readable(at(0xF0000000u - FRAME), EXC_PSP));
                EXPECT_FALSE(armv6m_fault_frame_readable(at(STACK_BASE - FRAME), EXC_PSP));
                EXPECT_FALSE(
                    armv6m_fault_frame_readable(at(STACK_BASE + STACK_SIZE - 16u), EXC_PSP));
            }

            TEST_F(V6mFaultFrame, a_psp_frame_with_no_thread_stack_is_not_read)
            {
                g_kernel_sp = KERNEL_SP;
                EXPECT_FALSE(armv6m_fault_frame_readable(at(STACK_BASE), EXC_PSP));
                EXPECT_FALSE(armv6m_fault_frame_readable(at(KERNEL_SP - FRAME), EXC_PSP));
            }

            // The kernel's own stack: no thread bound applies.
            TEST_F(V6mFaultFrame, an_msp_frame_is_read)
            {
                seat_current();
                EXPECT_TRUE(armv6m_fault_frame_readable(at(0xF0000000u - FRAME), EXC_MSP));
            }
        }
    }
}
