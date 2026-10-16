// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The kernel's side of the libc locks: the two mutexes an address space carries, which a region
// board's image is, named by pseudo-handle and handed over by a member that dies holding one.

#include <kickos/cap.h>
#include <kickos/instance.h>
#include <kickos/irqlock.h>
#include <kickos/sched.h>
#include <kickos/sync.h>

#include "kseam_test.h"

namespace kickos
{
    namespace testfix
    {
        namespace
        {
            class LibcLockSpace : public KSeam
            {
            };

            constexpr uint8_t PRIO_OWNER = 5;

            Thread* running_owner()
            {
                Thread* const c = spawn(0, PRIO_OWNER);
                {
                    IrqLock lock;
                    sched::reschedule(nullptr, lock);
                }
                EXPECT_EQ(kernel().current(kickos_kernel_core()), c);
                attach_caps(c, KICKOS_CAP_CHILD_WIDTH);
                return c;
            }

            TEST_F(LibcLockSpace, two_members_of_one_space_name_one_pair_of_locks)
            {
                Thread* const t = spawn(1, PRIO_OWNER);
                Thread* const u = spawn(2, PRIO_OWNER);
                Mutex* const libc = cap_libc_lock(t, KOS_CAP_LIBC_LOCK);
                Mutex* const mlock = cap_libc_lock(t, KOS_CAP_MALLOC_LOCK);
                ASSERT_NE(libc, nullptr);
                ASSERT_NE(mlock, nullptr);
                EXPECT_NE(libc, mlock) << "malloc's lock is apart from the libc lock";
                EXPECT_EQ(cap_libc_lock(u, KOS_CAP_LIBC_LOCK), libc);
                EXPECT_EQ(cap_libc_lock(u, KOS_CAP_MALLOC_LOCK), mlock);
            }

            TEST_F(LibcLockSpace, no_other_word_names_a_libc_lock)
            {
                Thread* const t = spawn(1, PRIO_OWNER);
                EXPECT_EQ(cap_libc_lock(t, KOS_CAP_AUTHORITY), nullptr);
                EXPECT_EQ(cap_libc_lock(t, KOS_CAP_NONE), nullptr);
                EXPECT_EQ(cap_libc_lock(t, KOS_CAP_FIRST_DYNAMIC), nullptr);
            }

            TEST_F(LibcLockSpace, a_libc_lock_is_no_capability_to_close_or_narrow)
            {
                Thread* const t = spawn(1, PRIO_OWNER);
                attach_caps(t, KICKOS_CAP_CHILD_WIDTH);
                IrqLock lock;
                EXPECT_EQ(handle_close(t, KOS_CAP_LIBC_LOCK, lock), -KOS_EBADF);
                EXPECT_EQ(handle_close(t, KOS_CAP_MALLOC_LOCK, lock), -KOS_EBADF);
                EXPECT_EQ(cap_narrow(t, KOS_CAP_LIBC_LOCK, 0u, lock), -KOS_EBADF);
                EXPECT_EQ(cap_narrow(t, KOS_CAP_MALLOC_LOCK, 0u, lock), -KOS_EBADF);
            }

            TEST_F(LibcLockSpace, holding_malloc_leaves_the_libc_lock_free)
            {
                Thread* const c = running_owner();
                int err = 0;
                Mutex* mlock = nullptr;
                Mutex* libc = nullptr;
                {
                    IrqLock lock;
                    mlock = cap_resolve_mutex(c, KOS_CAP_MALLOC_LOCK, &err);
                    libc = cap_resolve_mutex(c, KOS_CAP_LIBC_LOCK, &err);
                }
                ASSERT_NE(mlock, nullptr);
                ASSERT_EQ(mutex_lock(mlock, 0), 0);
                EXPECT_EQ(mlock->owner, c);
                EXPECT_EQ(libc->owner, nullptr);
                ASSERT_EQ(mutex_lock(libc, 0), 0);
                EXPECT_EQ(mutex_unlock(libc), 0);
                EXPECT_EQ(mutex_unlock(mlock), 0);
            }

            TEST_F(LibcLockSpace, a_member_dying_inside_malloc_hands_the_lock_over_held)
            {
                Thread* const c = running_owner();
                Thread* const waiter = spawn(1, PRIO_OWNER + 1);
                spawn(2, PRIO_OWNER - 1); // keeps k.live above zero
                Mutex* const mlock = cap_libc_lock(c, KOS_CAP_MALLOC_LOCK);
                ASSERT_EQ(mutex_lock(mlock, 0), 0);
                park_mutex_waiter(waiter, mlock);

                run_exit(0);

                EXPECT_EQ(mlock->owner, waiter) << "the waiter holds the lock its owner died in";
                EXPECT_EQ(waiter->wait_result, -KOS_EOWNERDEAD) << "and was told the owner died";
                EXPECT_EQ(c->held_list, nullptr) << "the dying thread holds nothing";
                EXPECT_EQ(cap_libc_lock(waiter, KOS_CAP_LIBC_LOCK)->owner, nullptr);
            }

            TEST_F(LibcLockSpace, a_member_dying_with_no_libc_lock_hands_nothing_over)
            {
                Thread* const c = running_owner();
                Thread* const other = spawn(1, PRIO_OWNER - 1);
                Mutex* const libc = cap_libc_lock(other, KOS_CAP_LIBC_LOCK);
                libc->owner = other;

                run_exit(0);

                EXPECT_EQ(libc->owner, other) << "a lock another member holds stays held";
                EXPECT_EQ(c->held_list, nullptr);
                libc->owner = nullptr;
            }
        }
    }
}
