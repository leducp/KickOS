// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// kickos_fault_stack_top: where a backend's redirect seats the fault stub, so the stub never
// runs at the depth the fault reached.

#include <kickos/arch/arch.h>
#include <kickos/instance.h>
#include <kickos/kernel.h>
#include <kickos/sched.h>
#include <kickos/thread.h>

#include "kseam_test.h"

namespace kickos
{
    namespace testfix
    {
        namespace
        {
            class FaultStackTop : public KSeam
            {
            };

            constexpr uint8_t PRIO = 5;
            constexpr uintptr_t STACK_BASE = 0x20010000u;
            constexpr size_t STACK_SIZE = 0x800u;

            Thread* seat_current_with_stack(uintptr_t base, size_t size)
            {
                Thread* const t = spawn(0, PRIO);
                t->stack_base = reinterpret_cast<void*>(base);
                t->stack_size = size;
                kernel().current(kickos_kernel_core()) = t;
                return t;
            }

            // kickos_fault_stack_top has TWO branches and this file compiles ONE. It answers
            // with ctx.kernel_sp wherever a per-thread kernel block is seated and with the
            // user stack top otherwise, and ARCH_SIM selects neither ARCH_HAS_KERNEL_STACKS
            // nor ARCH_KERNEL_STACKS_MANDATORY, so KICKOS_KERNEL_STACKS resolves 0 here and
            // the arm below covers the only branch there is.
            //
            // THE OTHER BRANCH CANNOT BE REACHED ON THE HOST AT ALL: the sim's struct
            // arch_context is an opaque byte array (arch/sim/include/kickos/arch/context.h)
            // with no kernel_sp member, so the block arm does not compile against it. The
            // death stubs' seat assert, run by the emulator fault gates, holds that branch.
            static_assert(KICKOS_KERNEL_STACKS == 0,
                          "the host now compiles kickos_fault_stack_top's kernel-block arm, "
                          "which nothing in this file covers: give the sim's arch_context a "
                          "kernel_sp and add the block-top arm beside the one below");

            TEST_F(FaultStackTop, the_stub_lands_at_the_top_of_the_dying_threads_own_stack)
            {
                seat_current_with_stack(STACK_BASE, STACK_SIZE);
                EXPECT_EQ(kickos_fault_stack_top(), STACK_BASE + STACK_SIZE);
            }

            // 0 is the backend's "do not relocate" answer, on which rxv3's kill predicate
            // escalates instead, and it has to be reachable: a thread with no stack of its own
            // is exactly the case where a reset would send the stub to an address the thread
            // does not own.
            TEST_F(FaultStackTop, a_thread_with_no_stack_declines_the_reset)
            {
                Thread* const t = spawn(0, PRIO);
                t->stack_base = nullptr;
                t->stack_size = 0;
                kernel().current(kickos_kernel_core()) = t;
                EXPECT_EQ(kickos_fault_stack_top(), 0u);

                t->stack_base = reinterpret_cast<void*>(STACK_BASE);
                t->stack_size = 0;
                EXPECT_EQ(kickos_fault_stack_top(), 0u);
            }

            // A fault before the first thread runs is a real boot case.
            TEST_F(FaultStackTop, no_current_thread_relocates_nothing)
            {
                kernel().current(kickos_kernel_core()) = nullptr;
                EXPECT_EQ(kickos_fault_stack_top(), 0u);
            }
        }
    }
}
