// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The three-valued non-cacheable admission, read from the REAL kernel/grant/grant.cc over
// an arch seam this file sets. REFUSED is the answer no chip in tree gives, so this fixture
// is where that value is driven. And the REAL kernel/domain/domain.cc over the same seam: its
// handle codec and its memory-type refusal; and kernel/thread/thread.cc's ram_region_admit,
// the order and the answers of every path's RAM admission.

#include <kickos/domain.h>
#include <kickos/grant.h>
#include <kickos/instance.h>
#include <kickos/kernel.h>

#include <kickos/sys/errno.h>

#include <gtest/gtest.h>

#include <stddef.h>
#include <stdint.h>

namespace
{
    // The whole fake arena, power-of-two and naturally aligned: geometry always passes, so
    // only the memory type can decide an arm's verdict.
    constexpr uintptr_t ARENA_BASE = 0x20010000u;
    constexpr size_t ARENA_SIZE = 0x10000u;

    int g_nocache = ARCH_MPU_NOCACHE_ALREADY;
    bool g_owned = true;
}

extern "C"
{
    int arch_mpu_nocache_support(void) { return g_nocache; }

    size_t arch_mpu_min_region(void) { return 32u; }
    int arch_mpu_region_pow2(void) { return 1; }

    bool arch_mpu_region_encodable(uintptr_t base, size_t size)
    {
        if (size < 32u or (size & (size - 1u)) != 0u)
        {
            return false;
        }
        return (base & (size - 1u)) == 0u;
    }

    uintptr_t arch_ram_base(void) { return ARENA_BASE; }
    size_t arch_ram_size(void) { return ARENA_SIZE; }

    // Zero blocks and no bit-band alias, so geometry cannot decide an arm's verdict.
    struct arch_reserved_span arch_reserved_blocks(void) { return {}; }

    // The i.MX RT1062's USB OTG1 core, the region chip's one bus master.
    constexpr struct arch_reserved_block BUS_MASTERS[] = {{0x402E0000u, 0x200u}};
    struct arch_reserved_span arch_bus_master_apertures(void) { return {BUS_MASTERS}; }
    int arch_bitband_present(void) { return 0; }
}

namespace
{
    constexpr uint32_t RW = ARCH_MPU_R | ARCH_MPU_W;
    constexpr uint32_t RW_NC = RW | ARCH_MPU_NOCACHE;

    class NocacheAdmission : public ::testing::Test
    {
    protected:
        void TearDown() override { g_nocache = ARCH_MPU_NOCACHE_ALREADY; }
    };

    // THE POSITIVE CONTROL, and it may not be removed.
    TEST_F(NocacheAdmission, a_chip_that_programs_the_attribute_admits_the_grant)
    {
        g_nocache = ARCH_MPU_NOCACHE_PROGRAMMED;
        EXPECT_TRUE(kickos::grant_region_admissible(ARENA_BASE, 4096u, RW_NC, false));
    }

    TEST_F(NocacheAdmission, a_chip_with_no_cache_in_the_path_admits_the_grant)
    {
        g_nocache = ARCH_MPU_NOCACHE_ALREADY;
        EXPECT_TRUE(kickos::grant_region_admissible(ARENA_BASE, 4096u, RW_NC, false));
    }

    // THE ARM NO BOARD REACHES, and it must refuse HERE: every commit backend drops a
    // region it cannot encode in silence, so no later point could report it.
    TEST_F(NocacheAdmission, a_chip_that_cannot_express_the_attribute_refuses_the_grant)
    {
        g_nocache = ARCH_MPU_NOCACHE_REFUSED;
        EXPECT_FALSE(kickos::grant_region_admissible(ARENA_BASE, 4096u, RW_NC, false));
    }

    // The refusal is keyed on the REQUEST, not on the chip.
    TEST_F(NocacheAdmission, a_refusing_chip_still_admits_an_ordinary_grant)
    {
        g_nocache = ARCH_MPU_NOCACHE_REFUSED;
        EXPECT_TRUE(kickos::grant_region_admissible(ARENA_BASE, 4096u, RW, false));
    }

    TEST_F(NocacheAdmission, the_memory_type_never_widens_what_geometry_refuses)
    {
        g_nocache = ARCH_MPU_NOCACHE_PROGRAMMED;
        EXPECT_FALSE(kickos::grant_region_admissible(ARENA_BASE + 1u, 4096u, RW_NC, false));
        EXPECT_FALSE(kickos::grant_region_admissible(0x40000000u, 4096u, RW_NC, false));
        EXPECT_FALSE(kickos::grant_region_admissible(ARENA_BASE, 0u, RW_NC, false));
    }

    // A region backend asks the chip's bus-master rows, as a translating one does.
    TEST(BusMasterWindow, a_window_meeting_a_bus_master_row_is_one)
    {
        EXPECT_TRUE(kickos::grant_window_bus_master(0x402E0000u, 0x200u));
        EXPECT_TRUE(kickos::grant_window_bus_master(0x402E0100u, 0x20u));
        EXPECT_TRUE(kickos::grant_window_bus_master(0x402DFF00u, 0x200u));
        EXPECT_FALSE(kickos::grant_window_bus_master(0x402E0200u, 0x600u));
        EXPECT_FALSE(kickos::grant_window_bus_master(0x402DFE00u, 0x200u));
    }
}

namespace kickos
{
    bool ram_owner_nameable(Task const*, uintptr_t, size_t)
    {
        return g_owned;
    }

    namespace
    {
        constexpr int SLOT = KICKOS_MAX_DOMAINS - 1;

        Domain* live_slot(uint16_t generation)
        {
            Domain* const d = &kernel().domains[SLOT];
            *d = Domain{};
            d->generation = generation;
            d->refcount = 1;
            return d;
        }

        void* const ARENA_RAM = reinterpret_cast<void*>(ARENA_BASE);
        constexpr uintptr_t BLOCK = ARENA_BASE + 0x1000u;
        constexpr size_t BLOCK_SIZE = 0x100u;
        constexpr uintptr_t UNALIGNED = BLOCK + 0x20u;

        class DomainAdmit : public ::testing::Test
        {
        protected:
            void TearDown() override
            {
                g_nocache = ARCH_MPU_NOCACHE_ALREADY;
                g_owned = true;
                domain_release(held_);
                held_ = nullptr;
            }

            int admit(uintptr_t base, uint32_t attr, RamAdmit how = RAM_ADMIT_GRANT)
            {
                return ram_region_admit(&granter_, base, BLOCK_SIZE, attr, how);
            }

            // A task's data region over BLOCK, committed non-cacheable.
            void hold_uncached()
            {
                g_nocache = ARCH_MPU_NOCACHE_PROGRAMMED;
                int err = 0;
                held_ = domain_for(0u, reinterpret_cast<void*>(BLOCK), BLOCK_SIZE,
                                   ARCH_MPU_NOCACHE, nullptr, &err);
                ASSERT_NE(held_, nullptr);
                domain_ref(held_);
            }

            Thread granter_{};
            Domain* held_ = nullptr;
        };
    }

    // A domain slot claimed 32768 times mints a handle with bit 31 set. It must still resolve,
    // or every capability naming that slot answers -KOS_EBADF until the generation wraps.
    TEST(DomainHandle, an_aged_handle_is_negative_and_resolves)
    {
        Domain* const d = live_slot(0x8000u);
        int const handle = domain_handle(d);

        EXPECT_LT(handle, 0) << "generation 0x8000 sets bit 31";
        EXPECT_EQ(domain_resolve(handle), d);
    }

    TEST(DomainHandle, the_last_generation_before_the_wrap_resolves)
    {
        Domain* const d = live_slot(0xFFFFu);
        EXPECT_EQ(domain_resolve(domain_handle(d)), d);
    }

    TEST(DomainHandle, a_stale_aged_handle_is_refused)
    {
        Domain* const d = live_slot(0x8000u);
        int const handle = domain_handle(d);
        d->generation++;

        EXPECT_EQ(domain_resolve(handle), nullptr);
    }

    TEST(DomainHandle, the_null_domains_handle_is_refused)
    {
        EXPECT_EQ(domain_handle(nullptr), -1);
        EXPECT_EQ(domain_resolve(-1), nullptr);
    }

    // The ABI's answer to a memory type the chip cannot honour is -KOS_ENOTSUP, on a region
    // backend as on a translating one.
    TEST_F(DomainAdmit, an_unhonoured_memory_type_answers_enotsup)
    {
        g_nocache = ARCH_MPU_NOCACHE_REFUSED;
        int err = 0;

        EXPECT_EQ(domain_for(0u, ARENA_RAM, 64u, ARCH_MPU_NOCACHE, nullptr, &err), nullptr);
        EXPECT_EQ(err, KOS_ENOTSUP);
    }

    TEST_F(DomainAdmit, an_honoured_memory_type_is_admitted)
    {
        g_nocache = ARCH_MPU_NOCACHE_PROGRAMMED;
        int err = 0;

        EXPECT_NE(domain_for(0u, ARENA_RAM, 64u, ARCH_MPU_NOCACHE, nullptr, &err), nullptr);
        EXPECT_EQ(err, 0);
    }

    TEST_F(DomainAdmit, a_block_the_granter_reserved_is_admitted)
    {
        EXPECT_EQ(admit(BLOCK, RW), 0);
    }

    TEST_F(DomainAdmit, a_region_no_one_descriptor_names_answers_einval)
    {
        EXPECT_EQ(admit(UNALIGNED, RW), -KOS_EINVAL);
        EXPECT_EQ(admit(0x40000001u, RW), -KOS_EINVAL);
    }

    TEST_F(DomainAdmit, the_shape_is_asked_before_the_owner)
    {
        g_owned = false;
        EXPECT_EQ(admit(UNALIGNED, RW), -KOS_EINVAL);
    }

    TEST_F(DomainAdmit, the_memory_type_is_asked_before_the_shape)
    {
        g_nocache = ARCH_MPU_NOCACHE_REFUSED;
        EXPECT_EQ(admit(UNALIGNED, RW_NC), -KOS_ENOTSUP);
    }

    TEST_F(DomainAdmit, the_memory_type_is_asked_before_the_owner)
    {
        g_nocache = ARCH_MPU_NOCACHE_REFUSED;
        g_owned = false;
        EXPECT_EQ(admit(BLOCK, RW_NC), -KOS_ENOTSUP);
    }

    TEST_F(DomainAdmit, a_region_out_of_the_arena_answers_eperm)
    {
        EXPECT_EQ(admit(ARENA_BASE + ARENA_SIZE, RW), -KOS_EPERM);
    }

    TEST_F(DomainAdmit, a_block_the_granter_never_reserved_answers_eperm)
    {
        g_owned = false;
        EXPECT_EQ(admit(BLOCK, RW), -KOS_EPERM);
    }

    TEST_F(DomainAdmit, a_privileged_child_s_window_waives_the_arena_and_the_owner)
    {
        g_owned = false;
        EXPECT_EQ(admit(ARENA_BASE + ARENA_SIZE, RW, RAM_ADMIT_PRIVILEGED), 0);
    }

    TEST_F(DomainAdmit, a_block_held_with_another_type_answers_ebusy)
    {
        hold_uncached();
        EXPECT_EQ(admit(BLOCK, RW), -KOS_EBUSY);
        EXPECT_EQ(admit(BLOCK, RW_NC), 0);
    }
}
