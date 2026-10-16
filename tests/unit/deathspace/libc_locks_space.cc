// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The libc locks at the translating posture: each address space's Domain carries its own pair, a
// member's death hands over what it holds of its Domain's, and a space with no member is left
// with idle locks.

#include <kickos/cap.h>
#include <kickos/domain.h>
#include <kickos/instance.h>
#include <kickos/irqlock.h>
#include <kickos/sync.h>
#include <kickos/task.h>
#include <kickos/thread.h>

#include "aspace_seam.h"
#include "kseam_test.h"

namespace kickos
{
    namespace testfix
    {
        namespace
        {
            class LibcLocksSpace : public KSeam
            {
              protected:
                void SetUp() override
                {
                    KSeam::SetUp();
                    aspace_seam_reset();
                }
            };

            static_assert(KICKOS_HAVE_ASPACE, "the translating posture keys the locks on a Domain");

            constexpr uint8_t PRIO_LOW = 4;
            constexpr uint8_t PRIO_MID = 5;
            constexpr uint8_t PRIO_HIGH = 6;

            Thread* member_of(int slot, uint8_t prio, Task* group)
            {
                Thread* const t = seat_pool(slot, prio);
                join_task(t, group);
                return t;
            }

            void run_as(Thread* t)
            {
                kernel().current(testfix::core_at(0)) = t;
                t->state.to<ThreadState::RUNNING>();
                install_for(t);
            }

            bool idle(Mutex const* m)
            {
                return m->owner == nullptr and m->waiters.empty() and m->next_held == nullptr;
            }
        }

        TEST_F(LibcLocksSpace, two_spaces_name_two_distinct_pairs)
        {
            Task* const a = task(0);
            Task* const b = task(1);
            ASSERT_NE(task_domain(a), task_domain(b));
            Thread* const ta = member_of(1, PRIO_MID, a);
            Thread* const tb = member_of(2, PRIO_MID, b);
            Mutex* const a_libc = cap_libc_lock(ta, KOS_CAP_LIBC_LOCK);
            Mutex* const a_malloc = cap_libc_lock(ta, KOS_CAP_MALLOC_LOCK);
            ASSERT_NE(a_libc, nullptr);
            ASSERT_NE(a_malloc, nullptr);
            EXPECT_NE(a_libc, a_malloc);
            EXPECT_EQ(a_libc, &task_domain(a)->libc_locks[LIBC_LOCK_STDIO]);
            EXPECT_EQ(a_malloc, &task_domain(a)->libc_locks[LIBC_LOCK_MALLOC]);
            EXPECT_NE(cap_libc_lock(tb, KOS_CAP_LIBC_LOCK), a_libc);
            EXPECT_NE(cap_libc_lock(tb, KOS_CAP_MALLOC_LOCK), a_malloc);
        }

        TEST_F(LibcLocksSpace, a_member_dying_inside_malloc_hands_its_spaces_lock_over)
        {
            (void)seat_pool(0, PRIO_LOW); // keeps k.live above zero
            Task* const group = task(0);
            Thread* const dying = member_of(1, PRIO_MID, group);
            Thread* const waiter = member_of(2, PRIO_HIGH, group);
            attach_caps(dying, KICKOS_CAP_CHILD_WIDTH);
            Mutex* const mlock = cap_libc_lock(dying, KOS_CAP_MALLOC_LOCK);
            run_as(dying);
            ASSERT_EQ(mutex_lock(mlock, 0), 0);
            park_mutex_waiter(waiter, mlock);

            run_exit(0);

            EXPECT_EQ(mlock->owner, waiter);
            EXPECT_EQ(waiter->wait_result, -KOS_EOWNERDEAD);
            EXPECT_EQ(dying->held_list, nullptr);
        }

        TEST_F(LibcLocksSpace, a_space_whose_last_member_died_holding_its_locks_marks_them)
        {
            (void)seat_pool(0, PRIO_LOW);
            Task* const group = task(0);
            Domain* const d = task_domain(group);
            Thread* const dying = member_of(1, PRIO_MID, group);
            attach_caps(dying, KICKOS_CAP_CHILD_WIDTH);
            run_as(dying);
            ASSERT_EQ(mutex_lock(cap_libc_lock(dying, KOS_CAP_LIBC_LOCK), 0), 0);
            ASSERT_EQ(mutex_lock(cap_libc_lock(dying, KOS_CAP_MALLOC_LOCK), 0), 0);

            run_exit(0);

            EXPECT_EQ(task_member_count(group), 0);
            EXPECT_TRUE(idle(&d->libc_locks[LIBC_LOCK_STDIO]));
            EXPECT_TRUE(idle(&d->libc_locks[LIBC_LOCK_MALLOC]));
            EXPECT_TRUE(d->libc_locks[LIBC_LOCK_STDIO].owner_died);
            EXPECT_TRUE(d->libc_locks[LIBC_LOCK_MALLOC].owner_died)
                << "the death went unrecorded for want of a waiter";
        }

        TEST_F(LibcLocksSpace, a_thread_with_no_space_names_no_libc_lock)
        {
            Thread* const t = seat_pool(1, PRIO_MID);
            ASSERT_EQ(t->task, nullptr);
            EXPECT_EQ(cap_libc_lock(t, KOS_CAP_LIBC_LOCK), nullptr);
            EXPECT_EQ(cap_libc_lock(t, KOS_CAP_MALLOC_LOCK), nullptr);
            int err = 0;
            {
                IrqLock lock;
                EXPECT_EQ(cap_resolve_mutex(t, KOS_CAP_LIBC_LOCK, &err), nullptr);
                cap_libc_locks_release(t, lock);
            }
            EXPECT_EQ(err, KOS_EBADF);
        }
    }
}
