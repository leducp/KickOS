// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Check ARM64 mapping maintenance, residency and ASID allocation through
// recorded system operations. Build all test mappings through the backend.
// These tests do not model a hardware TLB.

#include <kickos/arch/arch.h>
#include <kickos/diag.h>

#include <gtest/gtest.h>

#include <string>

#include "aspace_sysops_seam.h"
#include "uguard_armv8a.h"
#include "vmsa.h"

using namespace kickos::testfix;

// The seam's physical window (aspace_sysops_seam.cc).
extern "C" unsigned char __kickos_arm64_va_base[];
extern "C" uint64_t kickos_armv8a_empty_ttbr(void);

namespace
{
    // Peer synchronization requires the multicore code paths.
    static_assert(KICKOS_KERNEL_CORES > 1,
                  "map_exec must compile the multicore arm of removal_owes_rendezvous, "
                  "peer_cores and instruction_side_rendezvous");
    static_assert(KICKOS_NUM_CORES > 1, "the peer set needs a core other than this one");

    constexpr uint32_t RUNNING_CORE = 0;
    constexpr uint32_t PEER_CORE = 1;
    constexpr uint32_t PEER_MASK = 1u << PEER_CORE;

    // T0SZ=25 gives levels 1 through 3; each leaf requires two recursive calls.
    constexpr uintptr_t VA_DEEP = 0x40201000;  // level-1 index 1, level-2 index 1, level-3 index 1
    constexpr uintptr_t VA_OTHER = 0x40203000; // the same level-3 table, a different slot
    // Cross a level-2 boundary to edit leaves in separate recursive calls.
    constexpr uintptr_t VA_CROSS_LOW = 0x401FF000;  // level-2 index 0, level-3 index 511
    constexpr uintptr_t VA_CROSS_HIGH = 0x40200000; // level-2 index 1, level-3 index 0
    // Seed the low page's table without partially mapping the tested range.
    constexpr uintptr_t VA_CROSS_BELOW = 0x401FE000; // level-2 index 0, level-3 index 510

    constexpr arch_phys_addr_t PA_A = 0x20000000;
    constexpr arch_phys_addr_t PA_B = 0x20001000;
    constexpr arch_phys_addr_t PA_C = 0x20002000;

    constexpr uint32_t RIGHTS_EXEC = ARCH_MAP_R | ARCH_MAP_X;
    constexpr uint32_t RIGHTS_DATA = ARCH_MAP_R | ARCH_MAP_W;

    // Distinguish no register write from ASID 0.
    constexpr uint32_t NO_WRITE = 0xFFFFFFFFu;

    // Physical addresses are offsets into the host window array.
    uint64_t root_pa(struct arch_aspace* space)
    {
        return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(space) -
                                     reinterpret_cast<uintptr_t>(__kickos_arm64_va_base));
    }

    // TTBR0_EL1[63:48] of the last base-register write in the record.
    uint32_t last_identifier()
    {
        int const at = last_index_of(OP_WRITE_TTBR0);
        if (at < 0)
        {
            return NO_WRITE;
        }
        return static_cast<uint32_t>(op_at(static_cast<size_t>(at)).arg >> 48);
    }

    uint64_t last_base()
    {
        int const at = last_index_of(OP_WRITE_TTBR0);
        if (at < 0)
        {
            return 0;
        }
        return op_at(static_cast<size_t>(at)).arg;
    }

    struct arch_aspace* g_hook_space = nullptr;

    // Join after sampling to detect a peer mask computed too late.
    // Leaving would not change residency.
    void peer_joins_the_space()
    {
        set_cpu(PEER_CORE);
        arch_aspace_activate(g_hook_space);
        set_cpu(RUNNING_CORE);
    }

    class MapExec : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            sysops_reset();
            g_hook_space = nullptr;
            space = arch_aspace_create();
            ASSERT_NE(space, nullptr);
            elsewhere = arch_aspace_create();
            ASSERT_NE(elsewhere, nullptr);
            set_cpu(PEER_CORE);
            arch_aspace_activate(space);
            set_cpu(RUNNING_CORE);
            arch_aspace_activate(space);
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

        void seed(uintptr_t va, arch_phys_addr_t pa, uint32_t rights)
        {
            ASSERT_EQ(arch_aspace_map(space, va, pa, 1, rights, ARCH_MAP_NORMAL), ARCH_ASPACE_OK);
            ops_clear();
        }

        struct arch_aspace* space = nullptr;
        struct arch_aspace* elsewhere = nullptr;
    };

    void* frame_pointer(arch_phys_addr_t pa)
    {
        return &__kickos_arm64_va_base[pa];
    }

    // The level-3 descriptor `va` resolves to, walked as the MMU does from the space's root.
    uint64_t leaf_of(struct arch_aspace* space, uintptr_t va)
    {
        uint64_t const* table = reinterpret_cast<uint64_t const*>(space);
        for (unsigned shift = 30; shift > 12; shift -= 9)
        {
            uint64_t const desc = table[(va >> shift) & 0x1FFu];
            if ((desc & 3u) != TABLE)
            {
                return 0;
            }
            table = reinterpret_cast<uint64_t const*>(frame_pointer(desc & 0xFFFFFFFFF000ull));
        }
        return table[(va >> 12) & 0x1FFu];
    }

    // The MAIR_EL1 byte the boot programs at a leaf's AttrIndx.
    uint32_t mair_byte_of(uint64_t leaf)
    {
        return static_cast<uint32_t>((static_cast<uint64_t>(MAIR_VALUE) >> (8u * ((leaf >> 2) & 7u)))
                                     & 0xFFu);
    }

    // The level-1 walk reads VA bits 38:30, so this TTBR1 address indexes the slots of `low`.
    uintptr_t kernel_half_of(uintptr_t low)
    {
        return ~((static_cast<uintptr_t>(1) << 39) - 1u) | low;
    }

    uint64_t* larger_leaf_slot(struct arch_aspace* space, uintptr_t va)
    {
        uint64_t const desc = reinterpret_cast<uint64_t const*>(space)[(va >> 30) & 0x1FFu];
        if ((desc & 3u) != TABLE)
        {
            return nullptr;
        }
        uint64_t* const level2 =
            reinterpret_cast<uint64_t*>(frame_pointer(desc & 0xFFFFFFFFF000ull));
        return &level2[(va >> 21) & 0x1FFu];
    }
}

#define MAP_CONTRACT_FIXTURE MapExecContract
#define MAP_CONTRACT_NOCACHE_SEEN true
#define MAP_CONTRACT_PA_BITS 40u
// A level-2 block over physical 0, inside the seam's window.
#define MAP_CONTRACT_LARGER_LEAF static_cast<uint64_t>(BLOCK | AF)
#include "map_contract.h"

// Executable replacements.

TEST_F(MapExec, MapOverAnExecutableLeafRingsTheInstructionSide)
{
    seed(VA_DEEP, PA_A, RIGHTS_EXEC);

    ASSERT_EQ(arch_aspace_map(space, VA_DEEP, PA_B, 1, RIGHTS_DATA, ARCH_MAP_NORMAL),
              ARCH_ASPACE_OK);

    EXPECT_EQ(ops_with(OP_RENDEZVOUS), 1u);
    int const at = last_index_of(OP_RENDEZVOUS);
    ASSERT_GE(at, 0);
    EXPECT_EQ(op_at(static_cast<size_t>(at)).arg, PEER_MASK);
    EXPECT_EQ(arch_aspace_frame_at(space, VA_DEEP), PA_B);
}

TEST_F(MapExec, TheDebtIsTakenOncePerMapAndNotOncePerInvalidate)
{
    seed(VA_DEEP, PA_A, RIGHTS_EXEC);
    seed(VA_OTHER, PA_B, RIGHTS_EXEC);

    ASSERT_EQ(arch_aspace_map(space, VA_DEEP, PA_C, 1, RIGHTS_DATA, ARCH_MAP_NORMAL),
              ARCH_ASPACE_OK);

    // Two by-address invalidates per replaced page, one rendezvous for the call.
    EXPECT_EQ(ops_with(OP_TLBI_PAGE_IS), 2u);
    EXPECT_EQ(ops_with(OP_RENDEZVOUS), 1u);
}

// Data replacements and new executable mappings.

TEST_F(MapExec, MapOverANonExecutableLeafRingsNothing)
{
    seed(VA_DEEP, PA_A, RIGHTS_DATA);

    ASSERT_EQ(arch_aspace_map(space, VA_DEEP, PA_B, 1, RIGHTS_DATA, ARCH_MAP_NORMAL),
              ARCH_ASPACE_OK);

    EXPECT_EQ(ops_with(OP_TLBI_PAGE_IS), 2u);
    EXPECT_EQ(ops_with(OP_RENDEZVOUS), 0u);
}

TEST_F(MapExec, MapIntoAnInvalidLeafRingsNothing)
{
    ASSERT_EQ(arch_aspace_map(space, VA_DEEP, PA_A, 1, RIGHTS_EXEC, ARCH_MAP_NORMAL),
              ARCH_ASPACE_OK);

    // A new executable mapping has no previously fetched instructions to discard.
    EXPECT_EQ(ops_with(OP_RENDEZVOUS), 0u);
    EXPECT_EQ(ops_with(OP_TLBI_PAGE_IS), 1u);
}

// Memory types.

TEST_F(MapExec, AnUncachedMapsLeafSelectsTheNonCacheableAttribute)
{
    ASSERT_EQ(arch_aspace_map(space, VA_DEEP, PA_A, 1, RIGHTS_DATA, ARCH_MAP_NOCACHE),
              ARCH_ASPACE_OK);
    ASSERT_EQ(arch_aspace_map(space, VA_OTHER, PA_B, 1, RIGHTS_DATA, ARCH_MAP_NORMAL),
              ARCH_ASPACE_OK);

    // Normal inner and outer non-cacheable (DDI 0487 M.b, D24.2.126); 0x00 would be Device.
    EXPECT_EQ(mair_byte_of(leaf_of(space, VA_DEEP)), 0x44u)
        << "the uncached map's leaf names a cacheable or device attribute";
    EXPECT_EQ(mair_byte_of(leaf_of(space, VA_OTHER)), 0xFFu)
        << "the control: an ordinary map's leaf is write-back";
}

// Propagate executable removal across recursive calls.

TEST_F(MapExec, TheDebtSurvivesTheDEEPERRecursionOfTwo)
{
    // Only the second leaf requires instruction synchronization.
    seed(VA_CROSS_LOW, PA_A, RIGHTS_DATA);
    seed(VA_CROSS_HIGH, PA_B, RIGHTS_EXEC);

    ASSERT_EQ(arch_aspace_map(space, VA_CROSS_LOW, PA_C, 2, RIGHTS_DATA, ARCH_MAP_NORMAL),
              ARCH_ASPACE_OK);

    EXPECT_EQ(ops_with(OP_RENDEZVOUS), 1u);
    EXPECT_EQ(ops_with(OP_TLBI_PAGE_IS), 4u);
}

TEST_F(MapExec, TheDebtSurvivesTheEARLIERRecursionOfTwo)
{
    // The second call must preserve the first leaf's synchronization requirement.
    seed(VA_CROSS_LOW, PA_A, RIGHTS_EXEC);
    seed(VA_CROSS_HIGH, PA_B, RIGHTS_DATA);

    ASSERT_EQ(arch_aspace_map(space, VA_CROSS_LOW, PA_C, 2, RIGHTS_DATA, ARCH_MAP_NORMAL),
              ARCH_ASPACE_OK);

    EXPECT_EQ(ops_with(OP_RENDEZVOUS), 1u);
    EXPECT_EQ(ops_with(OP_TLBI_PAGE_IS), 4u);
}

// Publish edits before notifying the previously sampled peers.

TEST_F(MapExec, TheRendezvousFollowsTheBarrierThatPublishesTheEdits)
{
    seed(VA_DEEP, PA_A, RIGHTS_EXEC);

    ASSERT_EQ(arch_aspace_map(space, VA_DEEP, PA_B, 1, RIGHTS_DATA, ARCH_MAP_NORMAL),
              ARCH_ASPACE_OK);

    size_t const n = ops_count();
    ASSERT_GE(n, 2u);
    // Notify only after descriptor stores are published.
    EXPECT_EQ(op_at(n - 1).tag, OP_RENDEZVOUS);
    EXPECT_EQ(op_at(n - 2).tag, OP_DSB_ISHST);
    EXPECT_LT(static_cast<size_t>(last_index_of(OP_TLBI_PAGE_IS)), n - 1);
}

TEST_F(MapExec, ThePeerSetIsSampledBeforeTheEdits)
{
    // The peer joins during the edit.
    struct arch_aspace* const mine = arch_aspace_create();
    ASSERT_NE(mine, nullptr);
    arch_aspace_activate(mine);
    ASSERT_EQ(arch_aspace_map(mine, VA_DEEP, PA_A, 1, RIGHTS_EXEC, ARCH_MAP_NORMAL),
              ARCH_ASPACE_OK);
    g_hook_space = mine;
    arm_mid_edit(peer_joins_the_space);
    ops_clear();

    ASSERT_EQ(arch_aspace_map(mine, VA_DEEP, PA_B, 1, RIGHTS_DATA, ARCH_MAP_NORMAL),
              ARCH_ASPACE_OK);

    int const at = last_index_of(OP_RENDEZVOUS);
    ASSERT_GE(at, 0);
    EXPECT_EQ(op_at(static_cast<size_t>(at)).arg, 0u);
    arch_aspace_destroy(mine);
}

TEST_F(MapExec, ThePeerJoiningIsWhatTheSamplingPointIsReadAgainst)
{
    // Also test a peer already resident when sampling occurs.
    seed(VA_DEEP, PA_A, RIGHTS_EXEC);

    ASSERT_EQ(arch_aspace_map(space, VA_DEEP, PA_B, 1, RIGHTS_DATA, ARCH_MAP_NORMAL),
              ARCH_ASPACE_OK);

    int const at = last_index_of(OP_RENDEZVOUS);
    ASSERT_GE(at, 0);
    EXPECT_EQ(op_at(static_cast<size_t>(at)).arg, PEER_MASK);
}

// Rollback of executable mappings.

TEST_F(MapExec, TheRollbackOfAFailedMapRingsForTheExecutableLeafItRemoves)
{
    // The low leaf fits an existing table; allocating the high leaf's table fails.
    // Rollback must synchronize peers that could have fetched the new low leaf.
    seed(VA_CROSS_BELOW, PA_A, RIGHTS_DATA);
    set_frame_budget(0);

    ASSERT_EQ(arch_aspace_map(space, VA_CROSS_LOW, PA_C, 2, RIGHTS_EXEC, ARCH_MAP_NORMAL),
              ARCH_ASPACE_ENOMEM);

    EXPECT_EQ(arch_aspace_frame_at(space, VA_CROSS_LOW), 0u);
    EXPECT_EQ(arch_aspace_frame_at(space, VA_CROSS_HIGH), 0u);
    EXPECT_EQ(ops_with(OP_RENDEZVOUS), 1u);
    int const at = last_index_of(OP_RENDEZVOUS);
    ASSERT_GE(at, 0);
    EXPECT_EQ(op_at(static_cast<size_t>(at)).arg, PEER_MASK);
    // Notify after rollback invalidations complete.
    EXPECT_EQ(static_cast<size_t>(at), ops_count() - 1u);
}

TEST_F(MapExec, TheRollbackOfANonExecutableMapRingsNothing)
{
    // Data-only rollback requires no instruction synchronization.
    seed(VA_CROSS_BELOW, PA_A, RIGHTS_DATA);
    set_frame_budget(0);

    ASSERT_EQ(arch_aspace_map(space, VA_CROSS_LOW, PA_C, 2, RIGHTS_DATA, ARCH_MAP_NORMAL),
              ARCH_ASPACE_ENOMEM);

    EXPECT_EQ(arch_aspace_frame_at(space, VA_CROSS_LOW), 0u);
    EXPECT_EQ(ops_with(OP_RENDEZVOUS), 0u);
}

// Check executable/data classification through unmap.

TEST_F(MapExec, UnmapOfTheSameLeafRingsToo)
{
    seed(VA_DEEP, PA_A, RIGHTS_EXEC);

    ASSERT_EQ(arch_aspace_unmap(space, VA_DEEP, 1), ARCH_ASPACE_OK);

    EXPECT_EQ(ops_with(OP_RENDEZVOUS), 1u);
}

TEST_F(MapExec, UnmapOfANonExecutableLeafRingsNothing)
{
    seed(VA_DEEP, PA_A, RIGHTS_DATA);

    ASSERT_EQ(arch_aspace_unmap(space, VA_DEEP, 1), ARCH_ASPACE_OK);

    EXPECT_EQ(ops_with(OP_RENDEZVOUS), 0u);
}

TEST_F(MapExec, TheTablesAnUnmapEmptiesGoBackOnlyAfterTheBroadcastSweep)
{
    seed(VA_DEEP, PA_A, RIGHTS_DATA);
    uint32_t const before = frames_freed();

    ASSERT_EQ(arch_aspace_unmap(space, VA_DEEP, 1), ARCH_ASPACE_OK);

    EXPECT_EQ(frames_freed() - before, 2u);
    ASSERT_EQ(ops_with(OP_TLBI_ALL_IS), 1u) << "a detached table's walks were left cached";
    EXPECT_GT(last_index_of(OP_TLBI_ALL_IS), last_index_of(OP_TLBI_PAGE_IS));
    EXPECT_EQ(frames_freed_at_sweep(), before);
}

// Residency-based maintenance.

TEST_F(MapExec, ASpaceEveryCoreHasLeftIsStillMaintained)
{
    seed(VA_DEEP, PA_A, RIGHTS_DATA);
    set_cpu(PEER_CORE);
    arch_aspace_activate(elsewhere);
    set_cpu(RUNNING_CORE);
    arch_aspace_activate(elsewhere);
    ops_clear();

    ASSERT_EQ(arch_aspace_map(space, VA_DEEP, PA_B, 1, RIGHTS_DATA, ARCH_MAP_NORMAL),
              ARCH_ASPACE_OK);

    // Break-before-make: one invalidate for the clear and one for the replacement.
    EXPECT_EQ(ops_with(OP_TLBI_PAGE_IS), 2u);
}

TEST_F(MapExec, ASpaceNoCoreHasRunIsNotMaintained)
{
    // Never-run spaces skip invalidation, as a task's image seed does over many pages.
    constexpr size_t SEED_PAGES = 40;
    struct arch_aspace* const unrun = arch_aspace_create();
    ASSERT_NE(unrun, nullptr);
    ops_clear();

    ASSERT_EQ(arch_aspace_map(unrun, VA_DEEP, PA_A, SEED_PAGES, RIGHTS_DATA, ARCH_MAP_NORMAL),
              ARCH_ASPACE_OK);

    EXPECT_EQ(ops_with(OP_TLBI_PAGE_IS), 0u);
    EXPECT_EQ(ops_with(OP_TLBI_ALL_IS), 0u);
    EXPECT_EQ(ops_with(OP_TLBI_ALL_LOCAL), 0u);
    size_t const g = arch_aspace_granule();
    EXPECT_EQ(arch_aspace_frame_at(unrun, VA_DEEP), PA_A);
    EXPECT_EQ(arch_aspace_frame_at(unrun, VA_DEEP + (SEED_PAGES - 1u) * g),
              PA_A + (SEED_PAGES - 1u) * g);
    arch_aspace_destroy(unrun);
}

TEST_F(MapExec, TheDestroyRendezvousReachesACoreThatHasLeft)
{
    // Peers must invalidate before tables are reclaimed.
    set_cpu(PEER_CORE);
    arch_aspace_activate(elsewhere);
    set_cpu(RUNNING_CORE);
    arch_aspace_activate(elsewhere);
    ops_clear();

    arch_aspace_destroy(space);
    space = nullptr;

    ASSERT_EQ(ops_with(OP_RENDEZVOUS), 1u);
    int const at = last_index_of(OP_RENDEZVOUS);
    ASSERT_GE(at, 0);
    EXPECT_EQ(op_at(static_cast<size_t>(at)).arg, PEER_MASK);
}

TEST_F(MapExec, TheDestroyRendezvousIsEmptyForASpaceNoPeerRan)
{
    // The destination has never been activated.
    ops_clear();

    arch_aspace_destroy(elsewhere);
    elsewhere = nullptr;

    ASSERT_EQ(ops_with(OP_RENDEZVOUS), 1u);
    int const at = last_index_of(OP_RENDEZVOUS);
    ASSERT_GE(at, 0);
    EXPECT_EQ(op_at(static_cast<size_t>(at)).arg, 0u);
}

// ASID allocation, fallback and switch maintenance.

TEST_F(MapExec, ActivateCarriesTheSpacesIdentifierBesideItsRoot)
{
    arch_aspace_activate(elsewhere);

    ASSERT_NE(last_identifier(), NO_WRITE);
    EXPECT_NE(last_identifier(), 0u);
    // Check both root address and ASID fields.
    EXPECT_EQ(last_base(),
              root_pa(elsewhere) | (static_cast<uint64_t>(last_identifier()) << 48));
}

TEST_F(MapExec, TwoLiveSpacesAreGivenDifferentIdentifiers)
{
    arch_aspace_activate(space);
    uint32_t const first = last_identifier();
    arch_aspace_activate(elsewhere);
    uint32_t const second = last_identifier();

    ASSERT_NE(first, NO_WRITE);
    ASSERT_NE(second, NO_WRITE);
    EXPECT_NE(first, 0u);
    EXPECT_NE(second, 0u);
    EXPECT_NE(first, second);
}

TEST_F(MapExec, TheIdentifierBelongsToTheSpaceAndNotToTheSwitch)
{
    arch_aspace_activate(space);
    uint32_t const first = last_identifier();
    arch_aspace_activate(elsewhere);
    arch_aspace_activate(space);

    ASSERT_NE(first, NO_WRITE);
    EXPECT_EQ(last_identifier(), first);
}

TEST_F(MapExec, ADestroyedSpacesIdentifierIsHandedOutAgain)
{
    struct arch_aspace* const first_space = arch_aspace_create();
    ASSERT_NE(first_space, nullptr);
    arch_aspace_activate(first_space);
    uint32_t const freed = last_identifier();
    ASSERT_NE(freed, NO_WRITE);
    ASSERT_NE(freed, 0u);
    // Destroy must invalidate before releasing the ID.
    arch_aspace_destroy(first_space);

    struct arch_aspace* const second_space = arch_aspace_create();
    ASSERT_NE(second_space, nullptr);
    arch_aspace_activate(second_space);

    EXPECT_EQ(last_identifier(), freed);
    arch_aspace_destroy(second_space);
}

TEST_F(MapExec, LeavingARootThatCarriesAnIdentifierSweepsNothing)
{
    ops_clear();

    // Distinct tagged roots require no switch invalidation.
    arch_aspace_activate(elsewhere);

    EXPECT_EQ(ops_with(OP_TLBI_ALL_LOCAL), 0u);
    EXPECT_EQ(ops_with(OP_TLBI_ALL_IS), 0u);
    EXPECT_EQ(ops_with(OP_WRITE_TTBR0), 1u);
}

TEST_F(MapExec, LeavingARootWithNoIdentifierSweepsTheWholeLowHalf)
{
    // Leaving ASID 0 must also remove the boot root's global entries.
    set_mmfr0(mmfr0_with_asid_bits(1)); // reserved, so the port reads no width and tags nothing
    struct arch_aspace* const plain = arch_aspace_create();
    ASSERT_NE(plain, nullptr);
    set_mmfr0(mmfr0_with_asid_bits(2));
    arch_aspace_activate(plain);
    ASSERT_EQ(last_identifier(), 0u);
    ops_clear();

    arch_aspace_activate(space);

    EXPECT_EQ(ops_with(OP_TLBI_ALL_LOCAL), 1u);
    arch_aspace_destroy(plain);
}

TEST_F(MapExec, ASpaceBeyondTheRecordsRowsRunsUntaggedAndKeepsItsSweep)
{
    // Fill all rows to test the untracked-root fallback. SetUp already uses two.
    constexpr size_t ROWS_LEFT = KICKOS_MAX_DOMAINS - 2;
    // Each root uses one of the 64 frame slots.
    static_assert(KICKOS_MAX_DOMAINS + 2 < 60, "the seam's frame pool cannot fill this record");
    struct arch_aspace* filler[ROWS_LEFT] = {};
    for (size_t i = 0; i < ROWS_LEFT; i++)
    {
        filler[i] = arch_aspace_create();
        ASSERT_NE(filler[i], nullptr);
        arch_aspace_activate(filler[i]);
        ASSERT_NE(last_identifier(), NO_WRITE);
        EXPECT_NE(last_identifier(), 0u) << "row " << i << " of the record went unassigned";
    }

    struct arch_aspace* const rowless = arch_aspace_create();
    ASSERT_NE(rowless, nullptr);
    arch_aspace_activate(rowless);
    EXPECT_EQ(last_identifier(), 0u);

    ops_clear();
    arch_aspace_activate(space);
    EXPECT_EQ(ops_with(OP_TLBI_ALL_LOCAL), 1u);

    arch_aspace_destroy(rowless);
    for (size_t i = 0; i < ROWS_LEFT; i++)
    {
        arch_aspace_destroy(filler[i]);
    }
}

TEST_F(MapExec, AReservedIdentifierWidthTagsNothingAndLeavesEverySwitchSweeping)
{
    // Unknown ASID width disables tagging.
    set_mmfr0(mmfr0_with_asid_bits(1));

    struct arch_aspace* const a = arch_aspace_create();
    struct arch_aspace* const b = arch_aspace_create();
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    arch_aspace_activate(a);
    EXPECT_EQ(last_identifier(), 0u);
    ops_clear();
    arch_aspace_activate(b);
    EXPECT_EQ(last_identifier(), 0u);
    EXPECT_EQ(ops_with(OP_TLBI_ALL_LOCAL), 1u);

    arch_aspace_destroy(a);
    arch_aspace_destroy(b);
}

TEST_F(MapExec, AnEightBitFieldStillTagsBecauseTheRowCountIsTheNarrowerBound)
{
    // An 8-bit field is sufficient for this test configuration's rows.
    set_tcr(tcr_a53_without_as());

    struct arch_aspace* const narrow = arch_aspace_create();
    ASSERT_NE(narrow, nullptr);
    arch_aspace_activate(narrow);

    ASSERT_NE(last_identifier(), NO_WRITE);
    EXPECT_NE(last_identifier(), 0u);
    EXPECT_LT(last_identifier(), 1u << 8);

    set_tcr(tcr_a53());
    arch_aspace_destroy(narrow);
}

// The reserved-root form, on a core without PAN: EL1 stays on the empty root, so activation
// fills the core's cell and the sweep rule reads the cell.

// As kickos_armv8a_uguard_init leaves a core without PAN: every cell seeded with the empty root.
void enter_reserved_root()
{
    set_root_deferred(true);
    for (uint32_t c = 0; c < KICKOS_NUM_CORES; c++)
    {
        set_user_ttbr0(c, kickos_armv8a_empty_ttbr());
    }
}

TEST_F(MapExec, UnderTheReservedRootActivationFillsTheCellAndWritesNoRegister)
{
    enter_reserved_root();
    ops_clear();

    arch_aspace_activate(elsewhere);

    EXPECT_EQ(ops_with(OP_WRITE_TTBR0), 0u);
    EXPECT_EQ(user_ttbr0_of(RUNNING_CORE) & 0x0000FFFFFFFFF000ull, root_pa(elsewhere));
    EXPECT_NE(user_ttbr0_of(RUNNING_CORE) >> 48, 0u);
    EXPECT_EQ(user_ttbr0_of(PEER_CORE), kickos_armv8a_empty_ttbr());
}

TEST_F(MapExec, UnderTheReservedRootTheFirstActivationLeavesTheSeedAndOwesNoSweep)
{
    enter_reserved_root();
    ops_clear();

    arch_aspace_activate(space);

    EXPECT_EQ(ops_with(OP_TLBI_ALL_LOCAL), 0u);
}

TEST_F(MapExec, UnderTheReservedRootLeavingACellWithNoIdentifierSweeps)
{
    enter_reserved_root();
    set_mmfr0(mmfr0_with_asid_bits(1));
    struct arch_aspace* const plain = arch_aspace_create();
    ASSERT_NE(plain, nullptr);
    set_mmfr0(mmfr0_with_asid_bits(2));
    arch_aspace_activate(plain);
    ASSERT_EQ(user_ttbr0_of(RUNNING_CORE) >> 48, 0u);
    ops_clear();

    arch_aspace_activate(space);
    EXPECT_EQ(ops_with(OP_TLBI_ALL_LOCAL), 1u);

    ops_clear();
    arch_aspace_activate(elsewhere);
    EXPECT_EQ(ops_with(OP_TLBI_ALL_LOCAL), 0u);
    arch_aspace_destroy(plain);
}

// The empty root, which a core installs once no space is its own.

TEST_F(MapExec, DestroyingTheEmptyRootIsRefusedBeforeItReachesThePool)
{
    uint32_t const freed = frames_freed();
    ops_clear();

    arch_aspace_destroy(arch_aspace_boot());

    EXPECT_EQ(frames_freed(), freed);
    EXPECT_EQ(ops_with(OP_TLBI_ALL_IS), 0u);
    EXPECT_EQ(ops_with(OP_RENDEZVOUS), 0u);
    EXPECT_EQ(ops_count(), 0u);

    // Ordinary roots must still be reclaimed.
    struct arch_aspace* const ordinary = arch_aspace_create();
    ASSERT_NE(ordinary, nullptr);
    uint32_t const before = frames_freed();
    ops_clear();

    arch_aspace_destroy(ordinary);

    EXPECT_GT(frames_freed(), before);
    EXPECT_EQ(ops_with(OP_TLBI_ALL_IS), 1u);
}

TEST_F(MapExec, TheEmptyRootIsOneHandleThatNothingMapsInto)
{
    struct arch_aspace* const empty = arch_aspace_boot();

    set_cpu(PEER_CORE);
    arch_aspace_activate(elsewhere);
    set_cpu(RUNNING_CORE);

    EXPECT_EQ(arch_aspace_boot(), empty);
    EXPECT_NE(empty, space);
    EXPECT_NE(empty, elsewhere);
    EXPECT_EQ(arch_aspace_map(empty, VA_DEEP, PA_A, 1, RIGHTS_DATA, ARCH_MAP_NORMAL),
              ARCH_ASPACE_EINVAL);
    EXPECT_EQ(arch_aspace_acquire(empty, VA_DEEP, nullptr, nullptr), nullptr);
}

TEST_F(MapExec, TheEmptyRootRunsUnderTheTopIdentifierAndLeavingItSweepsNothing)
{
    arch_aspace_activate(arch_aspace_boot());
    EXPECT_EQ(last_identifier(), 0xFFFFu);
    ops_clear();

    arch_aspace_activate(space);

    EXPECT_EQ(ops_with(OP_TLBI_ALL_LOCAL), 0u);
    EXPECT_EQ(ops_with(OP_WRITE_TTBR0), 1u);
}

TEST_F(MapExec, AnEightBitFieldGivesTheEmptyRootTheTopEightBitIdentifier)
{
    set_tcr(tcr_a53_without_as());

    arch_aspace_activate(arch_aspace_boot());
    EXPECT_EQ(last_identifier(), 0xFFu);

    set_tcr(tcr_a53());
}

// What the boot core refuses to drive.

namespace kickos
{
    namespace armv8a
    {
        char const* mmu_refusal(uint64_t mmfr0, uint64_t tcr);
    }
}

namespace
{
    constexpr uint64_t TGRAN4_ABSENT = 0xFull << 28;
    constexpr uint64_t PARANGE_MASK = 0xFull;
    constexpr uint64_t PARANGE_36 = 1u;
    constexpr uint64_t PARANGE_RESERVED = 0xEu;
}

TEST(MmuRefusal, AnA53IsDriven)
{
    EXPECT_EQ(kickos::armv8a::mmu_refusal(mmfr0_with_asid_bits(2), tcr_a53()), nullptr);
}

TEST(MmuRefusal, AnMmuWithoutThe4KiBGranuleIsRefused)
{
    EXPECT_STREQ(kickos::armv8a::mmu_refusal(mmfr0_with_asid_bits(2) | TGRAN4_ABSENT, tcr_a53()),
                 kickos::diag::kMmuNoGranule);
}

TEST(MmuRefusal, APhysicalRangeNarrowerThanTheProgrammedOneIsRefused)
{
    uint64_t const a53 = mmfr0_with_asid_bits(2);
    EXPECT_STREQ(kickos::armv8a::mmu_refusal((a53 & ~PARANGE_MASK) | PARANGE_36, tcr_a53()),
                 kickos::diag::kMmuPaNarrow);
    EXPECT_STREQ(kickos::armv8a::mmu_refusal((a53 & ~PARANGE_MASK) | PARANGE_RESERVED,
                                             tcr_a53()),
                 kickos::diag::kMmuPaNarrow);
}

extern "C" void kickos_armv8a_mmu_check(void);

TEST(MmuRefusal, TheCoreCheckRefusesAnMmuItCannotDrive)
{
    sysops_reset();
    kickos_armv8a_mmu_check();
    EXPECT_DEATH(
        {
            set_mmfr0(mmfr0_with_asid_bits(2) | TGRAN4_ABSENT);
            kickos_armv8a_mmu_check();
        },
        std::string("KERNEL PANIC: ") + kickos::diag::kMmuNoGranule);
}

// Which form keeps a core's EL1 out of user pages, and the agreement every core owes.

using kickos::armv8a::UGuard;

TEST(UGuard, TheReservedRootIsRefusedWithoutAnIdentifier)
{
    EXPECT_STREQ(kickos::armv8a::uguard_root_refusal(0x4001b000ull), kickos::diag::kUguardNoAsid);
    EXPECT_EQ(kickos::armv8a::uguard_root_refusal(0xFFFF00004001b000ull), nullptr);
    EXPECT_EQ(kickos::armv8a::uguard_root_refusal(0x00FF00004001b000ull), nullptr);
}

TEST(UGuard, AnMmuWithNoIdentifierWidthGivesTheEmptyRootNoneAndIsRefused)
{
    sysops_reset();
    set_mmfr0(mmfr0_with_asid_bits(1)); // reserved: the port reads no width
    EXPECT_STREQ(kickos::armv8a::uguard_root_refusal(kickos_armv8a_empty_ttbr()),
                 kickos::diag::kUguardNoAsid);
    set_mmfr0(mmfr0_with_asid_bits(2));
    EXPECT_EQ(kickos::armv8a::uguard_root_refusal(kickos_armv8a_empty_ttbr()), nullptr);
}

TEST(UGuard, ACoreWithNoPanFieldTakesTheReservedRoot)
{
    EXPECT_EQ(kickos::armv8a::uguard_of(0), UGuard::ROOT);
    // The fields either side of ID_AA64MMFR1_EL1.PAN [23:20] say nothing about it.
    EXPECT_EQ(kickos::armv8a::uguard_of(0xFull << 16), UGuard::ROOT);
    EXPECT_EQ(kickos::armv8a::uguard_of(0xFull << 24), UGuard::ROOT);
}

TEST(UGuard, EveryPanLevelTakesPan)
{
    EXPECT_EQ(kickos::armv8a::uguard_of(1ull << 20), UGuard::PAN);
    EXPECT_EQ(kickos::armv8a::uguard_of(2ull << 20), UGuard::PAN);
    EXPECT_EQ(kickos::armv8a::uguard_of(3ull << 20), UGuard::PAN);
}

TEST(UGuard, TheBootCoreChoosesAndALaterCoreMustAgree)
{
    EXPECT_EQ(kickos::armv8a::uguard_refusal(UGuard::UNSET, UGuard::PAN), nullptr);
    EXPECT_EQ(kickos::armv8a::uguard_refusal(UGuard::UNSET, UGuard::ROOT), nullptr);
    EXPECT_EQ(kickos::armv8a::uguard_refusal(UGuard::PAN, UGuard::PAN), nullptr);
    EXPECT_EQ(kickos::armv8a::uguard_refusal(UGuard::ROOT, UGuard::ROOT), nullptr);
    EXPECT_STREQ(kickos::armv8a::uguard_refusal(UGuard::PAN, UGuard::ROOT),
                 kickos::diag::kUguardMixed);
    EXPECT_STREQ(kickos::armv8a::uguard_refusal(UGuard::ROOT, UGuard::PAN),
                 kickos::diag::kUguardMixed);
}
