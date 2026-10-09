// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Cases every translating map editor answers the same way, over a two-core seam built on
// sysops_seam.h. Include once per test binary, after defining:
//   MAP_CONTRACT_FIXTURE  the fixture name these cases register under; the refusals register
//                         under the same name suffixed Refusal
//   frame_pointer(pa)     the pointer acquire answers for a frame of the seam's RAM window
//   kernel_half_of(low)   a kernel-half address whose walk indexes reach `low`, where the
//                         translation regime has such an alias
//   MAP_CONTRACT_NOCACHE_SEEN  whether acquire reports a non-cacheable leaf as uncached, false
//                         where the kernel's view takes the leaf's type
//   MAP_CONTRACT_PA_BITS  the physical address width the seam's machine reports

#ifndef KICKOS_TESTS_UNIT_COMMON_MAP_CONTRACT_H
#define KICKOS_TESTS_UNIT_COMMON_MAP_CONTRACT_H

#include <kickos/arch/arch.h>

#include <gtest/gtest.h>

#include <string>

#include "sysops_seam.h"

namespace
{
    // Offsets from the backend's user offset.
    constexpr uintptr_t LOW_A = 0x10000000;
    constexpr uintptr_t LOW_B = 0x11000000;
    // Outputs outside the seam's RAM window; never dereferenced.
    constexpr arch_phys_addr_t PA_UNPOOLED = 0x20000000;
    constexpr arch_phys_addr_t PA_DEVICE = 0x08000000;
    constexpr uintptr_t IN_PAGE = 0x40;
    constexpr uint32_t RIGHTS_RW = ARCH_MAP_R | ARCH_MAP_W;

    // `watch` is the page a wrongly admitted map would have installed.
    void expect_map_refused(struct arch_aspace* space, uintptr_t va, arch_phys_addr_t pa,
                            size_t pages, uint32_t rights, uintptr_t watch)
    {
        uint32_t const allocated = kickos::testfix::frames_allocated();
        kickos::testfix::ops_clear();
        EXPECT_EQ(arch_aspace_map(space, va, pa, pages, rights, ARCH_MAP_NORMAL),
                  ARCH_ASPACE_EINVAL);
        EXPECT_EQ(kickos::testfix::frames_allocated(), allocated);
        EXPECT_EQ(kickos::testfix::ops_count(), 0u);
        EXPECT_EQ(arch_aspace_frame_at(space, watch), 0u);
    }
}

class MAP_CONTRACT_FIXTURE : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        kickos::testfix::sysops_reset();
        va_a = arch_aspace_user_offset() + LOW_A;
        va_b = arch_aspace_user_offset() + LOW_B;
        space = arch_aspace_create();
        ASSERT_NE(space, nullptr);
        for (uint32_t core = KICKOS_NUM_CORES; core > 0; core--)
        {
            kickos::testfix::set_cpu(core - 1u);
            arch_aspace_activate(space);
        }
        kickos::testfix::ops_clear();
    }

    void TearDown() override
    {
        arch_aspace_destroy(space);
    }

    struct arch_aspace* space = nullptr;
    uintptr_t va_a = 0;
    uintptr_t va_b = 0;
};

TEST_F(MAP_CONTRACT_FIXTURE, AcquireReportsTheLeafsCacheability)
{
    arch_phys_addr_t const plain = kickos_frame_alloc();
    arch_phys_addr_t const uncached_frame = kickos_frame_alloc();
    ASSERT_NE(plain, 0u);
    ASSERT_NE(uncached_frame, 0u);
    ASSERT_EQ(arch_aspace_map(space, va_a, plain, 1, RIGHTS_RW, ARCH_MAP_NORMAL), ARCH_ASPACE_OK);
    ASSERT_EQ(arch_aspace_map(space, va_b, uncached_frame, 1, RIGHTS_RW, ARCH_MAP_NOCACHE),
              ARCH_ASPACE_OK);
    bool uncached = true;
    ASSERT_NE(arch_aspace_acquire(space, va_a, &uncached), nullptr);
    arch_aspace_release(space, va_a);
    EXPECT_FALSE(uncached) << "a cacheable leaf was reported uncached";
    uncached = not MAP_CONTRACT_NOCACHE_SEEN;
    ASSERT_NE(arch_aspace_acquire(space, va_b, &uncached), nullptr);
    arch_aspace_release(space, va_b);
    EXPECT_EQ(uncached, MAP_CONTRACT_NOCACHE_SEEN) << "a non-cacheable leaf was misreported";
}

TEST_F(MAP_CONTRACT_FIXTURE, EveryNamedMemoryTypeIsHonouredAndNoOtherIsMapped)
{
    EXPECT_TRUE(arch_aspace_memtype_support(ARCH_MAP_NORMAL));
    EXPECT_TRUE(arch_aspace_memtype_support(ARCH_MAP_NOCACHE));
    EXPECT_TRUE(arch_aspace_memtype_support(ARCH_MAP_DEVICE));
    enum arch_map_memtype const unnamed = static_cast<enum arch_map_memtype>(3);
    EXPECT_FALSE(arch_aspace_memtype_support(unnamed));
    arch_phys_addr_t const frame = kickos_frame_alloc();
    ASSERT_NE(frame, 0u);
    EXPECT_NE(arch_aspace_map(space, va_a, frame, 1, RIGHTS_RW, unnamed), ARCH_ASPACE_OK);
    EXPECT_EQ(arch_aspace_frame_at(space, va_a), 0u);
}

TEST_F(MAP_CONTRACT_FIXTURE, AMappedFrameIsReachedThroughAcquireAndIsGoneAfterUnmap)
{
    arch_phys_addr_t const frame = kickos_frame_alloc();
    ASSERT_NE(frame, 0u);
    ASSERT_EQ(arch_aspace_map(space, va_a, frame, 1, RIGHTS_RW, ARCH_MAP_NORMAL), ARCH_ASPACE_OK);

    EXPECT_EQ(arch_aspace_acquire(space, va_a + IN_PAGE, nullptr),
              static_cast<unsigned char*>(frame_pointer(frame)) + IN_PAGE);
    arch_aspace_release(space, va_a + IN_PAGE);

    ASSERT_EQ(arch_aspace_unmap(space, va_a, 1), ARCH_ASPACE_OK);

    EXPECT_EQ(arch_aspace_acquire(space, va_a, nullptr), nullptr);
    EXPECT_EQ(arch_aspace_frame_at(space, va_a), 0u);
}

TEST_F(MAP_CONTRACT_FIXTURE, TwoPagesOverOneFrameReachOneStore)
{
    arch_phys_addr_t const frame = kickos_frame_alloc();
    ASSERT_NE(frame, 0u);
    ASSERT_EQ(arch_aspace_map(space, va_a, frame, 1, RIGHTS_RW, ARCH_MAP_NORMAL), ARCH_ASPACE_OK);
    ASSERT_EQ(arch_aspace_map(space, va_b, frame, 1, RIGHTS_RW, ARCH_MAP_NORMAL), ARCH_ASPACE_OK);

    EXPECT_EQ(arch_aspace_frame_at(space, va_a), frame);
    EXPECT_EQ(arch_aspace_frame_at(space, va_b), frame);
    EXPECT_EQ(arch_aspace_acquire(space, va_a, nullptr), frame_pointer(frame));
    EXPECT_EQ(arch_aspace_acquire(space, va_b, nullptr), frame_pointer(frame));
    arch_aspace_release(space, va_a);
    arch_aspace_release(space, va_b);
    // Destroy frees per leaf, so only one leaf may still name the frame.
    ASSERT_EQ(arch_aspace_unmap(space, va_b, 1), ARCH_ASPACE_OK);
}

TEST_F(MAP_CONTRACT_FIXTURE, TwoHoldsOfOnePageAnswerOnePointerAndAnotherPageItsOwn)
{
    ASSERT_EQ(arch_aspace_map(space, va_a, PA_DEVICE, 2, ARCH_MAP_R, ARCH_MAP_DEVICE),
              ARCH_ASPACE_OK);
    size_t const g = arch_aspace_granule();

    void* const a = arch_aspace_acquire(space, va_a, nullptr);
    void* const again = arch_aspace_acquire(space, va_a, nullptr);
    ASSERT_NE(a, nullptr);
    EXPECT_EQ(again, a);
    arch_aspace_release(space, va_a);
    // The surviving hold still names its pointer, so another page answers a different one.
    void* const other = arch_aspace_acquire(space, va_a + g, nullptr);
    ASSERT_NE(other, nullptr);
    EXPECT_NE(other, a);
    arch_aspace_release(space, va_a + g);
    arch_aspace_release(space, va_a);
    EXPECT_NE(arch_aspace_acquire(space, va_a, nullptr), nullptr);
    arch_aspace_release(space, va_a);
    // Destroy would offer device outputs back to the pool.
    ASSERT_EQ(arch_aspace_unmap(space, va_a, 2), ARCH_ASPACE_OK);
}

TEST_F(MAP_CONTRACT_FIXTURE, ARunAcrossLeafTablesIsContiguousBoundedAndUnmappedWhole)
{
    size_t const g = arch_aspace_granule();
    size_t const per_table = g / sizeof(uint64_t);
    uintptr_t const table_span = static_cast<uintptr_t>(per_table * g);
    // One page below a leaf-table boundary, a whole table, then half of a third.
    uintptr_t const start = va_a + table_span - g;
    size_t const pages = 1 + per_table + per_table / 2;
    struct arch_aspace* const fresh = arch_aspace_create();
    ASSERT_NE(fresh, nullptr);
    ASSERT_EQ(arch_aspace_map(fresh, start, PA_DEVICE, pages, ARCH_MAP_R, ARCH_MAP_DEVICE),
              ARCH_ASPACE_OK);

    for (size_t i = 0; i < pages; i++)
    {
        uintptr_t const at = start + i * g;
        ASSERT_EQ(arch_aspace_frame_at(fresh, at), PA_DEVICE + i * g) << "page " << i;
        void* const p = arch_aspace_acquire(fresh, at + IN_PAGE, nullptr);
        ASSERT_NE(p, nullptr) << "page " << i;
        EXPECT_EQ(reinterpret_cast<uintptr_t>(p) & (g - 1u), IN_PAGE) << "page " << i;
        arch_aspace_release(fresh, at + IN_PAGE);
    }
    EXPECT_EQ(arch_aspace_frame_at(fresh, start - g), 0u);
    EXPECT_EQ(arch_aspace_frame_at(fresh, start + pages * g), 0u);
    EXPECT_EQ(arch_aspace_acquire(fresh, start - g, nullptr), nullptr);
    EXPECT_EQ(arch_aspace_acquire(fresh, start + pages * g, nullptr), nullptr);

    ASSERT_EQ(arch_aspace_unmap(fresh, start, pages), ARCH_ASPACE_OK);

    EXPECT_EQ(arch_aspace_frame_at(fresh, start), 0u);
    EXPECT_EQ(arch_aspace_frame_at(fresh, start + (pages - 1u) * g), 0u);
    EXPECT_EQ(arch_aspace_acquire(fresh, start + (pages - 1u) * g, nullptr), nullptr);
    arch_aspace_destroy(fresh);
}

TEST_F(MAP_CONTRACT_FIXTURE, ASpaceCycleGivesBackEveryFrameItTookAndNoneTwice)
{
    uint32_t const allocated = kickos::testfix::frames_allocated();
    uint32_t const freed = kickos::testfix::frames_freed();

    for (int i = 0; i < 4; i++)
    {
        struct arch_aspace* const cycle = arch_aspace_create();
        ASSERT_NE(cycle, nullptr);
        arch_phys_addr_t const frame = kickos_frame_alloc();
        ASSERT_NE(frame, 0u);
        ASSERT_EQ(arch_aspace_map(cycle, va_a, frame, 1, RIGHTS_RW, ARCH_MAP_NORMAL),
                  ARCH_ASPACE_OK);
        ASSERT_EQ(arch_aspace_map(cycle, va_b, frame, 1, ARCH_MAP_R, ARCH_MAP_NORMAL),
                  ARCH_ASPACE_OK);
        ASSERT_EQ(arch_aspace_unmap(cycle, va_b, 1), ARCH_ASPACE_OK);
        arch_aspace_destroy(cycle);
    }

    EXPECT_GT(kickos::testfix::frames_allocated(), allocated);
    EXPECT_EQ(kickos::testfix::frames_freed() - freed,
              kickos::testfix::frames_allocated() - allocated);
}

struct MapRefusal
{
    char const* name;
    bool kernel_half;
    uintptr_t past_va_a;
    arch_phys_addr_t pa;
    size_t pages;
    uint32_t rights;
};

#define MAP_CONTRACT_JOIN_(a, b) a##b
#define MAP_CONTRACT_JOIN(a, b) MAP_CONTRACT_JOIN_(a, b)
#define MAP_CONTRACT_REFUSAL MAP_CONTRACT_JOIN(MAP_CONTRACT_FIXTURE, Refusal)

class MAP_CONTRACT_REFUSAL : public MAP_CONTRACT_FIXTURE,
                             public ::testing::WithParamInterface<MapRefusal>
{
};

TEST_P(MAP_CONTRACT_REFUSAL, MapIsRefused)
{
    MapRefusal const r = GetParam();
    uintptr_t va = va_a + r.past_va_a;
    if (r.kernel_half)
    {
        va = kernel_half_of(va);
    }

    expect_map_refused(space, va, r.pa, r.pages, r.rights, va_a);
}

INSTANTIATE_TEST_SUITE_P(
    Contract, MAP_CONTRACT_REFUSAL,
    ::testing::Values(
        MapRefusal{"AKernelHalfAddressIsRefused", true, 0, PA_UNPOOLED, 1, RIGHTS_RW},
        MapRefusal{"AnUnalignedAddressIsRefused", false, 1, PA_UNPOOLED, 1, RIGHTS_RW},
        MapRefusal{"AnEmptyRunIsRefused", false, 0, PA_UNPOOLED, 0, RIGHTS_RW},
        MapRefusal{"ARunWithoutReadIsRefused", false, 0, PA_UNPOOLED, 1, ARCH_MAP_W},
        MapRefusal{"AnUnknownRightIsRefused", false, 0, PA_UNPOOLED, 1, RIGHTS_RW | 0x80u},
        MapRefusal{"AWritableExecutableRunIsRefused", false, 0, PA_UNPOOLED, 1,
                   RIGHTS_RW | ARCH_MAP_X},
        MapRefusal{"ARunEndingPastThePhysicalWidthIsRefused", false, 0,
                   (static_cast<arch_phys_addr_t>(1) << MAP_CONTRACT_PA_BITS)
                       - kickos::testfix::SYSOPS_GRANULE,
                   2, RIGHTS_RW}),
    [](::testing::TestParamInfo<MapRefusal> const& p) { return std::string(p.param.name); });

TEST_F(MAP_CONTRACT_FIXTURE, APartiallyMappedMapIsRefusedBeforeAnyEdit)
{
    size_t const g = arch_aspace_granule();
    // The last page of a leaf table, so the run's second page needs a table of its own.
    uintptr_t const low = va_a + (g / sizeof(uint64_t)) * g - g;
    ASSERT_EQ(arch_aspace_map(space, low, PA_UNPOOLED, 1, RIGHTS_RW, ARCH_MAP_NORMAL),
              ARCH_ASPACE_OK);
    kickos::testfix::ops_clear();
    // An editor that edits before it refuses then fails that table and rolls back the low leaf.
    kickos::testfix::set_frame_budget(0);

    EXPECT_EQ(arch_aspace_map(space, low, PA_UNPOOLED + g, 2, RIGHTS_RW, ARCH_MAP_NORMAL),
              ARCH_ASPACE_EINVAL);

    EXPECT_EQ(arch_aspace_frame_at(space, low), PA_UNPOOLED);
    EXPECT_EQ(arch_aspace_frame_at(space, low + g), 0u);
    EXPECT_EQ(kickos::testfix::ops_count(), 0u);
}

TEST_F(MAP_CONTRACT_FIXTURE, APartiallyMappedUnmapIsRefusedAndKeepsTheLeaf)
{
    ASSERT_EQ(arch_aspace_map(space, va_a, PA_UNPOOLED, 1, RIGHTS_RW, ARCH_MAP_NORMAL),
              ARCH_ASPACE_OK);
    kickos::testfix::ops_clear();

    EXPECT_EQ(arch_aspace_unmap(space, va_a, 2), ARCH_ASPACE_EINVAL);

    EXPECT_EQ(arch_aspace_frame_at(space, va_a), PA_UNPOOLED);
    EXPECT_EQ(kickos::testfix::ops_count(), 0u);
}

#endif
