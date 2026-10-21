// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include <kickos/instance.h>
#include <kickos/irqlock.h>
#include <kickos/kernel.h>
#include <kickos/lock_id.h>
#include <kickos/thread.h>

#include "kseam_test.h"

namespace kickos
{
    namespace testfix
    {
        namespace
        {
            class LockId : public KSeam
            {
            };

            constexpr int SLOT_LAST = KICKOS_THREAD_SLOTS - 1;
            constexpr int SLOT_REUSED = 1;

            int claim()
            {
                IrqLock lock;
                return kernel().threads.alloc();
            }

            void unclaim(int index)
            {
                IrqLock lock;
                kernel().threads.release(index);
            }

            TEST_F(LockId, roots_id_is_one_and_derives_handle_zero)
            {
                ThreadPool const& pool = kernel().threads;
                ASSERT_EQ(pool.gen[ThreadPool::ROOT_INDEX], 0u);
                ASSERT_EQ(pool.gen_hi[ThreadPool::ROOT_INDEX], 0u);
                EXPECT_EQ(pool.lock_id_for(ThreadPool::ROOT_INDEX), 1u);
                EXPECT_EQ(lock_id_handle(1u), 0u);
            }

            TEST_F(LockId, every_slot_at_every_generation_half_derives_its_handle)
            {
                ThreadPool& pool = kernel().threads;
                uint16_t const gens[] = {0u, 1u, 0x7FFFu, 0x8000u, 0xFFFFu};
                for (int i = 0; i < KICKOS_THREAD_SLOTS; i++)
                {
                    for (uint16_t const g : gens)
                    {
                        pool.gen[i] = g;
                        pool.gen_hi[i] = static_cast<uint16_t>(g ^ 0x5A5Au);
                        uint32_t const id = pool.lock_id_for(i);
                        EXPECT_EQ(id >> 31, 0u) << "slot " << i << ": the id spent bit 31";
                        EXPECT_NE(id, 0u) << "slot " << i;
                        EXPECT_EQ(lock_id_handle(id), pool.handle_for(i))
                            << "slot " << i << " generation " << g;
                    }
                }
            }

            TEST_F(LockId, the_widest_slot_keeps_its_index_and_a_generation_bit)
            {
                ThreadPool& pool = kernel().threads;
                pool.gen[SLOT_LAST] = 1u;
                uint32_t const id = pool.lock_id_for(SLOT_LAST);
                EXPECT_EQ(id & LOCK_ID_SLOT_MASK, static_cast<uint32_t>(KICKOS_THREAD_SLOTS));
                EXPECT_EQ(id >> LOCK_ID_SLOT_BITS, 1u);
            }

            // A reclaim across the 16-bit wrap steps the high half, so the id differs from every
            // earlier occupant's while the handle wraps; the spawn's revert restores both halves.
            TEST_F(LockId, a_reclaim_and_its_revert_across_the_wrap_move_the_generation_as_one)
            {
                ThreadPool& pool = kernel().threads;
                if (pool.next <= SLOT_REUSED)
                {
                    pool.next = SLOT_REUSED + 1;
                }
                Thread& was = pool.slots[SLOT_REUSED];
                was.state.to<ThreadState::EXITED>();
                pool.gen[SLOT_REUSED] = 0xFFFFu;
                pool.gen_hi[SLOT_REUSED] = 0u;
                uint32_t const before = pool.lock_id_for(SLOT_REUSED);

                ASSERT_EQ(claim(), SLOT_REUSED);
                EXPECT_EQ(pool.gen[SLOT_REUSED], 0u);
                EXPECT_EQ(pool.gen_hi[SLOT_REUSED], 1u) << "the wrap did not carry";
                uint32_t const after = pool.lock_id_for(SLOT_REUSED);
                EXPECT_NE(after, before);
                EXPECT_EQ(lock_id_handle(after), pool.handle_for(SLOT_REUSED));

                unclaim(SLOT_REUSED);
                EXPECT_EQ(pool.gen[SLOT_REUSED], 0xFFFFu);
                EXPECT_EQ(pool.gen_hi[SLOT_REUSED], 0u) << "the revert left the high half stepped";
                EXPECT_EQ(pool.lock_id_for(SLOT_REUSED), before);
            }
        }
    }
}
