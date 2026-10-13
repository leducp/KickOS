// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Check x86-64 invalidation elision and shootdown cost through recorded system operations.
// Build all test mappings through the backend. These tests do not model a hardware TLB.

#include <kickos/arch/arch.h>
#include <kickos/arch/aspace.h>

#include <gtest/gtest.h>

#include <string>

#include "x86_64_sysops_seam.h"

using namespace kickos::testfix;

namespace
{
    static_assert(KICKOS_KERNEL_CORES > 1,
                  "map_shoot must compile the multicore arm of peers_reload");
    static_assert(KICKOS_NUM_CORES > 1, "the peer set needs a core other than this one");

    constexpr uint32_t RUNNING_CORE = 0;
    constexpr uint32_t PEER_CORE = 1;
    constexpr uint32_t PEER_MASK = 1u << PEER_CORE;

    // Outputs in the firmware's identity map but outside the frame pool; never dereferenced.
    constexpr arch_phys_addr_t PA_A = 0x20000000;
    constexpr arch_phys_addr_t PA_B = 0x20001000;

    constexpr uint32_t RIGHTS_DATA = ARCH_MAP_R | ARCH_MAP_W;
    constexpr size_t MANY = 8;

    // A user page: below the user slot base the backend refuses every address.
    uintptr_t user_va(uintptr_t low)
    {
        return arch_aspace_user_offset() + low;
    }

    class MapShoot : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            sysops_reset();
            va = user_va(0x10000000);
            space = arch_aspace_create();
            ASSERT_NE(space, nullptr);
            elsewhere = arch_aspace_create();
            ASSERT_NE(elsewhere, nullptr);
            set_cpu(PEER_CORE);
            arch_aspace_activate(space);
            set_cpu(RUNNING_CORE);
            arch_aspace_activate(space);
            watch_space(space);
            ops_clear();
        }

        // Destroy spaces to release backend residency rows; sysops_reset does not clear them.
        void TearDown() override
        {
            arch_aspace_destroy(space);
            arch_aspace_destroy(elsewhere);
            space = nullptr;
            elsewhere = nullptr;
        }

        void seed(size_t pages)
        {
            ASSERT_EQ(arch_aspace_map(space, va, PA_A, pages, RIGHTS_DATA, ARCH_MAP_NORMAL),
                      ARCH_ASPACE_OK);
            ops_clear();
        }

        void both_cores_leave()
        {
            set_cpu(PEER_CORE);
            arch_aspace_activate(elsewhere);
            set_cpu(RUNNING_CORE);
            arch_aspace_activate(elsewhere);
            ops_clear();
        }

        uintptr_t va = 0;
        struct arch_aspace* space = nullptr;
        struct arch_aspace* elsewhere = nullptr;
    };

    void* frame_pointer(arch_phys_addr_t pa)
    {
        return reinterpret_cast<void*>(static_cast<uintptr_t>(pa));
    }

    // A user slot is in the low half, so no canonical kernel-half address reaches it: a wrongly
    // admitted map shows only in the frames it allocates.
    uintptr_t kernel_half_of(uintptr_t low)
    {
        return ~((static_cast<uintptr_t>(1) << 47) - 1u) | low;
    }

    constexpr uint64_t PTE_P = 1u << 0;
    constexpr uint64_t PTE_ACCESSED = 1u << 5;
    constexpr uint64_t PTE_PS = 1u << 7;
    constexpr uint64_t PTE_ADDR = 0x000ffffffffff000ull;

    uint64_t* larger_leaf_slot(struct arch_aspace* space, uintptr_t va)
    {
        uint64_t* table = reinterpret_cast<uint64_t*>(space);
        for (unsigned shift = 39; shift > 21; shift -= 9)
        {
            uint64_t const desc = table[(va >> shift) & 0x1FFu];
            if ((desc & PTE_P) == 0 or (desc & PTE_PS) != 0)
            {
                return nullptr;
            }
            table = reinterpret_cast<uint64_t*>(static_cast<uintptr_t>(desc & PTE_ADDR));
        }
        return &table[(va >> 21) & 0x1FFu];
    }
}

#define MAP_CONTRACT_FIXTURE MapShootContract
#define MAP_CONTRACT_NOCACHE_SEEN true
#define MAP_CONTRACT_PA_BITS 48u
// A user-readable 2 MiB leaf over physical 0: P, US and PS.
#define MAP_CONTRACT_LARGER_LEAF static_cast<uint64_t>(0x85u)
#include "map_contract.h"

// Invalidation and its elision.

TEST_F(MapShoot, AFreshLeafInARunningSpaceIsInvalidatedOnce)
{
    ASSERT_EQ(arch_aspace_map(space, va, PA_A, 1, RIGHTS_DATA, ARCH_MAP_NORMAL), ARCH_ASPACE_OK);

    ASSERT_EQ(ops_with(OP_INVLPG), 1u);
    EXPECT_EQ(arg_of_nth(OP_INVLPG, 0), va);
}

TEST_F(MapShoot, ReplacingALivePageInvalidatesBetweenTheClearAndTheStore)
{
    seed(1);

    ASSERT_EQ(arch_aspace_map(space, va, PA_B, 1, RIGHTS_DATA, ARCH_MAP_NORMAL), ARCH_ASPACE_OK);

    ASSERT_EQ(ops_with(OP_INVLPG), 2u);
    EXPECT_EQ(arg_of_nth(OP_INVLPG, 0), va);
    EXPECT_EQ(arg_of_nth(OP_INVLPG_LEAF, 0), 0u)
        << "the old leaf was still stored when it was invalidated";
    EXPECT_EQ(arg_of_nth(OP_INVLPG, 1), va);
    EXPECT_EQ(arg_of_nth(OP_INVLPG_LEAF, 1), kickos::x86_64::aspace_leaf_desc(space, va));
    EXPECT_EQ(arch_aspace_frame_at(space, va), PA_B);
}

TEST_F(MapShoot, UnmapInvalidatesEachPageOnce)
{
    seed(1);

    ASSERT_EQ(arch_aspace_unmap(space, va, 1), ARCH_ASPACE_OK);

    ASSERT_EQ(ops_with(OP_INVLPG), 1u);
    EXPECT_EQ(arg_of_nth(OP_INVLPG, 0), va);
    EXPECT_EQ(arch_aspace_frame_at(space, va), 0u);
}

TEST_F(MapShoot, ASpaceNoCoreHasRunIsNeitherInvalidatedNorShotDown)
{
    // A task's image seed maps many pages into a space no core has run.
    constexpr size_t SEED_PAGES = 40;
    struct arch_aspace* const unrun = arch_aspace_create();
    ASSERT_NE(unrun, nullptr);
    ops_clear();

    ASSERT_EQ(arch_aspace_map(unrun, va, PA_A, SEED_PAGES, RIGHTS_DATA, ARCH_MAP_NORMAL),
              ARCH_ASPACE_OK);

    EXPECT_EQ(ops_with(OP_INVLPG), 0u);
    EXPECT_EQ(ops_with(OP_RENDEZVOUS), 0u);
    size_t const g = arch_aspace_granule();
    EXPECT_EQ(arch_aspace_frame_at(unrun, va), PA_A);
    EXPECT_EQ(arch_aspace_frame_at(unrun, va + (SEED_PAGES - 1u) * g),
              PA_A + (SEED_PAGES - 1u) * g);

    ASSERT_EQ(arch_aspace_unmap(unrun, va, SEED_PAGES), ARCH_ASPACE_OK);

    EXPECT_EQ(ops_with(OP_INVLPG), 0u);
    EXPECT_EQ(ops_with(OP_RENDEZVOUS), 0u);
    arch_aspace_destroy(unrun);
}

TEST_F(MapShoot, ASpaceEveryCoreHasLeftIsStillInvalidated)
{
    both_cores_leave();

    ASSERT_EQ(arch_aspace_map(space, va, PA_A, 1, RIGHTS_DATA, ARCH_MAP_NORMAL), ARCH_ASPACE_OK);

    ASSERT_EQ(ops_with(OP_INVLPG), 1u);
    EXPECT_EQ(arg_of_nth(OP_INVLPG, 0), va);
    ASSERT_EQ(ops_with(OP_RENDEZVOUS), 1u);
    EXPECT_EQ(arg_of_nth(OP_RENDEZVOUS, 0), PEER_MASK);
}

// One rendezvous per mapping change, whatever its page count.

TEST_F(MapShoot, AMapOfManyPagesCostsTheOneRendezvousAMapOfOnePageDoes)
{
    size_t const g = arch_aspace_granule();

    ASSERT_EQ(arch_aspace_map(space, va, PA_A, 1, RIGHTS_DATA, ARCH_MAP_NORMAL), ARCH_ASPACE_OK);
    ASSERT_EQ(ops_with(OP_RENDEZVOUS), 1u);
    EXPECT_EQ(arg_of_nth(OP_RENDEZVOUS, 0), PEER_MASK);
    ops_clear();

    ASSERT_EQ(arch_aspace_map(space, va + g, PA_B, MANY, RIGHTS_DATA, ARCH_MAP_NORMAL),
              ARCH_ASPACE_OK);
    EXPECT_EQ(ops_with(OP_INVLPG), MANY);
    ASSERT_EQ(ops_with(OP_RENDEZVOUS), 1u);
    EXPECT_EQ(arg_of_nth(OP_RENDEZVOUS, 0), PEER_MASK);
}

TEST_F(MapShoot, AnUnmapOfManyPagesCostsOneRendezvous)
{
    seed(MANY);

    ASSERT_EQ(arch_aspace_unmap(space, va, MANY), ARCH_ASPACE_OK);

    EXPECT_EQ(ops_with(OP_INVLPG), MANY);
    ASSERT_EQ(ops_with(OP_RENDEZVOUS), 1u);
    EXPECT_EQ(arg_of_nth(OP_RENDEZVOUS, 0), PEER_MASK);
}

TEST_F(MapShoot, TheDestroyRendezvousRingsBeforeAnyFrameGoesBack)
{
    seed(1);
    uint32_t const before = frames_freed();

    arch_aspace_destroy(space);
    space = nullptr;

    ASSERT_EQ(ops_with(OP_RENDEZVOUS), 1u);
    EXPECT_EQ(arg_of_nth(OP_RENDEZVOUS, 0), PEER_MASK);
    EXPECT_EQ(frames_freed_at_rendezvous(), before);
    EXPECT_GT(frames_freed(), before);
}

TEST_F(MapShoot, TheTablesAnUnmapEmptiesGoBackOnlyAfterEveryCoreReloads)
{
    seed(1);
    uint32_t const before = frames_freed();

    ASSERT_EQ(arch_aspace_unmap(space, va, 1), ARCH_ASPACE_OK);

    EXPECT_EQ(frames_freed() - before, 3u);
    ASSERT_EQ(ops_with(OP_WRITE_CR3), 1u) << "a detached table's walks were left cached here";
    ASSERT_EQ(ops_with(OP_RENDEZVOUS), 1u);
    EXPECT_GT(last_index_of(OP_RENDEZVOUS), last_index_of(OP_WRITE_CR3));
    EXPECT_EQ(frames_freed_at_rendezvous(), before);
}

// The boot space, a null space and the refusals this backend makes beside the shared contract.

TEST_F(MapShoot, TheBootSpaceRefusesAnUnmapAndItsDestroyFreesNothing)
{
    uint32_t const allocated = frames_allocated();
    uint32_t const freed = frames_freed();
    EXPECT_EQ(arch_aspace_unmap(arch_aspace_boot(), va, 1), ARCH_ASPACE_EINVAL);
    arch_aspace_destroy(arch_aspace_boot());
    arch_aspace_destroy(nullptr);
    EXPECT_EQ(frames_allocated(), allocated);
    EXPECT_EQ(frames_freed(), freed);
    EXPECT_EQ(ops_with(OP_WRITE_CR3), 0u);
}

TEST_F(MapShoot, ACreateCostsOneFrameAndItsDestroyGivesItBack)
{
    uint32_t const allocated = frames_allocated();
    uint32_t const freed = frames_freed();
    struct arch_aspace* const fresh = arch_aspace_create();
    ASSERT_NE(fresh, nullptr);
    EXPECT_EQ(frames_allocated() - allocated, 1u);
    arch_aspace_destroy(fresh);
    EXPECT_EQ(frames_freed() - freed, 1u);
}

TEST_F(MapShoot, ANullSpaceIsRefusedAndAnswersNothing)
{
    EXPECT_EQ(arch_aspace_map(nullptr, va, PA_A, 1, RIGHTS_DATA, ARCH_MAP_NORMAL),
              ARCH_ASPACE_EINVAL);
    EXPECT_EQ(arch_aspace_frame_at(nullptr, va), 0u);
    EXPECT_EQ(arch_aspace_acquire(nullptr, va, nullptr), nullptr);
}

TEST_F(MapShoot, AMisalignedFrameAWrappingCountAndASlotBelowTheUserHalfAreRefused)
{
    uint32_t const allocated = frames_allocated();
    EXPECT_EQ(arch_aspace_map(space, va, PA_A + 1u, 1, RIGHTS_DATA, ARCH_MAP_NORMAL),
              ARCH_ASPACE_EINVAL);
    EXPECT_EQ(arch_aspace_map(space, va, PA_A, SIZE_MAX, RIGHTS_DATA, ARCH_MAP_NORMAL),
              ARCH_ASPACE_EINVAL);
    uintptr_t const below = arch_aspace_user_offset() - arch_aspace_granule();
    EXPECT_EQ(arch_aspace_map(space, below, PA_A, 1, RIGHTS_DATA, ARCH_MAP_NORMAL),
              ARCH_ASPACE_EINVAL);
    EXPECT_EQ(frames_allocated(), allocated);
    EXPECT_EQ(arch_aspace_frame_at(space, va), 0u);
}

TEST_F(MapShoot, AnAddressOutsideTheUserHalfAnswersNoFrame)
{
    uintptr_t const below = arch_aspace_user_offset() - arch_aspace_granule();
    EXPECT_EQ(arch_aspace_frame_at(space, below), 0u);
    EXPECT_EQ(arch_aspace_acquire(space, below, nullptr), nullptr);
    EXPECT_EQ(arch_aspace_frame_at(space, kernel_half_of(0x1000)), 0u);
    EXPECT_EQ(arch_aspace_acquire(space, kernel_half_of(0x1000), nullptr), nullptr);
}

TEST_F(MapShoot, AFrameIsAnsweredForAnyByteOfItsPage)
{
    seed(1);
    EXPECT_EQ(arch_aspace_frame_at(space, va + 0x123u), PA_A);
}

// The accessed and dirty bits diverge per root, so a kernel slot is told by presence.
TEST_F(MapShoot, ADestroyKeepsAKernelSlotWhoseCopyTheProcessorMarked)
{
    struct arch_aspace* const fresh = arch_aspace_create();
    ASSERT_NE(fresh, nullptr);
    uint64_t* const root = reinterpret_cast<uint64_t*>(fresh);
    size_t slot = 0;
    while (slot < 512u and (root[slot] & PTE_P) == 0)
    {
        slot++;
    }
    ASSERT_LT(slot, 512u);
    root[slot] |= PTE_ACCESSED;
    uint32_t const freed = frames_freed();
    arch_aspace_destroy(fresh);
    EXPECT_EQ(frames_freed() - freed, 1u) << "the kernel half's table went back with the root";
}

// The user bit ANDs down the walk: an entry without it anywhere above a leaf hides the leaf.
struct UserChain
{
    int leaf_level;
    int clear_level;
    bool five_levels;
    bool refused;
};

class AdoptedMap : public ::testing::TestWithParam<UserChain>
{
};

TEST_P(AdoptedMap, AReachableUserLeafRefusesTheBoot)
{
    UserChain const c = GetParam();
    sysops_reset();
    firmware_user_chain(c.leaf_level, c.clear_level, c.five_levels);
    if (c.refused)
    {
        EXPECT_DEATH(adopt(), "lets ring 3 reach a leaf of the kernel half");
    }
    else
    {
        adopt();
    }
    sysops_reset();
}

INSTANTIATE_TEST_SUITE_P(
    Layouts, AdoptedMap,
    ::testing::Values(UserChain{3, 0, false, true}, UserChain{2, 0, false, true},
                      UserChain{1, 0, false, true}, UserChain{3, 0, true, true},
                      UserChain{3, 4, false, false}, UserChain{1, 3, false, false},
                      UserChain{1, 2, false, false}, UserChain{1, 1, false, false},
                      UserChain{2, 5, true, false}, UserChain{2, 4, true, false}),
    [](::testing::TestParamInfo<UserChain> const& p) {
        UserChain const& c = p.param;
        std::string name = "leaf" + std::to_string(c.leaf_level) + "_clear"
                           + std::to_string(c.clear_level);
        if (c.five_levels)
        {
            name += "_five";
        }
        return name;
    });

// The attribute table decode (Intel SDM Vol 3 Tables 14-10 and 14-12): the three leaf bits name
// the first field encoding the type.
namespace
{
    constexpr uint64_t PWT = 1u << 3;
    constexpr uint64_t PCD = 1u << 4;
    constexpr uint64_t PAT = 1u << 7;
    constexpr uint64_t REFUSED = ~0ull;

    constexpr uint64_t PAT_POWER_UP = 0x0007040600070406ull;
    // Write-back at field 4, UC- at 5 and UC at 6.
    constexpr uint64_t PAT_HIGH_HALF = 0x0400070601040501ull;
    // Write-back at fields 1 and 5.
    constexpr uint64_t PAT_TWICE = 0x0704060007040600ull;
    constexpr uint64_t PAT_NO_WB = 0x0401000705040100ull;
    constexpr uint64_t PAT_NO_UC_MINUS = 0x0406000406000406ull;
    constexpr uint64_t PAT_NO_UC = 0x0406070406070406ull;

    uint64_t decode(uint64_t pat, enum arch_map_memtype type)
    {
        uint64_t bits = 0;
        if (not kickos::x86_64::aspace_memtype_bits(pat, type, &bits))
        {
            return REFUSED;
        }
        return bits;
    }

    struct PatDecode
    {
        char const* name;
        uint64_t pat;
        enum arch_map_memtype type;
        uint64_t bits;
    };

    class AttributeTable : public ::testing::TestWithParam<PatDecode>
    {
    };
}

TEST_P(AttributeTable, TheLeafBitsNameTheFirstFieldOfTheType)
{
    PatDecode const d = GetParam();
    EXPECT_EQ(decode(d.pat, d.type), d.bits);
}

INSTANTIATE_TEST_SUITE_P(
    Decode, AttributeTable,
    ::testing::Values(
        PatDecode{"PowerUpNormal", PAT_POWER_UP, ARCH_MAP_NORMAL, 0},
        PatDecode{"PowerUpNocache", PAT_POWER_UP, ARCH_MAP_NOCACHE, PCD},
        PatDecode{"PowerUpDevice", PAT_POWER_UP, ARCH_MAP_DEVICE, PWT | PCD},
        PatDecode{"PermutedNormal", PAT_HIGH_HALF, ARCH_MAP_NORMAL, PAT},
        PatDecode{"PermutedNocache", PAT_HIGH_HALF, ARCH_MAP_NOCACHE, PAT | PWT},
        PatDecode{"PermutedDevice", PAT_HIGH_HALF, ARCH_MAP_DEVICE, PAT | PCD},
        PatDecode{"TheFirstMatchIsTaken", PAT_TWICE, ARCH_MAP_NORMAL, PWT},
        PatDecode{"NoWriteBackRefusesNormal", PAT_NO_WB, ARCH_MAP_NORMAL, REFUSED},
        PatDecode{"NoWriteBackStillAnswersNocache", PAT_NO_WB, ARCH_MAP_NOCACHE, PAT},
        PatDecode{"NoWriteBackStillAnswersDevice", PAT_NO_WB, ARCH_MAP_DEVICE, 0},
        PatDecode{"NoUncachedMinusRefusesNocache", PAT_NO_UC_MINUS, ARCH_MAP_NOCACHE, REFUSED},
        PatDecode{"NoUncacheableRefusesDevice", PAT_NO_UC, ARCH_MAP_DEVICE, REFUSED},
        PatDecode{"AnUnknownTypeIsRefused", PAT_POWER_UP, static_cast<enum arch_map_memtype>(3),
                  REFUSED}),
    [](::testing::TestParamInfo<PatDecode> const& p) { return std::string(p.param.name); });

TEST(AttributeTableInUse, LeavesAreBuiltFromTheLiveRegister)
{
    sysops_reset();
    set_pat(PAT_HIGH_HALF);
    adopt();
    EXPECT_EQ(kickos::x86_64::aspace_attribute_table(), PAT_HIGH_HALF);
    struct arch_aspace* const space = arch_aspace_create();
    ASSERT_NE(space, nullptr);
    size_t const g = arch_aspace_granule();
    uintptr_t const va = user_va(0x10000000);
    enum arch_map_memtype const types[] = {ARCH_MAP_NORMAL, ARCH_MAP_NOCACHE, ARCH_MAP_DEVICE};

    for (size_t i = 0; i < 3; i++)
    {
        uintptr_t const at = va + i * g;
        ASSERT_EQ(arch_aspace_map(space, at, PA_A, 1, ARCH_MAP_R, types[i]), ARCH_ASPACE_OK);
        EXPECT_EQ(kickos::x86_64::aspace_leaf_desc(space, at) & (PWT | PCD | PAT),
                  decode(PAT_HIGH_HALF, types[i]))
            << "type " << i;
    }
    bool uncached = false;
    ASSERT_NE(arch_aspace_acquire(space, va + g, &uncached), nullptr);
    EXPECT_TRUE(uncached);
    ASSERT_NE(arch_aspace_acquire(space, va, &uncached), nullptr);
    EXPECT_FALSE(uncached);

    ASSERT_EQ(arch_aspace_unmap(space, va, 3), ARCH_ASPACE_OK);
    arch_aspace_destroy(space);
    sysops_reset();
}
