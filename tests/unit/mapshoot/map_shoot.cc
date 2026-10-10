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
}

#define MAP_CONTRACT_FIXTURE MapShootContract
#define MAP_CONTRACT_NOCACHE_SEEN true
#define MAP_CONTRACT_PA_BITS 48u
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
