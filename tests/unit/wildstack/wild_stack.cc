// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// What kickos_thread_contain_wild_stack REFUSES. Containing the wrong thread is worse than
// ending the system: slaying idle leaves the scheduler nothing to run, and containing a
// privileged thread hides a kernel bug behind a thread death.

#include <kickos/arch/arch.h>

#include <stdint.h>
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
            constexpr int SLOT_VICTIM = 0;
            constexpr uint8_t PRIO_LOW = 4;

            struct WildStack : public KSeam
            {
            };

            TEST_F(WildStack, an_ordinary_pool_thread_is_contained)
            {
                Thread* const t = seat_pool(SLOT_VICTIM, PRIO_LOW);
                kernel().current[arch_cpu_id()] = t;
                EXPECT_NE(kickos_thread_contain_wild_stack(&t->ctx, nullptr), nullptr)
                    << "a pool thread with a wild sp is exactly what containment is for";
                EXPECT_EQ(t->cancel_kind, CANCEL_SLAY) << "and the slay must have claimed it";
            }

            // IDLE IS REFUSED BY NOT BEING A POOL SLOT: the seam resolves an offender by
            // scanning the pool, and idle is kernel().idle_tcb, outside it. The seam carried a
            // named `bad == k.idle` clause until mutating it away left this arm passing, which
            // is how it was found unreachable; the clause is gone and this is the reason that
            // remains.
            TEST_F(WildStack, idle_is_refused_because_no_pool_slot_owns_it)
            {
                Thread* const victim = kernel().idle;
                ASSERT_NE(victim, nullptr) << "the fixture seats an idle thread";
                EXPECT_LT(kernel().threads.index_of(victim), 0)
                    << "idle must stay outside the pool: the refusal below rests on it";
                EXPECT_EQ(kickos_thread_contain_wild_stack(&victim->ctx, nullptr), nullptr)
                    << "slaying idle leaves the scheduler nothing to run";
                EXPECT_NE(victim->cancel_kind, CANCEL_SLAY)
                    << "and the refusal must come BEFORE the slay, not after it";
            }

            TEST_F(WildStack, a_privileged_thread_is_refused)
            {
                Thread* const t = seat_pool(SLOT_VICTIM, PRIO_LOW);
                t->privileged = true;
                kernel().current[arch_cpu_id()] = t;
                EXPECT_EQ(kickos_thread_contain_wild_stack(&t->ctx, nullptr), nullptr)
                    << "a privileged thread's wild pointer is a kernel bug, not a thread's";
            }

            TEST_F(WildStack, a_context_no_slot_owns_is_refused)
            {
                struct arch_context stranger = {};
                EXPECT_EQ(kickos_thread_contain_wild_stack(&stranger, nullptr), nullptr)
                    << "the boot context and a freed one are not threads to contain";
            }
        }
    }
}
