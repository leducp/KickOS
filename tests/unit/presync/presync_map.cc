// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The locked pass of a mapping that owes its frames a sync, over the REAL kernel/mem/aspace.cc:
// a frame capability's map (aspace_cap_map) excused by the calling thread's live record of a sync
// made ahead of it, refused otherwise, and a record dropped by another thread's call only when
// that call completes. Two thread slots stand in for two callers; the map editor and the
// capability's run are seams that record nothing.

#include <kickos/aspace.h>
#include <kickos/domain.h>
#include <kickos/frame_pool.h>
#include <kickos/instance.h>
#include <kickos/kernel.h>
#include <kickos/sched.h>
#include <kickos/vrange.h>

#include <kickos/sys/errno.h>

#include "host_frame_pool.h"

#include <gtest/gtest.h>

#include <stddef.h>
#include <stdint.h>

#if not KICKOS_HAVE_ASPACE or not KICKOS_ARCH_ALIAS_DCACHE
#error "this gate's posture is a translating backend with a cacheable kernel view"
#endif

namespace
{
    constexpr size_t G = 4096u;
    // A run of the pool's frames a capability names, and the first frame past the pool.
    constexpr arch_phys_addr_t RUN = kickos::testfix::HOST_POOL_LO + 16u * G;
    constexpr arch_phys_addr_t POOL_HI =
        kickos::testfix::HOST_POOL_LO + kickos::testfix::HOST_POOL_FRAMES * G;
    constexpr uint32_t PAGES = 4;
    constexpr uintptr_t VA_A = 0x10000000u;
    constexpr uintptr_t VA_B = 0x20000000u;

    kickos::Thread* g_current = nullptr;
    int g_maps = 0;
}

extern "C"
{
    size_t arch_aspace_granule(void) { return G; }
    uintptr_t arch_aspace_user_offset(void) { return 0; }
    arch_phys_addr_t arch_aspace_frame_at(struct arch_aspace*, uintptr_t) { return 0; }

    enum arch_aspace_result arch_aspace_map(struct arch_aspace*, uintptr_t, arch_phys_addr_t,
                                            size_t, uint32_t, enum arch_map_memtype)
    {
        g_maps++;
        return ARCH_ASPACE_OK;
    }

    enum arch_aspace_result arch_aspace_unmap(struct arch_aspace*, uintptr_t, size_t)
    {
        return ARCH_ASPACE_OK;
    }

    arch_irq_state_t arch_irq_save(void) { return arch_irq_state_t{}; }
    void arch_irq_restore(arch_irq_state_t) {}
}

namespace kickos
{
    namespace sched
    {
        Thread* current()
        {
            return g_current;
        }
    }

    struct arch_aspace* domain_space(Domain const*) { return nullptr; }
    VirtualRanges const* domain_ranges(Domain const*) { return nullptr; }
    VirtualRanges* domain_ranges_mut(Domain*) { return nullptr; }

    bool frame_run_ref(int) { return true; }
    void frame_run_release(int) {}
    int frame_run_slot_of(int obj) { return obj; }
    bool frame_run_sync_owed(int) { return false; }
    void frame_run_set_sync_owed(int, bool) {}
}

namespace
{
    using kickos::Thread;

    class PresyncMap : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            for (Thread* t : {a(), b()})
            {
                g_current = t;
                kickos::presync_exit();
            }
            ASSERT_TRUE(ranges_[0].init(G));
            ASSERT_TRUE(ranges_[1].init(G));
            g_maps = 0;
        }

        static Thread* a() { return &kickos::kernel().threads.slots[0]; }
        static Thread* b() { return &kickos::kernel().threads.slots[1]; }

        static bool live(Thread const* t)
        {
            return kickos::kernel().presync[kickos::kernel().threads.index_of(t)].live;
        }

        // One round of a call `t` makes: noting `pages` granules of the run when `note`, then
        // the locked pass mapping the whole run non-cacheable at `va`.
        int map(Thread* t, int which, uintptr_t va, size_t pages, bool note)
        {
            g_current = t;
            kickos::presync_begin();
            if (note)
            {
                kickos::presync_note(RUN, pages);
            }
            return map_locked(which, va);
        }

        int map_locked(int which, uintptr_t va)
        {
            return map_at(which, &va, ARCH_MAP_NOCACHE);
        }

        int map_at(int which, uintptr_t* va, enum arch_map_memtype type)
        {
            return kickos::aspace_cap_map(space_, &ranges_[which], va, which, RUN, PAGES,
                                          ARCH_MAP_R | ARCH_MAP_W, type);
        }

        // Ends the round of `t`'s call, which `completed` or failed; true when it was refused.
        bool end(Thread* t, bool completed)
        {
            g_current = t;
            if (completed)
            {
                kickos::presync_commit();
            }
            return kickos::presync_end();
        }

        struct arch_aspace* const space_ = reinterpret_cast<struct arch_aspace*>(0x1000);
        kickos::VirtualRanges ranges_[2];
    };

    TEST_F(PresyncMap, a_live_record_covering_the_run_excuses_the_locked_pass)
    {
        EXPECT_EQ(map(a(), 0, VA_A, PAGES, true), 0);
        EXPECT_FALSE(end(a(), true));
        EXPECT_EQ(g_maps, 1);
    }

    TEST_F(PresyncMap, no_record_or_a_short_one_refuses_and_maps_nothing)
    {
        EXPECT_NE(map(a(), 0, VA_A, PAGES, false), 0);
        EXPECT_TRUE(end(a(), false));
        EXPECT_NE(map(a(), 0, VA_A, PAGES - 1u, true), 0);
        EXPECT_TRUE(end(a(), false));
        EXPECT_EQ(g_maps, 0);
    }

    TEST_F(PresyncMap, a_round_forgets_the_last_round_s_record)
    {
        EXPECT_EQ(map(a(), 0, VA_A, PAGES, true), 0);
        EXPECT_FALSE(end(a(), true));
        EXPECT_NE(map(a(), 1, VA_B, PAGES, false), 0) << "the record died with its call";
        EXPECT_TRUE(end(a(), false));
    }

    TEST_F(PresyncMap, another_call_s_completed_map_drops_the_record_and_only_on_completion)
    {
        g_current = a();
        kickos::presync_begin();
        kickos::presync_note(RUN, PAGES);
        // b's map is excused by b's own record and installs a mapping over the noted frames.
        EXPECT_EQ(map(b(), 1, VA_B, PAGES, true), 0);
        g_current = a();
        EXPECT_EQ(kickos::kernel().presync_live, 2u);
        EXPECT_FALSE(end(b(), false)) << "b's call did not complete";
        EXPECT_TRUE(live(a())) << "a failed call drops nothing";
        g_current = a();
        EXPECT_EQ(map_locked(0, VA_A), 0);
        EXPECT_FALSE(end(a(), true));
    }

    TEST_F(PresyncMap, a_completed_map_by_another_call_refuses_the_next_locked_pass)
    {
        g_current = a();
        kickos::presync_begin();
        kickos::presync_note(RUN, PAGES);
        EXPECT_EQ(map(b(), 1, VA_B, PAGES, true), 0);
        EXPECT_FALSE(end(b(), true));
        EXPECT_FALSE(live(a()));
        g_current = a();
        EXPECT_NE(map_locked(0, VA_A), 0);
        EXPECT_TRUE(end(a(), false));
    }

    TEST_F(PresyncMap, a_refused_round_drops_nothing)
    {
        g_current = a();
        kickos::presync_begin();
        kickos::presync_note(RUN, PAGES);
        EXPECT_NE(map(b(), 1, VA_B, PAGES, false), 0);
        EXPECT_TRUE(end(b(), true));
        EXPECT_TRUE(live(a()));
        g_current = a();
        EXPECT_EQ(map_locked(0, VA_A), 0);
        EXPECT_FALSE(end(a(), true));
    }

    TEST_F(PresyncMap, frames_outside_the_pool_owe_no_sync)
    {
        g_current = a();
        kickos::presync_begin();
        uintptr_t va = VA_A;
        EXPECT_EQ(kickos::aspace_cap_map(space_, &ranges_[0], &va, 0, POOL_HI, PAGES,
                                         ARCH_MAP_R | ARCH_MAP_W, ARCH_MAP_NOCACHE),
                  0);
        EXPECT_FALSE(end(a(), true));
    }

    TEST_F(PresyncMap, a_pool_mapping_outside_a_call_working_outside_the_lock_halts)
    {
        g_current = a();
        kickos::presync_begin();
        uintptr_t va = VA_A;
        EXPECT_EQ(map_at(0, &va, ARCH_MAP_NORMAL), 0);
        kickos::presync_commit();
        EXPECT_FALSE(kickos::presync_end());
        va = VA_B;
        EXPECT_DEATH((void)map_at(1, &va, ARCH_MAP_NORMAL), "");
    }

    class FrameMap : public PresyncMap
    {
    };

    TEST_F(FrameMap, a_second_va_zero_map_in_one_space_is_enomem)
    {
        g_current = a();
        kickos::presync_begin();
        uintptr_t va = 0;
        ASSERT_EQ(map_at(0, &va, ARCH_MAP_NORMAL), 0);
        EXPECT_NE(ranges_[0].at_base(RUN), nullptr) << "the range was registered elsewhere";
        uintptr_t again = 0;
        EXPECT_EQ(map_at(0, &again, ARCH_MAP_NORMAL), -KOS_ENOMEM);
        EXPECT_EQ(again, 0u) << "a refused map wrote the address";
        EXPECT_EQ(g_maps, 1);
        EXPECT_FALSE(end(a(), true));
    }
}
