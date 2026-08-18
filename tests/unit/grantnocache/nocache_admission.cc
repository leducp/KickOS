// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The three-valued non-cacheable admission (docs/design-m4.6.2-usb-cdc.md, S7), read from
// the REAL kernel/grant/grant.cc over an arch seam this file sets. No chip in tree answers
// REFUSED, so that value is reachable only here.

#include <kickos/grant.h>

#include <gtest/gtest.h>

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

namespace
{
    // The whole fake arena, power-of-two and naturally aligned: geometry always passes, so
    // only the memory type can decide an arm's verdict.
    constexpr uintptr_t ARENA_BASE = 0x20010000u;
    constexpr size_t ARENA_SIZE = 0x10000u;

    int g_nocache = ARCH_MPU_NOCACHE_ALREADY;
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

    // No reserved block and no bit-band alias: either would decide arms on geometry.
    size_t arch_reserved_blocks(struct arch_reserved_block*, size_t) { return 0; }
    int arch_bitband_present(void) { return 0; }
}

extern "C" size_t arch_domain_static_regions(struct arch_mpu_region*, size_t)
{
    return 0;
}

namespace kickos
{
    // grant_reserved_validate's KICKOS_ASSERT lands here; no arm should trip one.
    void kpanic(char const* msg)
    {
        ADD_FAILURE() << "kernel panic: " << msg;
        abort();
    }
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
}
