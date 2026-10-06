// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A run a system call stages outside every range, over the REAL kernel/mem/aspace.cc: a
// reservation's clear and a new space's copy of the image's static data, filled outside the
// lock, taken only by a locked pass that asks for exactly what was staged, and otherwise given
// back granule by granule when the round ends, or whole when the thread leaves. The frame pool is
// a host array whose every frame is accounted for, so a leak or a double free fails the case.

#include <kickos/aspace.h>
#include <kickos/domain.h>
#include <kickos/frame_pool.h>
#include <kickos/instance.h>
#include <kickos/kernel.h>
#include <kickos/sched.h>
#include <kickos/vrange.h>

#include <gtest/gtest.h>

#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if not KICKOS_PRESYNC
#error "this gate's posture is a translating backend working outside the lock"
#endif

namespace
{
    constexpr size_t G = 4096u;
    constexpr size_t FRAMES = 64u;
    constexpr arch_phys_addr_t POOL_LO = 0x40000000u;
    constexpr size_t DATA_PAGES = PRESYNC_STAGE_DATA_BYTES / G;
    static_assert(DATA_PAGES * G == PRESYNC_STAGE_DATA_BYTES and DATA_PAGES >= 2u,
                  "the linked image data is whole granules, at least two");

    alignas(G) unsigned char g_pool[FRAMES * G];
    bool g_used[FRAMES] = {};
    size_t g_fail_in = 0;
    int g_windows = 0;
    // The window at which the calling thread is slain, abandoning its call as a redirect does.
    int g_slay_at = 0;
    jmp_buf g_slain;
    // The window at which the calling thread's space is released.
    int g_release_at = 0;

    kickos::Thread* g_current = nullptr;
    // The calling thread's own space.
    struct arch_aspace* g_own_space = nullptr;

    struct arch_aspace* const SPACE_A = reinterpret_cast<struct arch_aspace*>(0x1000);
    struct arch_aspace* const SPACE_B = reinterpret_cast<struct arch_aspace*>(0x2000);
    struct arch_aspace* const SPACE_NEW = reinterpret_cast<struct arch_aspace*>(0x3000);

    size_t frame_index(arch_phys_addr_t pa)
    {
        return static_cast<size_t>((pa - POOL_LO) / G);
    }

    size_t frames_used()
    {
        size_t n = 0;
        for (bool const u : g_used)
        {
            if (u)
            {
                n++;
            }
        }
        return n;
    }
}

extern "C"
{
    // The app's static data, which the linker options place this array's bytes under.
    alignas(4096) unsigned char g_stage_sram[DATA_PAGES * G];

    size_t arch_aspace_granule(void) { return G; }
    uintptr_t arch_aspace_user_offset(void) { return 0; }
    arch_phys_addr_t arch_aspace_frame_at(struct arch_aspace*, uintptr_t) { return 0; }

    enum arch_aspace_result arch_aspace_map(struct arch_aspace*, uintptr_t, arch_phys_addr_t,
                                            size_t, uint32_t, enum arch_map_memtype)
    {
        return ARCH_ASPACE_OK;
    }

    enum arch_aspace_result arch_aspace_unmap(struct arch_aspace*, uintptr_t, size_t)
    {
        return ARCH_ASPACE_OK;
    }

    void* arch_aspace_acquire(struct arch_aspace*, uintptr_t va, bool*)
    {
        return reinterpret_cast<void*>(va);
    }

    void arch_aspace_release(struct arch_aspace*, uintptr_t) {}

    arch_irq_state_t arch_irq_save(void) { return arch_irq_state_t{}; }
    void arch_irq_restore(arch_irq_state_t) {}
    void arch_irq_window(void)
    {
        g_windows++;
        if (g_windows == g_release_at)
        {
            g_own_space = nullptr;
        }
        if (g_windows == g_slay_at)
        {
            longjmp(g_slain, 1);
        }
    }

    void arch_aspace_destroy(struct arch_aspace*) {}
    void arch_aspace_activate(struct arch_aspace*) {}
    struct arch_aspace* arch_aspace_boot(void) { return nullptr; }
    uint64_t arch_clock_now(void) { return 0; }
    void arch_irq_inject(int) {}

    void* kmemcpy(void* dst, void const* src, size_t n) { return memcpy(dst, src, n); }
    void* kmemset(void* dst, int c, size_t n) { return memset(dst, c, n); }

    void kickos_frame_free(arch_phys_addr_t frame)
    {
        size_t const i = frame_index(frame);
        if (i >= FRAMES or not g_used[i])
        {
            ADD_FAILURE() << "frame freed twice or never allocated";
            return;
        }
        g_used[i] = false;
    }
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

    void alias_sync(void const*, size_t) {}
    void frame_run_release_by_slot(int) {}

    struct arch_aspace* domain_space(Domain const*) { return g_own_space; }
    VirtualRanges const* domain_ranges(Domain const*) { return nullptr; }
    VirtualRanges* domain_ranges_mut(Domain*) { return nullptr; }
    Domain* task_domain(Task const*) { return nullptr; }

    void frame_pool_phys_bounds(arch_phys_addr_t* lo, arch_phys_addr_t* hi)
    {
        *lo = POOL_LO;
        *hi = POOL_LO + FRAMES * G;
    }

    arch_phys_addr_t frame_pool_alloc_run(size_t pages)
    {
        if (g_fail_in != 0 and --g_fail_in == 0)
        {
            return 0;
        }
        for (size_t i = 0; i + pages <= FRAMES; i++)
        {
            bool free = true;
            for (size_t j = 0; free and j < pages; j++)
            {
                free = not g_used[i + j];
            }
            if (free)
            {
                for (size_t j = 0; j < pages; j++)
                {
                    g_used[i + j] = true;
                }
                return POOL_LO + static_cast<arch_phys_addr_t>(i * G);
            }
        }
        return 0;
    }

    void frame_pool_free_run(arch_phys_addr_t run, size_t pages, size_t granule)
    {
        for (size_t i = 0; i < pages; i++)
        {
            kickos_frame_free(run + static_cast<arch_phys_addr_t>(i * granule));
        }
    }

    void* frame_pool_ptr(arch_phys_addr_t frame)
    {
        size_t const i = frame_index(frame);
        if (frame < POOL_LO or i >= FRAMES or not g_used[i])
        {
            return nullptr;
        }
        return &g_pool[i * G];
    }
}

namespace
{
    using kickos::Thread;

    // The first space borrows the image's data and makes SPACE_A its home; what every later
    // case stages is copied from it.
    class PresyncStage : public ::testing::Test
    {
      protected:
        static void SetUpTestSuite()
        {
            memset(g_stage_sram, 0x5A, sizeof(g_stage_sram));
            static kickos::VirtualRanges first;
            ASSERT_TRUE(kickos::aspace_image_seed(SPACE_A, &first, false, nullptr));
            g_base_frames = frames_used();
        }

        void SetUp() override
        {
            g_current = a();
            kickos::presync_exit();
            g_fail_in = 0;
            g_windows = 0;
            g_slay_at = 0;
            g_release_at = 0;
            g_own_space = nullptr;
            ASSERT_TRUE(ranges_.init(G));
            ASSERT_EQ(frames_used(), g_base_frames);
        }

        void TearDown() override
        {
            b()->privileged = false;
            g_current = b();
            kickos::presync_exit();
            g_current = a();
            kickos::presync_exit();
            EXPECT_EQ(frames_used(), g_base_frames) << "a staged run outlived its call";
        }

        static Thread* a() { return &kickos::kernel().threads.slots[0]; }
        static Thread* b() { return &kickos::kernel().threads.slots[1]; }

        static uint32_t staged_metric() { return kickos::presync_granules() >> 32; }

        // One spawn's round: stage the copy from `home`, fill it outside the lock, then the
        // locked pass seeds a new space as from `spawner`; true when that pass took the stage.
        bool spawn_round(struct arch_aspace* home, struct arch_aspace* spawner)
        {
            kickos::presync_begin();
            kickos::presync_stage_image(false, home);
            kickos::presync_run();
            return kickos::aspace_image_seed(SPACE_NEW, &ranges_, false, spawner);
        }

        // The round's end, the lock released: true when the call goes round again.
        bool end_round()
        {
            bool const again = kickos::presync_end();
            kickos::presync_release(false);
            return again;
        }

        static size_t g_base_frames;
        kickos::VirtualRanges ranges_;
    };

    size_t PresyncStage::g_base_frames = 0;

    TEST_F(PresyncStage, a_stage_from_the_spawner_s_home_is_taken_and_holds_its_bytes)
    {
        bool before[FRAMES];
        memcpy(before, g_used, sizeof(before));
        ASSERT_TRUE(spawn_round(SPACE_A, SPACE_A));
        EXPECT_FALSE(end_round());
        EXPECT_EQ(frames_used(), g_base_frames + DATA_PAGES) << "the new space holds its copy";
        EXPECT_EQ(g_windows, static_cast<int>(DATA_PAGES));
        size_t copied = 0;
        for (size_t i = 0; i < FRAMES; i++)
        {
            if (g_used[i] and not before[i])
            {
                for (size_t b = 0; b < G; b++)
                {
                    if (g_pool[i * G + b] != 0x5Au)
                    {
                        ADD_FAILURE() << "frame " << i << " byte " << b;
                        break;
                    }
                }
                copied++;
                // The space's mapped frames, which its table walk would free.
                kickos_frame_free(POOL_LO + static_cast<arch_phys_addr_t>(i * G));
            }
        }
        EXPECT_EQ(copied, DATA_PAGES);
    }

    TEST_F(PresyncStage, a_full_stage_from_another_home_is_refused_and_given_back)
    {
        g_own_space = SPACE_B;
        uint32_t const r0 = kickos::presync_refusals();
        EXPECT_FALSE(spawn_round(SPACE_B, SPACE_A));
        EXPECT_TRUE(kickos::presync_record()->staged_full);
        EXPECT_EQ(kickos::presync_refusals(), r0 + 1u);
        EXPECT_EQ(frames_used(), g_base_frames + DATA_PAGES) << "still staged until the end";
        int const w0 = g_windows;
        uint32_t const s0 = staged_metric();
        EXPECT_TRUE(end_round()) << "the call goes round again";
        EXPECT_EQ(frames_used(), g_base_frames);
        EXPECT_EQ(g_windows - w0, static_cast<int>(DATA_PAGES)) << "freed a granule a window";
        EXPECT_EQ(staged_metric() - s0, DATA_PAGES) << "the release is in the metric";
    }

    TEST_F(PresyncStage, a_home_released_mid_stage_leaves_it_short_and_refused)
    {
        g_own_space = SPACE_B;
        g_release_at = 1;
        uint32_t const r0 = kickos::presync_refusals();
        EXPECT_FALSE(spawn_round(SPACE_B, SPACE_B));
        EXPECT_FALSE(kickos::presync_record()->staged_full);
        EXPECT_EQ(kickos::presync_refusals(), r0 + 1u);
        EXPECT_TRUE(end_round()) << "the call goes round again";
        EXPECT_EQ(frames_used(), g_base_frames);
    }

    TEST_F(PresyncStage, a_thread_slain_mid_release_frees_the_rest_from_its_exit)
    {
        g_own_space = SPACE_B;
        EXPECT_FALSE(spawn_round(SPACE_B, SPACE_A));
        EXPECT_TRUE(kickos::presync_end());
        g_slay_at = g_windows + 1;
        if (setjmp(g_slain) == 0)
        {
            kickos::presync_release(false);
            FAIL() << "the release opened no window";
        }
        EXPECT_EQ(frames_used(), g_base_frames + DATA_PAGES - 1u) << "one granule freed";
        kickos::presync_exit();
        EXPECT_EQ(frames_used(), g_base_frames);
    }

    TEST_F(PresyncStage, a_privileged_caller_stages_and_releases_without_a_window)
    {
        b()->privileged = true;
        g_current = b();
        g_own_space = SPACE_B;
        EXPECT_FALSE(spawn_round(SPACE_B, SPACE_A));
        EXPECT_TRUE(kickos::presync_record()->staged_full);
        EXPECT_TRUE(end_round());
        EXPECT_EQ(frames_used(), g_base_frames);
        EXPECT_EQ(g_windows, 0);
    }

    TEST_F(PresyncStage, a_succeeded_call_holding_a_stage_panics_rather_than_open_a_window)
    {
        kickos::presync_begin();
        kickos::presync_stage_image(false, SPACE_A);
        EXPECT_FALSE(kickos::presync_end());
        EXPECT_DEATH(kickos::presync_release(true), "");
    }

    TEST_F(PresyncStage, a_pool_with_no_run_for_the_stage_fails_the_call_without_a_round)
    {
        uint32_t const r0 = kickos::presync_refusals();
        g_fail_in = 1;
        EXPECT_FALSE(spawn_round(SPACE_A, SPACE_A));
        EXPECT_FALSE(end_round()) << "a resource refusal is not a round";
        EXPECT_EQ(kickos::presync_refusals(), r0);
    }

    TEST_F(PresyncStage, a_thread_leaving_mid_call_gives_back_its_stage)
    {
        kickos::presync_begin();
        kickos::presync_stage_image(false, SPACE_A);
        EXPECT_EQ(frames_used(), g_base_frames + DATA_PAGES);
        kickos::presync_exit();
        EXPECT_EQ(frames_used(), g_base_frames);
    }

    TEST_F(PresyncStage, a_cleared_reservation_is_reserved_or_given_back)
    {
        kickos::presync_begin();
        ASSERT_NE(kickos::aspace_reserve_stage(&ranges_, 3u * G), 0u);
        unsigned char* const first = g_pool + frame_index(kickos::presync_record()->staged) * G;
        memset(first, 0xEE, 3u * G);
        kickos::presync_run();
        EXPECT_EQ(first[0], 0u);
        EXPECT_EQ(first[3u * G - 1u], 0u);
        EXPECT_EQ(kickos::aspace_reserve_commit(nullptr), 0u) << "no range list: kept staged";
        EXPECT_EQ(frames_used(), g_base_frames + 3u);
        EXPECT_FALSE(end_round());
        EXPECT_EQ(frames_used(), g_base_frames);
        kickos::presync_begin();
        ASSERT_NE(kickos::aspace_reserve_stage(&ranges_, G), 0u);
        kickos::presync_run();
        EXPECT_NE(kickos::aspace_reserve_commit(&ranges_), 0u);
        EXPECT_FALSE(end_round());
        EXPECT_EQ(frames_used(), g_base_frames + 1u) << "the reservation holds its frame";
        kickos::aspace_release(SPACE_NEW, &ranges_);
    }
}
