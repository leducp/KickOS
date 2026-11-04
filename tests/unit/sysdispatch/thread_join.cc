// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// KOS_SYS_THREAD_JOIN over the REAL kernel/syscall/syscall_thread.cc at the region posture, for a
// target that has already exited: a sibling of its task that did not create it is told it is
// gone, and a thread of another task learns no more than KOS_SYS_THREAD_CANCEL tells it.

#include <kickos/cap.h>
#include <kickos/instance.h>
#include <kickos/irqlock.h>
#include <kickos/kernel.h>
#include <kickos/sched.h>
#include <kickos/task.h>
#include <kickos/thread.h>

#include <kickos/sys/abi.h>
#include <kickos/sys/errno.h>

#include "kseam_test.h"
#include "stub_seam.h"

extern "C" uint64_t syscall_dispatch(uintptr_t nr, uintptr_t a0, uintptr_t a1, uintptr_t a2,
                                     uintptr_t a3);

namespace kickos
{
    namespace testfix
    {
        namespace
        {
            constexpr uint8_t PRIO = 10;

            class SysThreadJoin : public KSeam
            {
              protected:
                void SetUp() override
                {
                    KSeam::SetUp();
                    group_ = task(0);
                    other_ = task(1);
                    joiner_ = seat_pool(1, PRIO);
                    target_ = seat_pool(2, PRIO);
                    stranger_ = seat_pool(3, PRIO);
                    join_task(joiner_, group_);
                    join_task(target_, group_);
                    join_task(stranger_, other_);
                    {
                        IrqLock lock;
                        handle_ = static_cast<kos_thread_t>(
                            kernel().threads.handle_for(kernel().threads.index_of(target_)));
                    }
                }

                void exit_target()
                {
                    kernel().current(kickos_kernel_core()) = target_;
                    run_exit(0);
                    ASSERT_EQ(target_->state, ThreadState::EXITED) << "fixture: the target lives";
                }

                int32_t join_as(Thread* caller, uint32_t timeout_us)
                {
                    kernel().current(kickos_kernel_core()) = caller;
                    return static_cast<int32_t>(
                        syscall_dispatch(KOS_SYS_THREAD_JOIN, handle_, timeout_us, 0, 0));
                }

                int32_t cancel_as(Thread* caller)
                {
                    kernel().current(kickos_kernel_core()) = caller;
                    return static_cast<int32_t>(syscall_dispatch(
                        KOS_SYS_THREAD_CANCEL, handle_, KOS_CANCEL_KILL, 0, 0));
                }

                Task* group_ = nullptr;
                Task* other_ = nullptr;
                Thread* joiner_ = nullptr;
                Thread* target_ = nullptr;
                Thread* stranger_ = nullptr;
                kos_thread_t handle_ = KOS_THREAD_NONE;
            };
        }

        TEST_F(SysThreadJoin, a_sibling_joining_an_exited_thread_it_did_not_create_is_told_it_is_gone)
        {
            exit_target();

            EXPECT_EQ(join_as(joiner_, 0u), 0);
        }

        TEST_F(SysThreadJoin, another_task_s_thread_learns_of_an_exit_only_what_a_cancel_tells_it)
        {
            EXPECT_EQ(join_as(stranger_, 0u), -KOS_EPERM) << "a live thread of another task";
            EXPECT_EQ(cancel_as(stranger_), -KOS_EPERM);

            exit_target();

            EXPECT_EQ(cancel_as(stranger_), -KOS_EBADF) << "the cancel already tells an exit";
            EXPECT_EQ(join_as(stranger_, 0u), 0);
        }
    }
}
