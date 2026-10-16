// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// sched::exit_current hands every device window the dying thread holds to arch_periph_disable,
// once each, and only device windows. The seam's recorder also reads whether the window was
// still the dying thread's at the call: dropped first, a supervisor could respawn a second holder
// into it and have the late disable close that holder's block.

#include <kickos/instance.h>
#include <kickos/irqlock.h>
#include <kickos/kernel.h>
#include <kickos/sched.h>
#include <kickos/sync.h>
#include <kickos/task.h>

#include "kseam_test.h"

#include <stdint.h>

namespace kickos
{
    namespace testfix
    {
        namespace
        {
            class WindowClose : public KSeam
            {
            };

            constexpr int SLOT_DYING = 0;
            constexpr int SLOT_PEER = 1;
            constexpr uint8_t PRIO_LOW = 4;
            constexpr uint8_t PRIO_MID = 5;

            constexpr uintptr_t WINDOW_A = 0x4002C000u;
            constexpr uintptr_t WINDOW_B = 0x40040000u;
            constexpr uintptr_t WINDOW_PEER = 0x4006A000u;
            constexpr uintptr_t MEMORY = 0x20001000u;
            constexpr size_t SIZE = 0x40u;
            constexpr uint32_t DEVICE = ARCH_MPU_R | ARCH_MPU_W | ARCH_MPU_DEV;

            void hold(Thread* t, uintptr_t base, uint32_t attr)
            {
                EXPECT_TRUE(t->mpu.add(base, SIZE, attr));
            }

            bool holds_device(Thread const* t)
            {
                for (arch_mpu_region const& r : t->mpu)
                {
                    if ((r.attr & ARCH_MPU_DEV) != 0)
                    {
                        return true;
                    }
                }
                return false;
            }
        }

        TEST_F(WindowClose, a_holders_death_closes_each_block_it_held)
        {
            Thread* const c = seat_pool(SLOT_DYING, PRIO_MID);
            Thread* const peer = seat_pool(SLOT_PEER, PRIO_LOW);
            join_task(c, task(0));
            join_task(peer, task(1));
            hold(c, WINDOW_A, DEVICE);
            hold(c, MEMORY, ARCH_MPU_R | ARCH_MPU_W);
            hold(c, WINDOW_B, DEVICE);
            kernel().current(kickos_kernel_core()) = c;

            run_exit(0);

            ASSERT_EQ(g_periph_disables, 2u) << "one disable per device window, none for memory";
            EXPECT_EQ(g_periph_disabled[0], WINDOW_A);
            EXPECT_EQ(g_periph_disabled[1], WINDOW_B);
            EXPECT_EQ(g_periph_disables_held, 2u)
                << "each block closes while its window is still the dying thread's";
            EXPECT_FALSE(holds_device(c)) << "and the windows are released after";
        }

        // A fault ends the whole group, and still closes only what the faulting thread held: a
        // peer's window closes at the peer's own exit.
        TEST_F(WindowClose, a_fault_closes_the_faulting_holders_blocks_alone)
        {
            Task* const group = task(0);
            Thread* const c = seat_pool(SLOT_DYING, PRIO_MID);
            Thread* const peer = seat_pool(SLOT_PEER, PRIO_LOW);
            join_task(c, group);
            join_task(peer, group);
            hold(c, WINDOW_A, DEVICE);
            hold(peer, WINDOW_PEER, DEVICE);
            kernel().current(kickos_kernel_core()) = c;

            run_exit_faulted(0);

            ASSERT_EQ(g_periph_disables, 1u);
            EXPECT_EQ(g_periph_disabled[0], WINDOW_A);
            EXPECT_TRUE(holds_device(peer)) << "the slain peer has not reached its exit yet";
        }

        TEST_F(WindowClose, a_death_holding_no_window_closes_nothing)
        {
            Thread* const c = seat_pool(SLOT_DYING, PRIO_MID);
            Thread* const peer = seat_pool(SLOT_PEER, PRIO_LOW);
            join_task(c, task(0));
            join_task(peer, task(1));
            hold(c, MEMORY, ARCH_MPU_R | ARCH_MPU_W);
            kernel().current(kickos_kernel_core()) = c;

            run_exit(0);

            EXPECT_EQ(g_periph_disables, 0u);
        }
    }
}
