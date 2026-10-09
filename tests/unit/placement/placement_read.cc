// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The placement reads, kos_thread_affinity and kos_task_cores, over the real
// kernel/syscall/syscall_thread.cc at two kernel cores: each answers what its setter left, and
// reaches exactly what that setter reaches.

#include <kickos/irqlock.h>
#include <kickos/sched.h>
#include <kickos/sync.h>
#include <kickos/task.h>
#include <kickos/thread.h>

#include "kseam_test.h"
#include "syscall_internal.h"

using namespace kickos;
using namespace kickos::testfix;

namespace
{
    constexpr uint8_t PRIO = 5;
    constexpr uint32_t CORE_ONE = 1u << 1;

    class PlacementRead : public KSeam
    {
      protected:
        void SetUp() override
        {
            KSeam::SetUp();
            mine_ = task(0);
            main_ = seat_pool(1, PRIO);
            join_task(main_, mine_);
            seat_running_on(main_, 0);
            mate_ = seat_pool(2, PRIO);
            join_task(mate_, mine_);
        }

        static kos_thread_t handle(Thread const* t)
        {
            Kernel& k = kernel();
            return k.threads.handle_for(k.threads.index_of(t));
        }

        Task* mine_ = nullptr;
        Thread* main_ = nullptr;
        Thread* mate_ = nullptr;
    };
}

TEST_F(PlacementRead, affinity_reads_what_set_affinity_left)
{
    ASSERT_EQ(thread_set_affinity(handle(mate_), CORE_ONE), 0);
    EXPECT_EQ(thread_affinity(handle(mate_)), static_cast<int>(CORE_ONE))
        << "the read answered another mask than the setter left";
    EXPECT_EQ(thread_affinity(handle(main_)), static_cast<int>(main_->affinity));
}

TEST_F(PlacementRead, a_zero_request_reads_back_resolved)
{
    ASSERT_EQ(thread_set_affinity(handle(mate_), CORE_ONE), 0);
    ASSERT_EQ(thread_set_affinity(handle(mate_), 0), 0);
    EXPECT_EQ(thread_affinity(handle(mate_)), static_cast<int>(task_default_cores(mine_)));
}

TEST_F(PlacementRead, another_task_s_thread_is_eperm_unless_privileged)
{
    Thread* const stranger = seat_pool(3, PRIO);
    join_task(stranger, task(1));
    EXPECT_EQ(thread_affinity(handle(stranger)), -KOS_EPERM)
        << "an unprivileged caller read another task's placement";
    main_->privileged = true;
    EXPECT_EQ(thread_affinity(handle(stranger)), static_cast<int>(stranger->affinity));
}

TEST_F(PlacementRead, an_exited_thread_is_ebadf)
{
    kos_thread_t const h = handle(mate_);
    *reinterpret_cast<ThreadState*>(&mate_->state) = ThreadState::EXITED;
    EXPECT_EQ(thread_affinity(h), -KOS_EBADF);
    EXPECT_EQ(thread_affinity(KOS_THREAD_NONE), -KOS_EBADF);
}

TEST_F(PlacementRead, task_none_names_the_caller_s_task)
{
    {
        IrqLock lock;
        mine_->core_set = CORE_ONE;
    }
    EXPECT_EQ(task_cores(KOS_TASK_NONE), static_cast<int>(CORE_ONE))
        << "KOS_TASK_NONE read another task's grant";
}

TEST_F(PlacementRead, a_task_the_caller_did_not_create_is_eperm)
{
    Task* const theirs = task(1);
    EXPECT_EQ(task_cores(task_handle(theirs)), -KOS_EPERM)
        << "the caller read a grant it did not create";
    int err = 0;
    Task* const made =
        task_create(kernel().threads.kill_tag_of(main_), nullptr, 0, 0u, nullptr, &err);
    ASSERT_NE(made, nullptr) << err;
    {
        IrqLock lock;
        made->core_set = CORE_ONE;
    }
    EXPECT_EQ(task_cores(task_handle(made)), static_cast<int>(CORE_ONE));
    EXPECT_EQ(task_cores(0xFFFFFFFFu), -KOS_EBADF);
}
