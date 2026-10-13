// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The kernel's side of the libc lock on the real capability layer: the mutex one address space
// shares, which a region board's image is.

#include <kickos/cap.h>
#include <kickos/instance.h>
#include <kickos/irqlock.h>

#include "kseam_test.h"

namespace kickos
{
    namespace testfix
    {
        namespace
        {
            class LibcLockSeat : public KSeam
            {
            };

            Thread* member(int slot)
            {
                Thread* const t = seat_pool(slot, 5);
                attach_caps(t, KICKOS_CAP_CHILD_WIDTH);
                return t;
            }

            int refs(int lock)
            {
                Kernel& k = kernel();
                int const i = k.mutexes.live_index(lock);
                if (i < 0)
                {
                    return 0;
                }
                return k.mutex_refs[i];
            }

            TEST_F(LibcLockSeat, a_space_no_thread_holds_one_of_gets_a_fresh_mutex)
            {
                Thread* const t = member(1);
                IrqLock lock;
                int const libc = cap_libc_lock_take(t->task, lock);
                ASSERT_NE(libc, KCAP_LIBC_LOCK_NONE);
                EXPECT_EQ(refs(libc), 1);
                cap_seat_libc_lock(t, libc);
                CapEntry const* const e = cap_slot(t->caps, KOS_CAP_LIBC_LOCK);
                EXPECT_EQ(e->type, static_cast<uint8_t>(CapType::CAP_MUTEX));
                EXPECT_EQ(e->obj, libc);
                EXPECT_EQ(e->rights, 0u);
            }

            TEST_F(LibcLockSeat, a_new_member_shares_the_lock_a_live_thread_holds)
            {
                Thread* const t = member(1);
                Thread* const u = member(2);
                IrqLock lock;
                int const first = cap_libc_lock_take(t->task, lock);
                cap_seat_libc_lock(t, first);
                int const second = cap_libc_lock_take(u->task, lock);
                EXPECT_EQ(second, first);
                EXPECT_EQ(refs(first), 2);
                cap_seat_libc_lock(u, second);
            }

            TEST_F(LibcLockSeat, an_empty_mutex_pool_refuses_a_fresh_lock)
            {
                Thread* const t = member(1);
                Kernel& k = kernel();
                while (k.mutexes.alloc() >= 0)
                {
                }
                IrqLock lock;
                EXPECT_EQ(cap_libc_lock_take(t->task, lock), KCAP_LIBC_LOCK_NONE);
            }

            TEST_F(LibcLockSeat, its_holder_can_neither_close_nor_narrow_a_reserved_seat)
            {
                Thread* const t = member(1);
                IrqLock lock;
                int const libc = cap_libc_lock_take(t->task, lock);
                cap_seat_libc_lock(t, libc);
                CapEntry& out = *cap_slot(t->caps, KOS_CAP_STDOUT);
                out = *cap_slot(t->caps, KOS_CAP_LIBC_LOCK);
                EXPECT_EQ(handle_close(t, KOS_CAP_LIBC_LOCK, lock), -KOS_EPERM);
                EXPECT_EQ(cap_narrow(t, KOS_CAP_LIBC_LOCK, 0u, lock), -KOS_EPERM);
                EXPECT_EQ(handle_close(t, KOS_CAP_STDOUT, lock), -KOS_EPERM);
                EXPECT_EQ(cap_narrow(t, KOS_CAP_STDOUT, 0u, lock), -KOS_EPERM);
                CapEntry const* const e = cap_slot(t->caps, KOS_CAP_LIBC_LOCK);
                EXPECT_EQ(e->type, static_cast<uint8_t>(CapType::CAP_MUTEX));
                EXPECT_EQ(e->obj, libc);
                out = CapEntry{};
            }

            TEST_F(LibcLockSeat, a_create_never_takes_the_slot_an_address_space_still_needs)
            {
                Kernel& k = kernel();
                int taken = 0;
                while (KICKOS_MAX_MUTEXES - taken > 1)
                {
                    int const i = k.mutexes.alloc();
                    ASSERT_GE(i, 0);
                    k.mutex_refs[i] = 1;
                    taken++;
                }
                EXPECT_FALSE(cap_mutex_create_admitted());
                Thread* const t = member(1);
                IrqLock lock;
                int const libc = cap_libc_lock_take(t->task, lock);
                ASSERT_NE(libc, KCAP_LIBC_LOCK_NONE);
                cap_seat_libc_lock(t, libc);
                EXPECT_FALSE(cap_mutex_create_admitted());
            }

            TEST_F(LibcLockSeat, a_create_is_admitted_while_a_slot_beyond_the_reserve_is_free)
            {
                Kernel& k = kernel();
                int taken = 0;
                while (KICKOS_MAX_MUTEXES - taken > 2)
                {
                    int const i = k.mutexes.alloc();
                    ASSERT_GE(i, 0);
                    k.mutex_refs[i] = 1;
                    taken++;
                }
                EXPECT_TRUE(cap_mutex_create_admitted());
            }

            TEST_F(LibcLockSeat, a_take_given_back_frees_a_fresh_mutex_and_keeps_a_shared_one)
            {
                Thread* const t = member(1);
                IrqLock lock;
                int const fresh = cap_libc_lock_take(t->task, lock);
                cap_libc_lock_give(fresh, lock);
                EXPECT_EQ(kernel().mutexes.live_index(fresh), -1);
                int const held = cap_libc_lock_take(t->task, lock);
                cap_seat_libc_lock(t, held);
                int const again = cap_libc_lock_take(t->task, lock);
                cap_libc_lock_give(again, lock);
                EXPECT_EQ(refs(held), 1);
            }
        }
    }
}
