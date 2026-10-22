// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A run a system call stages outside every range, over the REAL kernel/mem/aspace.cc: a
// reservation's clear and a new space's copy of the pristine image of the static data, filled
// outside the lock, taken only by a locked pass that asks for exactly what was staged, and
// otherwise given back granule by granule when the round ends, or whole when the thread leaves.

#include <kickos/aspace.h>
#include <kickos/domain.h>
#include <kickos/frame_pool.h>
#include <kickos/instance.h>
#include <kickos/kernel.h>
#include <kickos/sched.h>
#include <kickos/vrange.h>

#include "acquire_holds.h"
#include "host_frame_pool.h"

#include <gtest/gtest.h>

#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if not KICKOS_PRESYNC
#error "this gate's posture is a translating backend working outside the lock"
#endif

namespace
{
    using kickos::testfix::host_pool_bytes;
    using kickos::testfix::host_pool_mark;
    using kickos::testfix::host_pool_used;
    using kickos::testfix::HostPoolMark;

    constexpr size_t G = kickos::testfix::HOST_POOL_GRANULE;
    constexpr size_t DATA_PAGES = PRESYNC_STAGE_DATA_BYTES / G;
    static_assert(DATA_PAGES * G == PRESYNC_STAGE_DATA_BYTES and DATA_PAGES >= 2u,
                  "the linked image data is whole granules, at least two");
    constexpr size_t TEXT_PAGES = PRESYNC_STAGE_TEXT_BYTES / G;
    static_assert(TEXT_PAGES * G == PRESYNC_STAGE_TEXT_BYTES and TEXT_PAGES >= 1u,
                  "the linked image text is whole granules, at least one");

    // What the loader put in the image's data, and what the first space writes over it.
    constexpr unsigned char PRISTINE = 0x5Au;
    constexpr unsigned char LIVE = 0xA5u;

    int g_windows = 0;
    kickos::testfix::AcquireHolds g_holds;
    bool g_locked = false;
    int g_locked_copies = 0;
    int g_syncs = 0;

    struct MapCall
    {
        struct arch_aspace* space;
        uintptr_t va;
        arch_phys_addr_t pa;
        size_t pages;
        uint32_t rights;
    };
    constexpr size_t MAPS_MAX = 8;
    MapCall g_map_log[MAPS_MAX] = {};
    size_t g_map_count = 0;
    // The window at which the calling thread is slain, abandoning its call as a redirect does.
    int g_slay_at = 0;
    jmp_buf g_slain;
    kickos::Thread* g_current = nullptr;
    // The calling thread's own space.
    struct arch_aspace* g_own_space = nullptr;

    struct arch_aspace* const SPACE_A = reinterpret_cast<struct arch_aspace*>(0x1000);
    struct arch_aspace* const SPACE_B = reinterpret_cast<struct arch_aspace*>(0x2000);
    struct arch_aspace* const SPACE_NEW = reinterpret_cast<struct arch_aspace*>(0x3000);
}

extern "C"
{
    // The app's text and static data, which the linker options place these arrays' bytes under.
    alignas(4096) unsigned char g_stage_rom[TEXT_PAGES * G];
    alignas(4096) unsigned char g_stage_sram[DATA_PAGES * G];

    size_t arch_aspace_granule(void) { return G; }
    uintptr_t arch_aspace_user_offset(void) { return 0; }
    arch_phys_addr_t arch_aspace_frame_at(struct arch_aspace*, uintptr_t) { return 0; }

    enum arch_aspace_result arch_aspace_map(struct arch_aspace* space, uintptr_t va,
                                            arch_phys_addr_t pa, size_t pages, uint32_t rights,
                                            enum arch_map_memtype)
    {
        if (g_map_count < MAPS_MAX)
        {
            g_map_log[g_map_count] = MapCall{space, va, pa, pages, rights};
        }
        g_map_count++;
        return ARCH_ASPACE_OK;
    }

    enum arch_aspace_result arch_aspace_unmap(struct arch_aspace*, uintptr_t, size_t)
    {
        return ARCH_ASPACE_OK;
    }

    void* arch_aspace_acquire(struct arch_aspace* space, uintptr_t va, bool*,
                              arch_phys_addr_t* frame)
    {
        if (frame != nullptr)
        {
            *frame = 0;
        }
        g_holds.take(space, va);
        return reinterpret_cast<void*>(va);
    }

    void arch_aspace_release(struct arch_aspace* space, uintptr_t va)
    {
        g_holds.give(space, va);
    }

    arch_irq_state_t arch_irq_save(void) { return arch_irq_state_t{}; }
    void arch_irq_restore(arch_irq_state_t) {}
    void arch_irq_window(void)
    {
        g_windows++;
        if (g_windows == g_slay_at)
        {
            longjmp(g_slain, 1);
        }
    }

    void arch_aspace_destroy(struct arch_aspace*) {}
    void arch_aspace_activate(struct arch_aspace*) {}
    struct arch_aspace* arch_aspace_boot(void) { return nullptr; }
    uint64_t arch_clock_now(void) { return 0; }
    bool arch_irq_inject(int) { return true; }

    void* kmemcpy(void* dst, void const* src, size_t n)
    {
        if (g_locked)
        {
            g_locked_copies++;
        }
        return memcpy(dst, src, n);
    }
    void* kmemset(void* dst, int c, size_t n) { return memset(dst, c, n); }
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

    void alias_sync(void const*, size_t)
    {
        g_syncs++;
    }
    void frame_run_release_by_slot(int) {}

    struct arch_aspace* domain_space(Domain const*) { return g_own_space; }
    VirtualRanges const* domain_ranges(Domain const*) { return nullptr; }
    VirtualRanges* domain_ranges_mut(Domain*) { return nullptr; }
    Domain* task_domain(Task const*) { return nullptr; }
}

namespace
{
    using kickos::Thread;

    // The first space borrows the image's data, which it then writes over: every later case
    // must find the loader's bytes and never the first space's.
    class PresyncStage : public ::testing::Test
    {
      protected:
        static void SetUpTestSuite()
        {
            if (g_seeded)
            {
                return;
            }
            g_seeded = true;
            memset(g_stage_sram, PRISTINE, sizeof(g_stage_sram));
            g_map_count = 0;
            ASSERT_TRUE(kickos::aspace_image_seed(SPACE_A, &g_first));
            ASSERT_EQ(g_map_count, 2u) << "the first space maps its text and its data";
            g_home_text = g_map_log[0];
            g_home_data = g_map_log[1];
            g_base_frames = host_pool_used();
            memset(g_stage_sram, LIVE, sizeof(g_stage_sram));
        }

        void SetUp() override
        {
            g_current = a();
            kickos::presync_exit();
            kickos::testfix::host_pool_reset_counters();
            g_windows = 0;
            g_holds = kickos::testfix::AcquireHolds{};
            g_locked = false;
            g_locked_copies = 0;
            g_syncs = 0;
            g_map_count = 0;
            g_slay_at = 0;
            g_own_space = SPACE_A;
            ASSERT_TRUE(ranges_.init(G));
            ASSERT_EQ(host_pool_used(), g_base_frames);
        }

        void TearDown() override
        {
            g_current = b();
            kickos::presync_exit();
            g_current = a();
            kickos::presync_exit();
            EXPECT_EQ(host_pool_used(), g_base_frames) << "a staged run outlived its call";
            EXPECT_EQ(g_holds.live, 0u) << "an acquire was never released";
            EXPECT_EQ(g_holds.unpaired, 0u) << "a release met no acquire";
            EXPECT_EQ(kickos::testfix::host_pool_double_frees(), 0u);
            EXPECT_EQ(kickos::testfix::host_pool_outside_frees(), 0u);
        }

        static Thread* a() { return &kickos::kernel().threads.slots[0]; }
        static Thread* b() { return &kickos::kernel().threads.slots[1]; }

        static bool locked_seed(struct arch_aspace* space, kickos::VirtualRanges* ranges)
        {
            g_locked = true;
            bool const took = kickos::aspace_image_seed(space, ranges);
            g_locked = false;
            return took;
        }

        // One round: stage the copy, fill it outside the lock unless `filled` is false, then the
        // locked pass seeds `space`; true when that pass took the stage.
        static bool seed_round(struct arch_aspace* space, kickos::VirtualRanges* ranges,
                               bool filled = true)
        {
            kickos::presync_begin();
            kickos::presync_stage_image();
            if (filled)
            {
                kickos::presync_run();
            }
            return locked_seed(space, ranges);
        }

        bool spawn_round(bool filled = true)
        {
            return seed_round(SPACE_NEW, &ranges_, filled);
        }

        static MapCall const* mapped(struct arch_aspace* space, uintptr_t va)
        {
            for (size_t i = 0; i < g_map_count and i < MAPS_MAX; i++)
            {
                if (g_map_log[i].space == space and g_map_log[i].va == va)
                {
                    return &g_map_log[i];
                }
            }
            return nullptr;
        }

        static bool frames_hold(arch_phys_addr_t pa, size_t pages, unsigned char want)
        {
            unsigned char const* const bytes = host_pool_bytes(pa, pages);
            if (bytes == nullptr)
            {
                return false;
            }
            for (size_t i = 0; i < pages * G; i++)
            {
                if (bytes[i] != want)
                {
                    return false;
                }
            }
            return true;
        }

        // The round's end, the lock released: true when the call goes round again.
        bool end_round()
        {
            bool const again = kickos::presync_end();
            kickos::presync_release(false);
            return again;
        }

        static bool g_seeded;
        static size_t g_base_frames;
        static kickos::VirtualRanges g_first;
        static MapCall g_home_text;
        static MapCall g_home_data;
        kickos::VirtualRanges ranges_;
    };

    bool PresyncStage::g_seeded = false;
    size_t PresyncStage::g_base_frames = 0;
    kickos::VirtualRanges PresyncStage::g_first;
    MapCall PresyncStage::g_home_text = {};
    MapCall PresyncStage::g_home_data = {};

    TEST_F(PresyncStage, a_stage_is_taken_and_holds_the_image_s_pristine_bytes)
    {
        HostPoolMark const before = host_pool_mark();
        ASSERT_TRUE(spawn_round());
        EXPECT_FALSE(end_round());
        EXPECT_EQ(host_pool_used(), g_base_frames + DATA_PAGES) << "the new space holds its copy";
        EXPECT_EQ(g_windows, static_cast<int>(DATA_PAGES));
        EXPECT_EQ(g_locked_copies, 0) << "a granule was copied under the lock";
        EXPECT_EQ(g_holds.peak, 0u) << "a live space's page was read";
        MapCall const* const data = mapped(SPACE_NEW, g_home_data.va);
        ASSERT_NE(data, nullptr);
        EXPECT_TRUE(frames_hold(data->pa, DATA_PAGES, PRISTINE)) << "the first space's bytes";
        kickos::testfix::host_pool_free_since(before);
    }

    TEST_F(PresyncStage, a_stage_the_round_did_not_fill_is_refused_and_given_back)
    {
        EXPECT_FALSE(spawn_round(false));
        EXPECT_FALSE(kickos::presync_record()->staged_full);
        EXPECT_EQ(host_pool_used(), g_base_frames + DATA_PAGES) << "still staged until the end";
        EXPECT_TRUE(end_round()) << "the call goes round again";
        EXPECT_EQ(host_pool_used(), g_base_frames);
        EXPECT_EQ(g_windows, static_cast<int>(DATA_PAGES)) << "freed a granule a window";
    }

    TEST_F(PresyncStage, a_thread_slain_mid_release_frees_the_rest_from_its_exit)
    {
        EXPECT_FALSE(spawn_round(false));
        EXPECT_TRUE(kickos::presync_end());
        g_slay_at = g_windows + 1;
        if (setjmp(g_slain) == 0)
        {
            kickos::presync_release(false);
            FAIL() << "the release opened no window";
        }
        EXPECT_EQ(host_pool_used(), g_base_frames + DATA_PAGES - 1u) << "one granule freed";
        kickos::presync_exit();
        EXPECT_EQ(host_pool_used(), g_base_frames);
    }

    TEST_F(PresyncStage, a_succeeded_call_holding_a_stage_panics_rather_than_open_a_window)
    {
        kickos::presync_begin();
        kickos::presync_stage_image();
        EXPECT_FALSE(kickos::presync_end());
        EXPECT_DEATH(kickos::presync_release(true), "");
    }

    TEST_F(PresyncStage, a_cleared_reservation_is_reserved_or_given_back)
    {
        kickos::presync_begin();
        ASSERT_NE(kickos::aspace_reserve_stage(&ranges_, 3u * G), 0u);
        unsigned char* const first = host_pool_bytes(kickos::presync_record()->staged, 3u);
        memset(first, 0xEE, 3u * G);
        kickos::presync_run();
        EXPECT_EQ(first[0], 0u);
        EXPECT_EQ(first[3u * G - 1u], 0u);
        EXPECT_EQ(kickos::aspace_reserve_commit(nullptr), 0u) << "no range list: kept staged";
        EXPECT_EQ(host_pool_used(), g_base_frames + 3u);
        EXPECT_FALSE(end_round());
        EXPECT_EQ(host_pool_used(), g_base_frames);
        kickos::presync_begin();
        ASSERT_NE(kickos::aspace_reserve_stage(&ranges_, G), 0u);
        kickos::presync_run();
        EXPECT_NE(kickos::aspace_reserve_commit(&ranges_), 0u);
        EXPECT_FALSE(end_round());
        EXPECT_EQ(host_pool_used(), g_base_frames + 1u) << "the reservation holds its frame";
        kickos::aspace_release(SPACE_NEW, &ranges_);
    }

    TEST_F(PresyncStage, a_seed_outside_a_call_copies_its_data_under_the_lock)
    {
        HostPoolMark const before = host_pool_mark();
        ASSERT_TRUE(locked_seed(SPACE_NEW, &ranges_));
        EXPECT_GE(g_locked_copies, 1);
        EXPECT_EQ(g_windows, 0);
        MapCall const* const data = mapped(SPACE_NEW, g_home_data.va);
        ASSERT_NE(data, nullptr);
        EXPECT_TRUE(frames_hold(data->pa, DATA_PAGES, PRISTINE));
        kickos::testfix::host_pool_free_since(before);
    }

    TEST_F(PresyncStage, every_space_maps_the_image_s_text_frames_and_a_data_run_of_its_own)
    {
        HostPoolMark const before = host_pool_mark();
        kickos::VirtualRanges other;
        ASSERT_TRUE(spawn_round());
        EXPECT_FALSE(end_round());
        ASSERT_TRUE(seed_round(SPACE_B, &other));
        EXPECT_FALSE(end_round());
        EXPECT_EQ(g_home_text.pa,
                  kickos::aspace_frame_of(reinterpret_cast<uintptr_t>(g_stage_rom)))
            << "the first space maps the frames the loader put the text in";
        EXPECT_EQ(g_home_data.pa,
                  kickos::aspace_frame_of(reinterpret_cast<uintptr_t>(g_stage_sram)))
            << "the first space borrows the image's own data";
        arch_phys_addr_t data_pa[2] = {};
        struct arch_aspace* const spaces[2] = {SPACE_NEW, SPACE_B};
        for (size_t i = 0; i < 2; i++)
        {
            MapCall const* const text = mapped(spaces[i], g_home_text.va);
            MapCall const* const data = mapped(spaces[i], g_home_data.va);
            ASSERT_NE(text, nullptr) << "space " << i;
            ASSERT_NE(data, nullptr) << "space " << i;
            EXPECT_EQ(text->pa, g_home_text.pa) << "space " << i << ": the text is one frame";
            EXPECT_EQ(text->pages, TEXT_PAGES);
            EXPECT_EQ(text->rights, static_cast<uint32_t>(ARCH_MAP_R | ARCH_MAP_X));
            EXPECT_NE(data->pa, g_home_data.pa) << "space " << i << ": the data is a copy";
            EXPECT_EQ(data->pages, DATA_PAGES);
            EXPECT_EQ(data->rights, static_cast<uint32_t>(ARCH_MAP_R | ARCH_MAP_W));
            EXPECT_TRUE(frames_hold(data->pa, DATA_PAGES, PRISTINE)) << "space " << i;
            data_pa[i] = data->pa;
        }
        EXPECT_NE(data_pa[0], data_pa[1]) << "two spaces share one data run";
        kickos::testfix::host_pool_free_since(before);
    }

#if KICKOS_ARCH_ALIAS_DCACHE
    TEST_F(PresyncStage, every_granule_synced_or_staged_outside_the_lock_opens_one_window)
    {
        constexpr size_t NOTED = 3;
        HostPoolMark const before = host_pool_mark();
        kickos::presync_begin();
        arch_phys_addr_t const run = kickos::testfix::host_pool_take(NOTED);
        ASSERT_NE(run, 0u);
        kickos::presync_note(run, NOTED);
        kickos::presync_stage_image();
        kickos::presync_run();
        EXPECT_EQ(g_syncs, static_cast<int>(NOTED));
        EXPECT_EQ(g_windows, static_cast<int>(NOTED + DATA_PAGES));
        EXPECT_TRUE(locked_seed(SPACE_NEW, &ranges_));
        EXPECT_FALSE(end_round());
        EXPECT_EQ(g_syncs, static_cast<int>(NOTED)) << "the locked pass synced again";
        kickos::testfix::host_pool_free_since(before);
    }
#endif

    TEST_F(PresyncStage, a_space_seeded_after_the_first_is_released_copies_the_image)
    {
        HostPoolMark const before = host_pool_mark();
        kickos::aspace_release(SPACE_A, &g_first);
        EXPECT_EQ(host_pool_used(), g_base_frames) << "the first space's data is not the pool's";
        g_map_count = 0;
        ASSERT_TRUE(spawn_round());
        EXPECT_FALSE(end_round());
        EXPECT_EQ(g_locked_copies, 0) << "a granule was copied under the lock";
        MapCall const* const data = mapped(SPACE_NEW, g_home_data.va);
        ASSERT_NE(data, nullptr);
        EXPECT_NE(data->pa, g_home_data.pa) << "a run of its own, not the image's own data";
        EXPECT_TRUE(frames_hold(data->pa, DATA_PAGES, PRISTINE));
        kickos::testfix::host_pool_free_since(before);
    }
}
