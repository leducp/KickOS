// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include <kickos/aspace.h>
#include <kickos/domain.h>
#include <kickos/instance.h>
#include <kickos/kernel.h>
#include <kickos/sched.h>
#include <kickos/task.h>
#include <kickos/thread.h>
#include <kickos/vrange.h>

#include <kickos/sys/errno.h>

#include "host_aspace.h"
#include "host_frame_pool.h"

#include <gtest/gtest.h>

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace
{
    using kickos::testfix::g_aspaces_live;
    using kickos::testfix::host_pool_used;

    constexpr size_t G = kickos::testfix::HOST_POOL_GRANULE;
    constexpr size_t IMAGE_PAGES = DOMAIN_UNWIND_IMAGE_BYTES / G;
    static_assert(IMAGE_PAGES * G == DOMAIN_UNWIND_IMAGE_BYTES and IMAGE_PAGES != 0u,
                  "the linked image text and data are whole granules");
    constexpr unsigned char IMAGE_DATA_FILL = 0x5Au;
    constexpr size_t GRANT_BYTES = 2u * G;
    constexpr size_t SWEEP_LIMIT = 64u;
    // One root, one table at each of two levels, and the private copy of the image's data.
    constexpr size_t MIN_CREATE_ALLOCS = 4u;

    kickos::Thread* g_current = nullptr;
}

extern "C"
{
    alignas(4096) unsigned char g_image_text[IMAGE_PAGES * G];
    alignas(4096) unsigned char g_image_data[IMAGE_PAGES * G];

    uintptr_t arch_ram_base(void) { return 0; }
    size_t arch_ram_size(void) { return 0; }

    arch_irq_state_t arch_irq_save(void) { return arch_irq_state_t{}; }
    void arch_irq_restore(arch_irq_state_t) {}
    void arch_irq_window(void) {}
    bool arch_irq_inject(int) { return true; }
    uint64_t arch_clock_now(void) { return 0; }

    void* kmemcpy(void* dst, void const* src, size_t n) { return memcpy(dst, src, n); }
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

    Domain* task_domain(Task const*)
    {
        return nullptr;
    }

    // No space here maps a frame capability.
    void frame_run_release_by_slot(int)
    {
        ADD_FAILURE() << "a teardown released a frame run no space here maps";
    }
}

namespace
{
    using kickos::Domain;
    using kickos::DOM_CALLER_TASK;

    // The first seed of the process takes the image's data as its home.
    class DomainSpace : public ::testing::Test
    {
      protected:
        static void SetUpTestSuite()
        {
            if (not s_booted)
            {
                s_booted = true;
                memset(g_image_data, IMAGE_DATA_FILL, sizeof(g_image_data));
                kickos::testfix::host_aspace_tables_from_pool(true);
                kickos::domain_init();
                g_current = &kickos::kernel().threads.slots[0];
            }
            int err = 0;
            s_root = kickos::domain_for(0, nullptr, 0, 0, nullptr, &err);
            ASSERT_NE(s_root, nullptr) << "err " << err;
            kickos::domain_ref(s_root);
        }

        static void TearDownTestSuite()
        {
            kickos::domain_release(s_root);
            s_root = nullptr;
            EXPECT_EQ(g_aspaces_live, 0u) << "a suite left a space alive";
        }

        void SetUp() override
        {
            ASSERT_NE(s_root, nullptr);
            kickos::testfix::host_pool_reset_counters();
            kickos::testfix::host_aspace_reset();
            frames0_ = host_pool_used();
            spaces0_ = g_aspaces_live;
        }

        void TearDown() override
        {
            kickos::testfix::host_pool_fail_in(0);
            EXPECT_EQ(host_pool_used(), frames0_) << "a case leaked frames";
            EXPECT_EQ(g_aspaces_live, spaces0_) << "a case left a space alive";
            EXPECT_EQ(kickos::testfix::host_pool_double_frees(), 0u) << "a frame freed twice";
            EXPECT_EQ(kickos::testfix::host_pool_outside_frees(), 0u)
                << "a teardown freed a frame no pool owns";
            EXPECT_EQ(kickos::testfix::g_aspace_double_destroys, 0u) << "a space destroyed twice";
        }

        static Domain* held_domain()
        {
            int err = 0;
            Domain* const d = kickos::domain_for(0, nullptr, 0, 0, nullptr, &err);
            if (d != nullptr)
            {
                kickos::domain_ref(d);
            }
            return d;
        }

        // The calls kos_ram_alloc makes.
        static uintptr_t reserve_in(Domain* owner, size_t bytes)
        {
            if (kickos::aspace_reserve_stage(kickos::domain_ranges(owner), bytes) == 0)
            {
                return 0;
            }
            kickos::presync_run();
            uintptr_t const va = kickos::aspace_reserve_commit(kickos::domain_ranges_mut(owner));
            kickos::presync_release(va != 0);
            return va;
        }

        static size_t attempts_of_one_create(uint32_t caller, void* mem_base, size_t mem_size,
                                             Domain* donor)
        {
            size_t const before = kickos::testfix::host_pool_attempts();
            int err = 0;
            Domain* const d = kickos::domain_for(caller, mem_base, mem_size, 0, donor, &err);
            size_t const n = kickos::testfix::host_pool_attempts() - before;
            EXPECT_NE(d, nullptr) << "an unrefused create failed, err " << err;
            kickos::domain_release(d);
            return n;
        }

        // Each refusal is checked before the next create could reclaim what it left. Answers
        // the allocations refused.
        size_t sweep(uint32_t caller, void* mem_base, size_t mem_size, Domain* donor)
        {
            size_t depth = 0;
            bool swept = false;
            for (size_t nth = 1; nth <= SWEEP_LIMIT; nth++)
            {
                kickos::testfix::host_pool_fail_in(nth);
                int err = 0;
                Domain* const d = kickos::domain_for(caller, mem_base, mem_size, 0, donor, &err);
                bool const spent = not kickos::testfix::host_pool_fail_armed();
                kickos::testfix::host_pool_fail_in(0);
                if (not spent)
                {
                    swept = true;
                    EXPECT_NE(d, nullptr) << "a create no refusal reached failed, err " << err;
                    kickos::domain_release(d);
                    EXPECT_EQ(host_pool_used(), frames0_) << "the create after the sweep leaked";
                    EXPECT_EQ(g_aspaces_live, spaces0_);
                    break;
                }
                depth++;
                EXPECT_EQ(d, nullptr) << "allocation " << nth
                                      << " was refused and the create carried on past it";
                if (d != nullptr)
                {
                    kickos::domain_release(d);
                    continue;
                }
                EXPECT_EQ(err, KOS_ENOMEM) << "allocation " << nth;
                EXPECT_EQ(host_pool_used(), frames0_)
                    << "allocation " << nth << " was refused and the frame pool did not balance";
                EXPECT_EQ(g_aspaces_live, spaces0_)
                    << "allocation " << nth << " was refused and the live spaces did not balance";
            }
            EXPECT_TRUE(swept) << "the sweep never ran past the last allocation";
            return depth;
        }

        static bool s_booted;
        static Domain* s_root;
        size_t frames0_ = 0;
        size_t spaces0_ = 0;
    };

    bool DomainSpace::s_booted = false;
    Domain* DomainSpace::s_root = nullptr;

    class DomainUnwind : public DomainSpace
    {
    };

    class DomainBalance : public DomainSpace
    {
    };

    class SpacesHeld : public DomainSpace
    {
    };

    TEST_F(DomainUnwind, every_allocation_of_a_create_refused_in_turn_unwinds_whole)
    {
        struct Row
        {
            char const* name;
            uint32_t caller;
        };
        Row const rows[] = {{"a spawn's create", 0}, {"a task create", DOM_CALLER_TASK}};
        for (Row const& row : rows)
        {
            SCOPED_TRACE(row.name);
            size_t const points = attempts_of_one_create(row.caller, nullptr, 0, nullptr);
            EXPECT_GE(points, MIN_CREATE_ALLOCS);
            EXPECT_EQ(sweep(row.caller, nullptr, 0, nullptr), points)
                << "the sweep refused fewer allocations than a create makes";
        }
    }

    // The borrower's unwind unmaps the donor's frames and frees none of them.
    TEST_F(DomainUnwind, every_allocation_of_a_grant_carrying_create_refused_in_turn_unwinds_whole)
    {
        size_t const frames_before = frames0_;
        size_t const spaces_before = spaces0_;
        Domain* const donor = held_domain();
        ASSERT_NE(donor, nullptr);
        uintptr_t const block = reserve_in(donor, GRANT_BYTES);
        ASSERT_NE(block, 0u);
        frames0_ = host_pool_used();
        spaces0_ = g_aspaces_live;
        void* const base = reinterpret_cast<void*>(block);

        size_t const points = attempts_of_one_create(0, base, GRANT_BYTES, donor);
        EXPECT_GT(points, MIN_CREATE_ALLOCS) << "the handoff itself allocated nothing";
        EXPECT_EQ(sweep(0, base, GRANT_BYTES, donor), points);
        EXPECT_EQ(kickos::domain_refcount(donor), 1u)
            << "a refused handoff kept a reference on its donor";

        kickos::domain_release(donor);
        frames0_ = frames_before;
        spaces0_ = spaces_before;
    }

    TEST_F(DomainBalance, unreferenced_creates_land_on_one_slot)
    {
        int err = 0;
        Domain* const first = kickos::domain_for(0, nullptr, 0, 0, nullptr, &err);
        ASSERT_NE(first, nullptr);
        size_t const one = host_pool_used();
        for (int i = 0; i < 3; i++)
        {
            Domain* const again = kickos::domain_for(0, nullptr, 0, 0, nullptr, &err);
            ASSERT_EQ(again, first);
            EXPECT_EQ(host_pool_used(), one) << "a reused slot stranded its previous space";
            EXPECT_EQ(g_aspaces_live, spaces0_ + 1u);
        }
        kickos::domain_release(first);
    }

    TEST_F(DomainBalance, a_reclaimed_slot_answers_no_handle_minted_before)
    {
        Domain* const d = held_domain();
        ASSERT_NE(d, nullptr);
        int const old_handle = kickos::domain_handle(d);
        ASSERT_EQ(kickos::domain_resolve(old_handle), d);
        kickos::domain_release(d);

        Domain* const again = held_domain();
        ASSERT_EQ(again, d) << "fixture: the create took another slot";
        EXPECT_EQ(kickos::domain_resolve(old_handle), nullptr)
            << "a handle minted for the slot's previous occupant names the new one";
        EXPECT_EQ(kickos::domain_resolve(kickos::domain_handle(again)), again);
        kickos::domain_release(again);
    }

    TEST_F(SpacesHeld, the_space_goes_with_the_last_hold_and_not_before)
    {
        Domain* const d = held_domain();
        ASSERT_NE(d, nullptr);
        kickos::domain_ref(d);
        EXPECT_GT(host_pool_used(), frames0_);
        kickos::domain_release(d);
        EXPECT_EQ(g_aspaces_live, spaces0_ + 1u) << "the space went with a hold still standing";
        kickos::domain_release(d);
        EXPECT_EQ(g_aspaces_live, spaces0_) << "the last release kept the space";
        EXPECT_EQ(host_pool_used(), frames0_) << "the last release kept frames";
    }

    TEST_F(SpacesHeld, a_donor_s_space_goes_with_its_last_borrower)
    {
        Domain* const donor = held_domain();
        ASSERT_NE(donor, nullptr);
        uintptr_t const block = reserve_in(donor, GRANT_BYTES);
        ASSERT_NE(block, 0u);
        int err = 0;
        Domain* const borrower =
            kickos::domain_for(0, reinterpret_cast<void*>(block), GRANT_BYTES, 0, donor, &err);
        ASSERT_NE(borrower, nullptr) << "err " << err;
        kickos::domain_ref(borrower);
        EXPECT_EQ(g_aspaces_live, spaces0_ + 2u);

        kickos::domain_release(donor);
        EXPECT_EQ(g_aspaces_live, spaces0_ + 2u)
            << "the donor's space went under a live borrower";
        kickos::domain_release(borrower);
        EXPECT_EQ(g_aspaces_live, spaces0_);
    }
}
