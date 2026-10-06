// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// dev_window_free and dev_window_held_outside over the real thread pool: a live thread holding
// an overlapping device region holds the window, an exited one and an adjacent region do not,
// and only a holder outside the task asked about counts against it.

#include <kickos/instance.h>
#include <kickos/kernel.h>

#include <gtest/gtest.h>

#include <stdint.h>

extern "C" uint32_t arch_mpu_encode(arch_mpu_region const*, size_t n, arch_mpu_encoded*)
{
    return static_cast<uint32_t>((1ull << n) - 1u);
}

namespace kickos
{
    namespace detail
    {
        constinit InstanceLocal<Kernel> g_instance;
    }
}

namespace
{
    using kickos::Task;
    using kickos::Thread;
    using kickos::ThreadState;

    constexpr uintptr_t WINDOW = 0x40000000u;
    constexpr size_t SIZE = 0x100u;

    Task* task(uintptr_t n)
    {
        return reinterpret_cast<Task*>(n * 0x100u);
    }

    Thread& holder(int slot, Task* t, uintptr_t base, ThreadState state)
    {
        kickos::Kernel& k = kickos::kernel();
        Thread& th = k.threads.slots[slot];
        th.state = state;
        th.task = t;
        th.mpu.clear();
        EXPECT_TRUE(th.mpu.add(base, SIZE, ARCH_MPU_R | ARCH_MPU_W | ARCH_MPU_DEV));
        if (k.threads.next <= slot)
        {
            k.threads.next = slot + 1;
        }
        return th;
    }

    class WindowHolder : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            kickos::Kernel& k = kickos::kernel();
            for (Thread& th : k.threads.slots)
            {
                th.state = ThreadState::INACTIVE;
                th.mpu.clear();
            }
            k.threads.next = 0;
        }
    };
}

TEST_F(WindowHolder, a_holder_inside_the_task_asked_about_is_not_outside_it)
{
    holder(0, task(1), WINDOW, ThreadState::BLOCKED);
    EXPECT_FALSE(kickos::dev_window_free(WINDOW, SIZE));
    EXPECT_FALSE(kickos::dev_window_held_outside(WINDOW, SIZE, task(1)));
    EXPECT_TRUE(kickos::dev_window_held_outside(WINDOW, SIZE, task(2)));
}

TEST_F(WindowHolder, one_holder_outside_among_members_is_outside)
{
    holder(0, task(1), WINDOW, ThreadState::READY);
    holder(1, task(2), WINDOW + SIZE / 2u, ThreadState::RUNNING);
    EXPECT_TRUE(kickos::dev_window_held_outside(WINDOW, SIZE, task(1)));
    EXPECT_TRUE(kickos::dev_window_held_outside(WINDOW, SIZE, task(2)));
}

TEST_F(WindowHolder, an_exited_thread_and_an_adjacent_window_hold_nothing)
{
    holder(0, task(2), WINDOW, ThreadState::EXITED);
    holder(1, task(2), WINDOW + SIZE, ThreadState::READY);
    EXPECT_TRUE(kickos::dev_window_free(WINDOW, SIZE));
    EXPECT_FALSE(kickos::dev_window_held_outside(WINDOW, SIZE, task(1)));
}
