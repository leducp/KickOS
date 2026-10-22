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
//   larger_leaf_slot(space, va)  the entry one level above the leaf that `va` walks through,
//                         where the space already maps `va`
//   MAP_CONTRACT_LARGER_LEAF  a valid leaf descriptor for that entry, over a 2 MiB output the
//                         editor may read as memory

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

    // What the pool handed out and took back since construction.
    struct FrameCensus
    {
        uint32_t taken() const { return kickos::testfix::frames_allocated() - allocated; }
        uint32_t given() const { return kickos::testfix::frames_freed() - freed; }

        uint32_t allocated = kickos::testfix::frames_allocated();
        uint32_t freed = kickos::testfix::frames_freed();
    };

    // The tables a clean map of [va, va + pages) takes in a space mapping nothing.
    uint32_t tables_for(uintptr_t va, size_t pages)
    {
        struct arch_aspace* const probe = arch_aspace_create();
        if (probe == nullptr)
        {
            return 0;
        }
        FrameCensus const census;
        uint32_t taken = 0;
        if (arch_aspace_map(probe, va, PA_DEVICE, pages, ARCH_MAP_R, ARCH_MAP_DEVICE)
            == ARCH_ASPACE_OK)
        {
            taken = census.taken();
            (void)arch_aspace_unmap(probe, va, pages);
        }
        arch_aspace_destroy(probe);
        return taken;
    }

    bool none_mapped(struct arch_aspace* space, uintptr_t va, size_t pages)
    {
        size_t const g = arch_aspace_granule();
        for (size_t i = 0; i < pages; i++)
        {
            if (arch_aspace_frame_at(space, va + i * g) != 0)
            {
                return false;
            }
        }
        return true;
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
    ASSERT_NE(arch_aspace_acquire(space, va_a, &uncached, nullptr), nullptr);
    arch_aspace_release(space, va_a);
    EXPECT_FALSE(uncached) << "a cacheable leaf was reported uncached";
    uncached = not MAP_CONTRACT_NOCACHE_SEEN;
    ASSERT_NE(arch_aspace_acquire(space, va_b, &uncached, nullptr), nullptr);
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

    EXPECT_EQ(arch_aspace_acquire(space, va_a + IN_PAGE, nullptr, nullptr),
              static_cast<unsigned char*>(frame_pointer(frame)) + IN_PAGE);
    arch_aspace_release(space, va_a + IN_PAGE);

    ASSERT_EQ(arch_aspace_unmap(space, va_a, 1), ARCH_ASPACE_OK);

    EXPECT_EQ(arch_aspace_acquire(space, va_a, nullptr, nullptr), nullptr);
    EXPECT_EQ(arch_aspace_frame_at(space, va_a), 0u);
}

TEST_F(MAP_CONTRACT_FIXTURE, AcquireNamesTheFrameFrameAtAnswersAndZeroForAnUnmappedPage)
{
    size_t const g = arch_aspace_granule();
    ASSERT_EQ(arch_aspace_map(space, va_a, PA_DEVICE, 2, ARCH_MAP_R, ARCH_MAP_DEVICE),
              ARCH_ASPACE_OK);
    for (size_t i = 0; i < 2; i++)
    {
        uintptr_t const at = va_a + i * g + IN_PAGE;
        arch_phys_addr_t frame = 1;
        ASSERT_NE(arch_aspace_acquire(space, at, nullptr, &frame), nullptr) << "page " << i;
        arch_aspace_release(space, at);
        EXPECT_EQ(frame, arch_aspace_frame_at(space, at)) << "page " << i;
        EXPECT_EQ(frame, PA_DEVICE + i * g) << "page " << i;
    }
    ASSERT_EQ(arch_aspace_unmap(space, va_a, 2), ARCH_ASPACE_OK);
    arch_phys_addr_t frame = 1;
    EXPECT_EQ(arch_aspace_acquire(space, va_a, nullptr, &frame), nullptr);
    EXPECT_EQ(frame, 0u) << "an unmapped page named a frame";
}

TEST_F(MAP_CONTRACT_FIXTURE, TwoPagesOverOneFrameReachOneStore)
{
    arch_phys_addr_t const frame = kickos_frame_alloc();
    ASSERT_NE(frame, 0u);
    ASSERT_EQ(arch_aspace_map(space, va_a, frame, 1, RIGHTS_RW, ARCH_MAP_NORMAL), ARCH_ASPACE_OK);
    ASSERT_EQ(arch_aspace_map(space, va_b, frame, 1, RIGHTS_RW, ARCH_MAP_NORMAL), ARCH_ASPACE_OK);

    EXPECT_EQ(arch_aspace_frame_at(space, va_a), frame);
    EXPECT_EQ(arch_aspace_frame_at(space, va_b), frame);
    EXPECT_EQ(arch_aspace_acquire(space, va_a, nullptr, nullptr), frame_pointer(frame));
    EXPECT_EQ(arch_aspace_acquire(space, va_b, nullptr, nullptr), frame_pointer(frame));
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

    void* const a = arch_aspace_acquire(space, va_a, nullptr, nullptr);
    void* const again = arch_aspace_acquire(space, va_a, nullptr, nullptr);
    ASSERT_NE(a, nullptr);
    EXPECT_EQ(again, a);
    arch_aspace_release(space, va_a);
    // The surviving hold still names its pointer, so another page answers a different one.
    void* const other = arch_aspace_acquire(space, va_a + g, nullptr, nullptr);
    ASSERT_NE(other, nullptr);
    EXPECT_NE(other, a);
    arch_aspace_release(space, va_a + g);
    arch_aspace_release(space, va_a);
    EXPECT_NE(arch_aspace_acquire(space, va_a, nullptr, nullptr), nullptr);
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
        void* const p = arch_aspace_acquire(fresh, at + IN_PAGE, nullptr, nullptr);
        ASSERT_NE(p, nullptr) << "page " << i;
        EXPECT_EQ(reinterpret_cast<uintptr_t>(p) & (g - 1u), IN_PAGE) << "page " << i;
        arch_aspace_release(fresh, at + IN_PAGE);
    }
    EXPECT_EQ(arch_aspace_frame_at(fresh, start - g), 0u);
    EXPECT_EQ(arch_aspace_frame_at(fresh, start + pages * g), 0u);
    EXPECT_EQ(arch_aspace_acquire(fresh, start - g, nullptr, nullptr), nullptr);
    EXPECT_EQ(arch_aspace_acquire(fresh, start + pages * g, nullptr, nullptr), nullptr);

    ASSERT_EQ(arch_aspace_unmap(fresh, start, pages), ARCH_ASPACE_OK);

    EXPECT_EQ(arch_aspace_frame_at(fresh, start), 0u);
    EXPECT_EQ(arch_aspace_frame_at(fresh, start + (pages - 1u) * g), 0u);
    EXPECT_EQ(arch_aspace_acquire(fresh, start + (pages - 1u) * g, nullptr, nullptr), nullptr);
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

TEST_F(MAP_CONTRACT_FIXTURE, AMapOutOfFramesBeforeItsFirstLeafGivesBackItsTables)
{
    uint32_t const chain = tables_for(va_a, 1);
    ASSERT_GT(chain, 1u) << "a page needs a table below the root, then its leaf table";
    FrameCensus const census;
    kickos::testfix::set_frame_budget(chain - 1u);

    EXPECT_EQ(arch_aspace_map(space, va_a, PA_DEVICE, 1, ARCH_MAP_R, ARCH_MAP_DEVICE),
              ARCH_ASPACE_ENOMEM);

    EXPECT_EQ(census.taken(), chain - 1u);
    EXPECT_EQ(census.given(), census.taken()) << "the unwind kept a table it built";
    EXPECT_EQ(arch_aspace_frame_at(space, va_a), 0u);
    kickos::testfix::set_frame_budget(kickos::testfix::SYSOPS_FRAMES);
    ASSERT_EQ(arch_aspace_map(space, va_a, PA_DEVICE, 1, ARCH_MAP_R, ARCH_MAP_DEVICE),
              ARCH_ASPACE_OK);
    EXPECT_EQ(arch_aspace_frame_at(space, va_a), PA_DEVICE);
    ASSERT_EQ(arch_aspace_unmap(space, va_a, 1), ARCH_ASPACE_OK);
}

TEST_F(MAP_CONTRACT_FIXTURE, AMapOutOfFramesPartwayUnwindsEveryLeafAndTable)
{
    size_t const g = arch_aspace_granule();
    uintptr_t const table_span = static_cast<uintptr_t>((g / sizeof(uint64_t)) * g);
    // One page below a leaf-table boundary, a whole table, then half of a third.
    uintptr_t const start = va_a + table_span - g;
    size_t const pages = 1 + table_span / g + table_span / g / 2;
    uint32_t const tables = tables_for(start, pages);
    ASSERT_GT(tables, 3u);
    FrameCensus const census;
    // The last leaf table fails, after every leaf the first two hold went in.
    kickos::testfix::set_frame_budget(tables - 1u);

    EXPECT_EQ(arch_aspace_map(space, start, PA_DEVICE, pages, ARCH_MAP_R, ARCH_MAP_DEVICE),
              ARCH_ASPACE_ENOMEM);

    EXPECT_TRUE(none_mapped(space, start, pages)) << "the unwind left a leaf of the run";
    EXPECT_EQ(census.taken(), tables - 1u);
    EXPECT_EQ(census.given(), census.taken()) << "the unwind kept a table it built";
}

TEST_F(MAP_CONTRACT_FIXTURE, AMapOutOfFramesBesideALivePageKeepsThePageAndItsTables)
{
    size_t const g = arch_aspace_granule();
    uintptr_t const table_span = static_cast<uintptr_t>((g / sizeof(uint64_t)) * g);
    uintptr_t const start = va_a + table_span - g;
    size_t const pages = 1 + table_span / g + table_span / g / 2;
    // In the run's last leaf table, just past its end.
    uintptr_t const live = start + pages * g;
    ASSERT_EQ(arch_aspace_map(space, live, PA_UNPOOLED, 1, RIGHTS_RW, ARCH_MAP_NORMAL),
              ARCH_ASPACE_OK);
    FrameCensus const census;
    // The run's first leaf table goes in, its second fails.
    kickos::testfix::set_frame_budget(1);

    EXPECT_EQ(arch_aspace_map(space, start, PA_DEVICE, pages, ARCH_MAP_R, ARCH_MAP_DEVICE),
              ARCH_ASPACE_ENOMEM);

    EXPECT_EQ(arch_aspace_frame_at(space, live), PA_UNPOOLED) << "the unwind took the live page";
    EXPECT_TRUE(none_mapped(space, start, pages));
    EXPECT_EQ(census.taken(), 1u);
    EXPECT_EQ(census.given(), census.taken());
    ASSERT_EQ(arch_aspace_unmap(space, live, 1), ARCH_ASPACE_OK);
}

TEST_F(MAP_CONTRACT_FIXTURE, AWhollyMappedRangeIsRemappedInPlace)
{
    size_t const g = arch_aspace_granule();
    // The last page of one leaf table and the first of the next.
    uintptr_t const low = va_a + (g / sizeof(uint64_t)) * g - g;
    ASSERT_EQ(arch_aspace_map(space, low, PA_UNPOOLED, 2, RIGHTS_RW, ARCH_MAP_NORMAL),
              ARCH_ASPACE_OK);
    FrameCensus const census;

    ASSERT_EQ(arch_aspace_map(space, low, PA_DEVICE, 2, ARCH_MAP_R, ARCH_MAP_DEVICE),
              ARCH_ASPACE_OK);

    EXPECT_EQ(arch_aspace_frame_at(space, low), PA_DEVICE);
    EXPECT_EQ(arch_aspace_frame_at(space, low + g), PA_DEVICE + g);
    EXPECT_EQ(census.taken(), 0u);
    EXPECT_EQ(census.given(), 0u);
    ASSERT_EQ(arch_aspace_unmap(space, low, 2), ARCH_ASPACE_OK);
}

TEST_F(MAP_CONTRACT_FIXTURE, AnUnmapGivesBackEveryTableItEmptied)
{
    size_t const g = arch_aspace_granule();
    uintptr_t const table_span = static_cast<uintptr_t>((g / sizeof(uint64_t)) * g);
    uintptr_t const start = va_a + table_span - g;
    size_t const pages = 1 + table_span / g + table_span / g / 2;
    FrameCensus const census;
    ASSERT_EQ(arch_aspace_map(space, start, PA_DEVICE, pages, ARCH_MAP_R, ARCH_MAP_DEVICE),
              ARCH_ASPACE_OK);
    uint32_t const tables = census.taken();
    ASSERT_GT(tables, 3u);

    ASSERT_EQ(arch_aspace_unmap(space, start, pages), ARCH_ASPACE_OK);

    EXPECT_EQ(census.given(), tables) << "the unmap kept a table it emptied";
    ASSERT_EQ(arch_aspace_map(space, start, PA_DEVICE, 1, ARCH_MAP_R, ARCH_MAP_DEVICE),
              ARCH_ASPACE_OK);
    EXPECT_EQ(arch_aspace_frame_at(space, start), PA_DEVICE);
    ASSERT_EQ(arch_aspace_unmap(space, start, 1), ARCH_ASPACE_OK);
}

TEST_F(MAP_CONTRACT_FIXTURE, AnUnmapKeepsATableAnotherPageStillUses)
{
    size_t const g = arch_aspace_granule();
    uint32_t const chain = tables_for(va_a, 1);
    ASSERT_EQ(arch_aspace_map(space, va_a, PA_UNPOOLED, 2, RIGHTS_RW, ARCH_MAP_NORMAL),
              ARCH_ASPACE_OK);
    FrameCensus const census;

    ASSERT_EQ(arch_aspace_unmap(space, va_a, 1), ARCH_ASPACE_OK);

    EXPECT_EQ(census.given(), 0u) << "the unmap took a table a live page uses";
    EXPECT_EQ(arch_aspace_frame_at(space, va_a + g), PA_UNPOOLED + g);
    ASSERT_EQ(arch_aspace_unmap(space, va_a + g, 1), ARCH_ASPACE_OK);
    EXPECT_EQ(census.given(), chain);
}

TEST_F(MAP_CONTRACT_FIXTURE, AMapUnderALargerLeafIsRefusedAndTheLeafStands)
{
    size_t const g = arch_aspace_granule();
    ASSERT_EQ(arch_aspace_map(space, va_a, PA_UNPOOLED, 1, RIGHTS_RW, ARCH_MAP_NORMAL),
              ARCH_ASPACE_OK);
    uint64_t* const slot = larger_leaf_slot(space, va_a);
    ASSERT_NE(slot, nullptr);
    uint64_t const table = *slot;
    *slot = MAP_CONTRACT_LARGER_LEAF;
    FrameCensus const census;
    kickos::testfix::ops_clear();

    EXPECT_EQ(arch_aspace_map(space, va_a + g, PA_UNPOOLED, 1, RIGHTS_RW, ARCH_MAP_NORMAL),
              ARCH_ASPACE_EINVAL);

    EXPECT_EQ(*slot, MAP_CONTRACT_LARGER_LEAF) << "the larger leaf was replaced";
    EXPECT_EQ(census.taken(), 0u);
    EXPECT_EQ(kickos::testfix::ops_count(), 0u);
    *slot = table;
    ASSERT_EQ(arch_aspace_unmap(space, va_a, 1), ARCH_ASPACE_OK);
}

TEST_F(MAP_CONTRACT_FIXTURE, TheBootSpaceIsNeverEdited)
{
    FrameCensus const census;
    kickos::testfix::ops_clear();

    EXPECT_EQ(arch_aspace_map(arch_aspace_boot(), va_a, PA_UNPOOLED, 1, RIGHTS_RW,
                              ARCH_MAP_NORMAL),
              ARCH_ASPACE_EINVAL);

    EXPECT_EQ(arch_aspace_frame_at(arch_aspace_boot(), va_a), 0u);
    EXPECT_EQ(census.taken(), 0u);
    EXPECT_EQ(kickos::testfix::ops_count(), 0u);
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
