// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The program's span at the fixed user base must lie in top-level slots the adopted firmware
// root leaves empty, or in empty entries of firmware's table under the one slot it maps there,
// which every space then holds a copy of; anything else refuses the boot.

#include <kickos/arch/arch.h>
#include <kickos/arch/aspace.h>
#include <kickos/arch/program_base.h>

#include <gtest/gtest.h>

#include "x86_64_sysops_seam.h"

using namespace kickos::testfix;

namespace
{
    constexpr uintptr_t BASE = KICKOS_X86_64_PROGRAM_BASE;
    // Inside the firmware's identity map, outside the frame pool; never dereferenced.
    constexpr arch_phys_addr_t PA = 0x20000000;
    constexpr uint64_t PTE_US = 1u << 2;
    constexpr uint64_t PTE_ADDR = 0x000ffffffffff000ull;

    // Whether a page maps at the base and resolves to its frame.
    bool maps_the_base(struct arch_aspace* space)
    {
        bool const mapped = arch_aspace_map(space, BASE, PA, 1, ARCH_MAP_R | ARCH_MAP_W,
                                            ARCH_MAP_NORMAL) == ARCH_ASPACE_OK;
        return mapped and arch_aspace_frame_at(space, BASE) == PA;
    }
}

TEST(ProgramSpan, AFourLevelRootLeavingTheProgramSlotEmptyIsAdopted)
{
    sysops_reset();
    adopt();
    struct arch_aspace* const space = arch_aspace_create();
    ASSERT_NE(space, nullptr);
    EXPECT_TRUE(maps_the_base(space));
    arch_aspace_destroy(space);
}

TEST(ProgramSpan, ARootMappingTheProgramSlotRefusesTheBoot)
{
    sysops_reset();
    firmware_identity_slot(BASE);
    EXPECT_DEATH(adopt(), "the program base lies outside the top-level slots firmware left empty");
    sysops_reset();
}

TEST(ProgramSpan, AFiveLevelRootMapsTheProgramThroughACopyOfFirmwaresTable)
{
    sysops_reset();
    firmware_user_chain(2, 5, true);
    adopt();
    uint64_t const* const boot = reinterpret_cast<uint64_t const*>(arch_aspace_boot());
    uint64_t const* const firmware = reinterpret_cast<uint64_t const*>(boot[0] & PTE_ADDR);
    uint32_t const allocated = frames_allocated();
    uint32_t const freed = frames_freed();
    struct arch_aspace* const space = arch_aspace_create();
    ASSERT_NE(space, nullptr);
    uint64_t const* const root = reinterpret_cast<uint64_t const*>(space);
    uint64_t const* const copy = reinterpret_cast<uint64_t const*>(root[0] & PTE_ADDR);
    EXPECT_NE(copy, firmware);
    EXPECT_NE(root[0] & PTE_US, 0u);
    for (size_t i = 0; i < 512; i++)
    {
        EXPECT_EQ(copy[i], firmware[i] & ~PTE_US) << "entry " << i;
    }
    EXPECT_TRUE(maps_the_base(space));
    EXPECT_EQ(boot[0] & PTE_ADDR, reinterpret_cast<uintptr_t>(firmware));
    // Unmapped first, so the balance below counts the tables and the copy alone.
    EXPECT_EQ(arch_aspace_unmap(space, BASE, 1), ARCH_ASPACE_OK);
    arch_aspace_destroy(space);
    EXPECT_EQ(frames_allocated() - allocated, frames_freed() - freed);
    sysops_reset();
}

TEST(ProgramSpan, AFiveLevelRootWhoseTableMapsTheProgramRefusesTheBoot)
{
    sysops_reset();
    firmware_user_chain(2, 5, true);
    firmware_identity_slot(BASE);
    EXPECT_DEATH(adopt(), "the program base lies outside the top-level slots firmware left empty");
    sysops_reset();
}

// The copy of firmware's table and the tables a space builds under it are the space's own, and
// a map there builds what it predicts.
TEST(ProgramSpan, ASpaceCountsTheCopyAndTheTablesUnderIt)
{
    sysops_reset();
    firmware_user_chain(2, 5, true);
    adopt();
    uint32_t const before = frames_allocated();
    struct arch_aspace* const space = arch_aspace_create();
    ASSERT_NE(space, nullptr);
    EXPECT_EQ(arch_aspace_create_pages(), 2u);
    EXPECT_EQ(frames_allocated() - before, arch_aspace_create_pages());
    EXPECT_EQ(arch_aspace_table_pages(space), arch_aspace_create_pages());
    size_t const want = arch_aspace_map_tables(space, BASE, 1);
    EXPECT_GT(want, 0u);
    uint32_t const at = frames_allocated();
    EXPECT_TRUE(maps_the_base(space));
    EXPECT_EQ(frames_allocated() - at, want);
    EXPECT_EQ(arch_aspace_table_pages(space), arch_aspace_create_pages() + want);
    EXPECT_EQ(arch_aspace_unmap(space, BASE, 1), ARCH_ASPACE_OK);
    arch_aspace_destroy(space);
    sysops_reset();
}
