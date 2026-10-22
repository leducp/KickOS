// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The libc locks' words at the translating posture, on host words whose key is their address: a
// member's death hands a word with a waiter over to that waiter, and a word it held alone keeps
// its dead id for the next taker.

#include <kickos/cap.h>
#include <kickos/domain.h>
#include <kickos/instance.h>
#include <kickos/irqlock.h>
#include <kickos/sync.h>
#include <kickos/task.h>
#include <kickos/thread.h>

#include <kickos/sys/abi.h>

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

            static_assert(KICKOS_HAVE_ASPACE, "the death walk runs after the translating unmaps");

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

            uintptr_t va(uint32_t* word)
            {
                return reinterpret_cast<uintptr_t>(word);
            }
        }

        TEST_F(LibcLocksSpace, a_member_dying_inside_malloc_hands_its_word_over)
        {
            (void)seat_pool(0, PRIO_LOW); // keeps k.live above zero
            Task* const group = task(0);
            Thread* const dying = member_of(1, PRIO_MID, group);
            Thread* const waiter = member_of(2, PRIO_HIGH, group);
            attach_caps(dying, KICKOS_CAP_CHILD_WIDTH);
            uint32_t malloc_word = 0;
            run_as(dying);
            ASSERT_EQ(mutex_lock(va(&malloc_word), 0), 0);
            park_mutex_waiter(waiter, own_mutex(dying, &malloc_word));

            run_exit(0);

            EXPECT_EQ(malloc_word, waiter->lock_id) << "the death wrote another owner";
            EXPECT_EQ(waiter->wait_result, -KOS_EOWNERDEAD);
            EXPECT_EQ(dying->held_list, nullptr);
            EXPECT_EQ(kernel().word_active, nullptr) << "the emptied record was kept";
        }

        // Nobody waited, so the kernel never knew of either word: each keeps the dead id, and
        // the next taker of each steals it once.
        TEST_F(LibcLocksSpace, a_space_whose_last_member_died_holding_its_words_leaves_its_id)
        {
            (void)seat_pool(0, PRIO_LOW);
            Task* const group = task(0);
            Thread* const dying = member_of(1, PRIO_MID, group);
            attach_caps(dying, KICKOS_CAP_CHILD_WIDTH);
            uint32_t libc_word = 0;
            uint32_t malloc_word = 0;
            run_as(dying);
            ASSERT_EQ(mutex_lock(va(&libc_word), 0), 0);
            ASSERT_EQ(mutex_lock(va(&malloc_word), 0), 0);
            uint32_t const dead = dying->lock_id;

            run_exit(0);

            EXPECT_EQ(task_member_count(group), 0);
            EXPECT_EQ(libc_word, dead);
            EXPECT_EQ(malloc_word, dead) << "the death went unrecorded for want of a waiter";

            Thread* const later = member_of(2, PRIO_MID, task(1));
            run_as(later);
            EXPECT_EQ(mutex_lock(va(&libc_word), 0), -KOS_EOWNERDEAD);
            EXPECT_EQ(mutex_lock(va(&malloc_word), KOS_TIMEOUT_NONE), -KOS_EOWNERDEAD);
            EXPECT_EQ(mutex_unlock(va(&libc_word)), 0);
            EXPECT_EQ(mutex_lock(va(&libc_word), 0), 0) << "the death was reported twice";
        }
    }
}
