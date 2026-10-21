// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Which thread may name a user word and which could hold it, on a board that does not
// translate: the key is the address, a thread names what its region set or the static-data
// fallback lets it write, and the sim adds the host's own memory outside its arena. Built once
// per posture; an arm that only one posture answers sits under its #if.

#include <kickos/ampshare.h>
#include <kickos/arch/arch.h>
#include <kickos/instance.h>
#include <kickos/irqlock.h>
#include <kickos/kernel.h>
#include <kickos/sync.h>
#include <kickos/thread.h>
#include <kickos/word.h>

#include <kickos/sys/errno.h>

#include <stdint.h>
#include <stdlib.h>
#include <sys/mman.h>

#include "kseam_test.h"

#if defined(WORDREF_BOUNDED)
#include <kickos/arch/atomic_cas.h>
#endif

static_assert(not KICKOS_HAVE_ASPACE, "the translating posture's gate is sys_word_ref");

#if defined(WORDREF_BOUNDED)
namespace
{
    // How many of the next stores are lost.
    uint32_t g_lose = 0;
}

bool arch_cas_u32_once(uint32_t* word, uint32_t want, uint32_t desired, uint32_t* observed)
{
    *observed = *word;
    if (*observed != want)
    {
        return false;
    }
    if (g_lose != 0)
    {
        g_lose--;
        return false;
    }
    *word = desired;
    return true;
}
#endif

namespace
{
    constexpr size_t BLOCK = 256u;
    // The arena: the sim's mprotect boundary, and the blocks a region board grants from.
    alignas(BLOCK) unsigned char g_arena[8u * BLOCK];
    // The app's static data, which no region names on a flat board.
    alignas(BLOCK) unsigned char g_statics[BLOCK];
}

extern "C"
{
    uintptr_t arch_ram_base(void)
    {
        return reinterpret_cast<uintptr_t>(g_arena);
    }

    size_t arch_ram_size(void)
    {
        return sizeof(g_arena);
    }

    bool arch_user_text_readable(uintptr_t, size_t)
    {
        return false;
    }

    // The fallback as each posture answers it: the static RAM below the arena on a flat board,
    // the host image on the sim, nothing where regions are enforced.
    bool arch_user_data_writable(uintptr_t ptr, size_t len)
    {
#if KICKOS_ARCH_SIM or not KICKOS_MEMORY_ENFORCED
        uintptr_t const lo = reinterpret_cast<uintptr_t>(g_statics);
        return ptr >= lo and ptr + len <= lo + sizeof(g_statics);
#else
        (void)ptr;
        (void)len;
        return false;
#endif
    }
}

namespace kickos
{
    namespace testfix
    {
        namespace
        {
            constexpr uint32_t RW = ARCH_MPU_R | ARCH_MPU_W;
            constexpr uintptr_t AT = 8u;

            uintptr_t block(size_t i)
            {
                return reinterpret_cast<uintptr_t>(g_arena) + i * BLOCK;
            }

            class WordClasses : public KSeam
            {
              protected:
                void SetUp() override
                {
                    KSeam::SetUp();
                    caller_ = seat_pool(0, 4);
                    peer_ = seat_pool(1, 4);
                    stranger_ = seat_pool(2, 4);
                }

                static void grant(Thread* t, uintptr_t base, uint32_t attr)
                {
                    ASSERT_TRUE(t->mpu.add(base, BLOCK, attr));
                }

                static int ref(Thread const* t, uintptr_t va, WordRef* w)
                {
                    IrqLock lock;
                    return word_ref(t, va, w);
                }

                static bool holds(Thread const* t, WordRef const& w)
                {
                    IrqLock lock;
                    return word_could_hold(t, w);
                }

                // The system calls' own path, as `t`: a try, and an unlock.
                static int try_as(Thread* t, uintptr_t va)
                {
                    Thread*& cur = kernel().current(kickos_kernel_core());
                    Thread* const was = cur;
                    cur = t;
                    int const rc = mutex_lock(va, 0);
                    cur = was;
                    return rc;
                }

                static int unlock_as(Thread* t, uintptr_t va)
                {
                    Thread*& cur = kernel().current(kickos_kernel_core());
                    Thread* const was = cur;
                    cur = t;
                    int const rc = mutex_unlock(va);
                    cur = was;
                    return rc;
                }

                // A free word the caller may not name is neither taken nor, held by the caller,
                // released: the refusal comes before the kernel reaches the word.
                void expect_untouched(uintptr_t va, int refusal)
                {
                    uint32_t* const word = reinterpret_cast<uint32_t*>(va);
                    *word = 0;
                    EXPECT_EQ(try_as(caller_, va), refusal) << "a lock of the word at " << va;
                    EXPECT_EQ(*word, 0u) << "a refused lock took the word";
                    *word = caller_->lock_id;
                    EXPECT_EQ(unlock_as(caller_, va), refusal) << "an unlock of the word at " << va;
                    EXPECT_EQ(*word, caller_->lock_id) << "a refused unlock released the word";
                }

                Thread* caller_ = nullptr;
                Thread* peer_ = nullptr;
                Thread* stranger_ = nullptr;
            };

            TEST_F(WordClasses, a_word_a_region_grants_written_is_keyed_on_its_address)
            {
                grant(caller_, block(0), RW);
                grant(peer_, block(0), RW);
                WordRef w;
                ASSERT_EQ(ref(caller_, block(0) + AT, &w), 0);
                EXPECT_EQ(w.key, block(0) + AT);
                EXPECT_EQ(w.va, block(0) + AT);
                EXPECT_TRUE(holds(caller_, w));
                EXPECT_TRUE(holds(peer_, w)) << "a holder granted the block";
            }

            // A region backend makes every other thread's block private: a stack, a self-grant,
            // a window or a task's data region the caller was not granted.
            TEST_F(WordClasses, another_thread_s_block_is_refused_and_held_only_by_its_grantees)
            {
                grant(peer_, block(1), RW);
                WordRef w;
                EXPECT_EQ(ref(caller_, block(1) + AT, &w), -KOS_EFAULT);
                ASSERT_EQ(ref(peer_, block(1) + AT, &w), 0);
#if KICKOS_MEMORY_ENFORCED
                EXPECT_FALSE(holds(stranger_, w)) << "a thread the block is not granted to";
#else
                EXPECT_TRUE(holds(stranger_, w)) << "a flat board's every thread writes it";
#endif
            }

            TEST_F(WordClasses, a_word_the_caller_can_only_read_is_refused)
            {
                grant(caller_, block(2), ARCH_MPU_R);
                grant(caller_, block(3), RW);
                grant(caller_, block(3), ARCH_MPU_R);
                grant(caller_, block(4), ARCH_MPU_R | ARCH_MPU_X);
                WordRef w;
                EXPECT_EQ(ref(caller_, block(2) + AT, &w), -KOS_EFAULT) << "a read-only block";
                EXPECT_EQ(ref(caller_, block(3) + AT, &w), -KOS_EFAULT)
                    << "a read-only region over a writable one";
                EXPECT_EQ(ref(caller_, block(4) + AT, &w), -KOS_EFAULT) << "text";
            }

            TEST_F(WordClasses, a_misaligned_uncached_or_device_word_is_refused)
            {
                grant(caller_, block(0), RW);
                grant(caller_, block(5), RW | ARCH_MPU_NOCACHE);
                grant(caller_, block(6), RW | ARCH_MPU_DEV);
                WordRef w;
                EXPECT_EQ(ref(caller_, block(0) + 2u, &w), -KOS_EINVAL) << "misaligned";
                EXPECT_EQ(ref(caller_, block(5) + AT, &w), -KOS_EINVAL) << "uncached";
                EXPECT_EQ(ref(caller_, block(6) + AT, &w), -KOS_EINVAL) << "device";
                EXPECT_EQ(ref(caller_, block(5) - sizeof(uint32_t), &w), -KOS_EFAULT)
                    << "the word below an uncached block is no region's";
            }

            // The app's statics: a region on a region board, the fallback on a flat one, host
            // memory on the sim.
            TEST_F(WordClasses, a_static_word_is_named_by_whatever_reaches_the_statics)
            {
                uintptr_t const va = reinterpret_cast<uintptr_t>(g_statics) + AT;
#if KICKOS_MEMORY_ENFORCED and not KICKOS_ARCH_SIM
                WordRef w;
                EXPECT_EQ(ref(caller_, va, &w), -KOS_EFAULT) << "no region names the statics";
                ASSERT_TRUE(caller_->mpu.add(reinterpret_cast<uintptr_t>(g_statics), BLOCK, RW));
                ASSERT_TRUE(peer_->mpu.add(reinterpret_cast<uintptr_t>(g_statics), BLOCK, RW));
#endif
                WordRef s;
                ASSERT_EQ(ref(caller_, va, &s), 0);
                EXPECT_EQ(s.key, va);
                EXPECT_TRUE(holds(peer_, s));
            }

#if KICKOS_ARCH_SIM
            // The host's heap, and the host block standing in for a stack under 64 KiB, are
            // neither the image nor the arena, and every thread of the sim writes them.
            TEST_F(WordClasses, the_sim_names_host_memory_and_keeps_the_arena_s_rule_inside_it)
            {
                uint32_t* const heap = static_cast<uint32_t*>(malloc(64));
                uint32_t* const stack = static_cast<uint32_t*>(malloc(4096));
                ASSERT_NE(heap, nullptr);
                ASSERT_NE(stack, nullptr);
                WordRef w;
                EXPECT_EQ(ref(caller_, reinterpret_cast<uintptr_t>(heap + 1), &w), 0)
                    << "a host-heap word";
                EXPECT_TRUE(holds(stranger_, w));
                EXPECT_EQ(ref(caller_, reinterpret_cast<uintptr_t>(stack + 512), &w), 0)
                    << "a host-block stack word";
                EXPECT_TRUE(holds(stranger_, w));
                grant(peer_, block(1), RW);
                EXPECT_EQ(ref(caller_, block(1) + AT, &w), -KOS_EFAULT)
                    << "another domain's arena word";
                free(stack);
                free(heap);
            }
#endif

            TEST_F(WordClasses, a_mutex_on_a_read_only_uncached_or_device_word_leaves_it_alone)
            {
                grant(caller_, block(2), ARCH_MPU_R);
                grant(caller_, block(3), RW);
                grant(caller_, block(3), ARCH_MPU_R);
                grant(caller_, block(5), RW | ARCH_MPU_NOCACHE);
                grant(caller_, block(6), RW | ARCH_MPU_DEV);
                ASSERT_NE(caller_->lock_id, 0u);
                expect_untouched(block(2) + AT, -KOS_EFAULT);
                expect_untouched(block(3) + AT, -KOS_EFAULT);
                expect_untouched(block(5) + AT, -KOS_EINVAL);
                expect_untouched(block(6) + AT, -KOS_EINVAL);
                grant(caller_, block(0), RW);
                uint32_t* const word = reinterpret_cast<uint32_t*>(block(0) + AT);
                *word = 0;
                EXPECT_EQ(try_as(caller_, block(0) + AT), 0) << "fixture: a writable word";
                EXPECT_EQ(*word, caller_->lock_id);
                EXPECT_EQ(unlock_as(caller_, block(0) + AT), 0);
                EXPECT_EQ(*word, 0u);
            }

#if KICKOS_AMP_SHARE
            // The share stands at a fixed address, so the posture puts it where the host can map
            // it.
            TEST_F(WordClasses, a_mutex_on_a_word_in_the_amp_share_leaves_it_alone)
            {
                void* const at = mmap(reinterpret_cast<void*>(AMP_SHARE_BASE), AMP_SHARE_SIZE,
                                      PROT_READ | PROT_WRITE,
                                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
                ASSERT_EQ(at, reinterpret_cast<void*>(AMP_SHARE_BASE)) << "fixture: the share";
                ASSERT_TRUE(caller_->mpu.add(AMP_SHARE_BASE, AMP_SHARE_SIZE, RW));
                ASSERT_NE(caller_->lock_id, 0u);
                expect_untouched(AMP_SHARE_BASE, -KOS_EINVAL);
                expect_untouched(AMP_SHARE_BASE + AMP_SHARE_SIZE - sizeof(uint32_t), -KOS_EINVAL);
                munmap(at, AMP_SHARE_SIZE);
            }
#endif

            TEST_F(WordClasses, the_kernel_reaches_the_word_at_its_address)
            {
                grant(caller_, block(7), RW);
                uint32_t* const word = reinterpret_cast<uint32_t*>(block(7) + AT);
                *word = 0;
                WordRef w;
                ASSERT_EQ(ref(caller_, block(7) + AT, &w), 0);
                IrqLock lock;
                WordAccess access(w);
                ASSERT_TRUE(access.reached());
                access.store(0x80000001u);
                EXPECT_EQ(*word, 0x80000001u);
                uint32_t expected = 1u;
                EXPECT_FALSE(access.cas(&expected, 2u));
                EXPECT_EQ(expected, 0x80000001u);
                EXPECT_TRUE(access.cas(&expected, 2u));
                EXPECT_EQ(access.load(), 2u);
            }

#if defined(WORDREF_BOUNDED)
            // A word another core keeps writing loses every store: the call ends, answering the
            // word as read, and the caller retries with its lock dropped.
            TEST_F(WordClasses, a_store_lost_at_every_attempt_answers_differed_with_the_word)
            {
                grant(caller_, block(7), RW);
                uint32_t* const word = reinterpret_cast<uint32_t*>(block(7) + AT);
                *word = 5u;
                WordRef w;
                ASSERT_EQ(ref(caller_, block(7) + AT, &w), 0);
                IrqLock lock;
                WordAccess access(w);
                ASSERT_TRUE(access.reached());
                uint32_t expected = 5u;
                g_lose = ARCH_CAS_U32_STORES;
                EXPECT_FALSE(access.cas(&expected, 9u));
                EXPECT_EQ(expected, 5u);
                EXPECT_EQ(*word, 5u);
                EXPECT_EQ(g_lose, 0u) << "the bound was not reached";
                g_lose = ARCH_CAS_U32_STORES - 1u;
                EXPECT_TRUE(access.cas(&expected, 9u));
                EXPECT_EQ(*word, 9u);
            }
#endif
        }
    }
}
