// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The locked pass of a mapping that owes its frames a sync, over the REAL kernel/mem/aspace.cc:
// a frame capability's map (aspace_cap_map) excused by the calling thread's live record of a sync
// made ahead of it, refused otherwise, and a record dropped by another thread's call only when
// that call completes. Two thread slots stand in for two callers; the map editor, the frame pool
// and the capability's run are seams that record nothing.

#include <kickos/aspace.h>
#include <kickos/domain.h>
#include <kickos/frame_pool.h>
#include <kickos/instance.h>
#include <kickos/kernel.h>
#include <kickos/sched.h>
#include <kickos/vrange.h>

#include <gtest/gtest.h>

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#if not KICKOS_HAVE_ASPACE or not KICKOS_ARCH_ALIAS_DCACHE
#error "this gate's posture is a translating backend with a cacheable kernel view"
#endif

namespace
{
    constexpr size_t G = 4096u;
    // The pool's frames, and a run of them a capability names.
    constexpr arch_phys_addr_t POOL_LO = 0x40000000u;
    constexpr arch_phys_addr_t POOL_HI = 0x40100000u;
    constexpr arch_phys_addr_t RUN = POOL_LO + 16u * G;
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
    namespace detail
    {
        constinit InstanceLocal<Kernel> g_instance;
    }

    namespace sched
    {
        Thread* current()
        {
            return g_current;
        }
    }

    void kpanic(char const* msg)
    {
        ADD_FAILURE() << "kernel panic: " << msg;
        abort();
    }

    struct arch_aspace* domain_space(Domain const*) { return nullptr; }
    VirtualRanges const* domain_ranges(Domain const*) { return nullptr; }
    VirtualRanges* domain_ranges_mut(Domain*) { return nullptr; }

    void frame_pool_phys_bounds(arch_phys_addr_t* lo, arch_phys_addr_t* hi)
    {
        *lo = POOL_LO;
        *hi = POOL_HI;
    }

    void frame_pool_free_run(arch_phys_addr_t, size_t, size_t) {}
    void* frame_pool_ptr(arch_phys_addr_t) { return nullptr; }

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
            return kickos::aspace_cap_map(space_, &ranges_[which], va, which, RUN, PAGES,
                                          ARCH_MAP_R | ARCH_MAP_W, ARCH_MAP_NOCACHE);
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
        uint32_t const r0 = kickos::presync_refusals();
        EXPECT_EQ(map(a(), 0, VA_A, PAGES, true), 0);
        EXPECT_FALSE(end(a(), true));
        EXPECT_EQ(kickos::presync_refusals(), r0);
        EXPECT_EQ(g_maps, 1);
    }

    TEST_F(PresyncMap, no_record_or_a_short_one_refuses_and_maps_nothing)
    {
        uint32_t const r0 = kickos::presync_refusals();
        EXPECT_NE(map(a(), 0, VA_A, PAGES, false), 0);
        EXPECT_TRUE(end(a(), false));
        EXPECT_NE(map(a(), 0, VA_A, PAGES - 1u, true), 0);
        EXPECT_TRUE(end(a(), false));
        EXPECT_EQ(kickos::presync_refusals(), r0 + 2u);
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
        EXPECT_EQ(kickos::presync_live_count(), 2u);
        EXPECT_FALSE(end(b(), false)) << "b's call did not complete";
        EXPECT_TRUE(kickos::presync_live_of(a())) << "a failed call drops nothing";
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
        EXPECT_FALSE(kickos::presync_live_of(a()));
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
        EXPECT_TRUE(kickos::presync_live_of(a()));
        g_current = a();
        EXPECT_EQ(map_locked(0, VA_A), 0);
        EXPECT_FALSE(end(a(), true));
    }

    TEST_F(PresyncMap, frames_outside_the_pool_owe_no_sync)
    {
        g_current = a();
        kickos::presync_begin();
        EXPECT_EQ(kickos::aspace_cap_map(space_, &ranges_[0], VA_A, 0, POOL_HI, PAGES,
                                         ARCH_MAP_R | ARCH_MAP_W, ARCH_MAP_NOCACHE),
                  0);
        EXPECT_FALSE(end(a(), true));
    }

    TEST_F(PresyncMap, a_pool_mapping_outside_a_call_working_outside_the_lock_halts)
    {
        g_current = a();
        kickos::presync_begin();
        EXPECT_EQ(kickos::aspace_cap_map(space_, &ranges_[0], VA_A, 0, RUN, PAGES,
                                         ARCH_MAP_R | ARCH_MAP_W, ARCH_MAP_NORMAL),
                  0);
        kickos::presync_commit();
        EXPECT_FALSE(kickos::presync_end());
        EXPECT_DEATH((void)kickos::aspace_cap_map(space_, &ranges_[1], VA_B, 1, RUN, PAGES,
                                                  ARCH_MAP_R | ARCH_MAP_W, ARCH_MAP_NORMAL),
                     "");
    }
}
