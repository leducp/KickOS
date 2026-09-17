// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Check ARM64 page-table publication on one core. The MMU walker requires
// completed stores even on the issuing PE (DDI 0487 M.b, D8.17.1).
// Seed intermediate tables first so allocation barriers cannot hide a
// missing leaf-publication barrier.

#include <kickos/arch/arch.h>

#include <gtest/gtest.h>

#include "aspace_sysops_seam.h"

using namespace kickos::testfix;

namespace
{
    // Require the single-core paths rather than the SMP publication path.
    static_assert(KICKOS_KERNEL_CORES == 1,
                  "map_publish must compile the single-core arm of publish_edits and "
                  "invalidate_page");

    // Levels 1 through 3 require two child tables per initial leaf.
    constexpr uintptr_t VA_SEED = 0x40201000;  // level-1 index 1, level-2 index 1, level-3 index 1
    constexpr uintptr_t VA_DEEP = 0x40203000; // the same level-3 table, a different slot

    constexpr arch_phys_addr_t PA_A = 0x20000000;
    constexpr arch_phys_addr_t PA_B = 0x20001000;
    constexpr arch_phys_addr_t PA_C = 0x20002000;

    constexpr uint32_t RIGHTS_DATA = ARCH_MAP_R | ARCH_MAP_W;

    // The high page requires another leaf table, allowing allocation failure.
    constexpr uintptr_t VA_CROSS_LOW = 0x401FF000;  // level-2 index 0, level-3 index 511
    constexpr uintptr_t VA_CROSS_BELOW = 0x401FE000; // the low page's table, a slot below it

    // Distinguish no register write from ASID 0.
    constexpr uint32_t NO_WRITE = 0xFFFFFFFFu;

    uint32_t last_identifier()
    {
        int const at = last_index_of(OP_WRITE_TTBR0);
        if (at < 0)
        {
            return NO_WRITE;
        }
        return static_cast<uint32_t>(op_at(static_cast<size_t>(at)).arg >> 48);
    }

    class MapPublish : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            sysops_reset();
            running = arch_aspace_create();
            ASSERT_NE(running, nullptr);
            fresh = arch_aspace_create();
            ASSERT_NE(fresh, nullptr);
            // A tagged predecessor avoids publication through a switch-time flush.
            arch_aspace_activate(running);
            ASSERT_NE(last_identifier(), NO_WRITE);
            ASSERT_NE(last_identifier(), 0u);
            ops_clear();
        }

        // Release residency rows; sysops_reset does not clear backend state.
        void TearDown() override
        {
            arch_aspace_destroy(running);
            arch_aspace_destroy(fresh);
            running = nullptr;
            fresh = nullptr;
        }

        // Build intermediate tables and clear the operation trace.
        void seed(struct arch_aspace* space, uintptr_t va, arch_phys_addr_t pa)
        {
            ASSERT_EQ(arch_aspace_map(space, va, pa, 1, RIGHTS_DATA, ARCH_MAP_NORMAL),
                      ARCH_ASPACE_OK);
            ops_clear();
        }

        struct arch_aspace* running = nullptr;
        struct arch_aspace* fresh = nullptr;
    };
}

// Publication without invalidation.

TEST_F(MapPublish, MappingIntoANeverRunSpacePublishesItsDescriptors)
{
    seed(fresh, VA_SEED, PA_A);
    uint32_t const before = frames_allocated();

    ASSERT_EQ(arch_aspace_map(fresh, VA_DEEP, PA_B, 1, RIGHTS_DATA, ARCH_MAP_NORMAL),
              ARCH_ASPACE_OK);

    // Exclude allocation and invalidation barriers from this measurement.
    EXPECT_EQ(frames_allocated(), before);
    EXPECT_EQ(ops_with(OP_TLBI_PAGE_LOCAL), 0u);
    EXPECT_EQ(ops_with(OP_TLBI_PAGE_IS), 0u);
    EXPECT_EQ(ops_with(OP_TLBI_ALL_LOCAL), 0u);

    EXPECT_EQ(ops_with(OP_DSB_ISHST), 1u) << "the leaf store was left for the walker to miss";
    EXPECT_EQ(arch_aspace_frame_at(fresh, VA_DEEP), PA_B);
}

TEST_F(MapPublish, ThePublicationStandsBeforeTheRootIsInstalled)
{
    seed(fresh, VA_SEED, PA_A);

    ASSERT_EQ(arch_aspace_map(fresh, VA_DEEP, PA_B, 1, RIGHTS_DATA, ARCH_MAP_NORMAL),
              ARCH_ASPACE_OK);
    arch_aspace_activate(fresh);

    // Verify that the switch itself supplies no invalidation barrier.
    ASSERT_NE(last_identifier(), 0u);
    EXPECT_EQ(ops_with(OP_TLBI_ALL_LOCAL), 0u);
    EXPECT_EQ(ops_with(OP_DSB_ISH), 0u);

    int const published = last_index_of(OP_DSB_ISHST);
    int const installed = last_index_of(OP_WRITE_TTBR0);
    ASSERT_GE(installed, 0);
    ASSERT_GE(published, 0) << "no barrier at all stands between the leaf store and the walk";
    EXPECT_LT(published, installed);
}

TEST_F(MapPublish, UnmappingFromANeverRunSpacePublishesItsClears)
{
    seed(fresh, VA_SEED, PA_A);

    ASSERT_EQ(arch_aspace_unmap(fresh, VA_SEED, 1), ARCH_ASPACE_OK);

    EXPECT_EQ(ops_with(OP_TLBI_PAGE_LOCAL), 0u);
    EXPECT_EQ(ops_with(OP_DSB_ISHST), 1u) << "the cleared leaf was left for the walker to miss";
    EXPECT_EQ(arch_aspace_frame_at(fresh, VA_SEED), 0u);
}

// Resident-space maintenance.

TEST_F(MapPublish, AResidentSpacesEditTakesTheLocalInvalidateAndThePublicationBoth)
{
    // Check publication alongside the resident leaf invalidation.
    seed(running, VA_SEED, PA_A);

    ASSERT_EQ(arch_aspace_map(running, VA_DEEP, PA_B, 1, RIGHTS_DATA, ARCH_MAP_NORMAL),
              ARCH_ASPACE_OK);

    EXPECT_EQ(ops_with(OP_TLBI_PAGE_LOCAL), 1u);
    EXPECT_EQ(ops_with(OP_TLBI_PAGE_IS), 0u);
    EXPECT_EQ(ops_with(OP_DSB_ISHST), 2u);
}

// Creation and rollback publication.

TEST_F(MapPublish, AFreshRootIsPublishedBeforeItCanBeHandedOut)
{
    // A new root needs publication even without an invalidation.
    struct arch_aspace* const another = arch_aspace_create();
    ASSERT_NE(another, nullptr);

    EXPECT_EQ(ops_with(OP_DSB_ISHST), 1u);

    arch_aspace_destroy(another);
}

TEST_F(MapPublish, AFailedMapsUnwindPublishesItsClearsAtOneCore)
{
    // Rollback barriers must also run on one core.
    seed(fresh, VA_CROSS_BELOW, PA_A);
    set_frame_budget(0);

    ASSERT_EQ(arch_aspace_map(fresh, VA_CROSS_LOW, PA_C, 2, RIGHTS_DATA, ARCH_MAP_NORMAL),
              ARCH_ASPACE_ENOMEM);

    EXPECT_EQ(ops_with(OP_TLBI_ALL_LOCAL), 2u);
    EXPECT_EQ(ops_with(OP_TLBI_ALL_IS), 0u);
    EXPECT_GE(ops_with(OP_DSB_ISHST), 2u);
    EXPECT_EQ(arch_aspace_frame_at(fresh, VA_CROSS_LOW), 0u);
}
