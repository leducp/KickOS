// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Check ARM64 mapping maintenance, residency and ASID allocation through
// recorded system operations. Build all test mappings through the backend.
// These tests do not model a hardware TLB.

#include <kickos/arch/arch.h>

#include <gtest/gtest.h>

#include "aspace_sysops_seam.h"

using namespace kickos::testfix;

// The seam's physical window (aspace_sysops_seam.cc).
extern "C" unsigned char __kickos_arm64_va_base[];

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

    uint32_t model_asid_bits()
    {
        return static_cast<uint32_t>((arch_aspace_model() >> ARCH_ASPACE_MODEL_ASID_SHIFT) &
                                     ARCH_ASPACE_MODEL_FIELD_MASK);
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
}

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

// Partial-map refusal.

TEST_F(MapExec, APartiallyMappedRangeIsRefusedBeforeAnyEdit)
{
    // A partial remap must preserve the low leaf even if allocation would fail later.
    seed(VA_CROSS_LOW, PA_A, RIGHTS_DATA);
    set_frame_budget(0);

    // Continue checking the post-state even if refusal fails.
    EXPECT_EQ(arch_aspace_map(space, VA_CROSS_LOW, PA_C, 2, RIGHTS_DATA, ARCH_MAP_NORMAL),
              ARCH_ASPACE_EINVAL);

    EXPECT_EQ(arch_aspace_frame_at(space, VA_CROSS_LOW), PA_A);
    EXPECT_EQ(arch_aspace_frame_at(space, VA_CROSS_HIGH), 0u);
    EXPECT_EQ(ops_count(), 0u);
}

TEST_F(MapExec, AWhollyMappedRangeIsStillARemapAndPasses)
{
    // Wholly mapped ranges remain valid remap targets.
    seed(VA_CROSS_LOW, PA_A, RIGHTS_DATA);
    seed(VA_CROSS_HIGH, PA_B, RIGHTS_DATA);

    ASSERT_EQ(arch_aspace_map(space, VA_CROSS_LOW, PA_C, 2, RIGHTS_DATA, ARCH_MAP_NORMAL),
              ARCH_ASPACE_OK);

    arch_phys_addr_t const g = static_cast<arch_phys_addr_t>(arch_aspace_granule());
    EXPECT_EQ(arch_aspace_frame_at(space, VA_CROSS_LOW), PA_C);
    EXPECT_EQ(arch_aspace_frame_at(space, VA_CROSS_HIGH), PA_C + g);
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
    // Never-run spaces skip invalidation.
    struct arch_aspace* const unrun = arch_aspace_create();
    ASSERT_NE(unrun, nullptr);
    ops_clear();

    ASSERT_EQ(arch_aspace_map(unrun, VA_DEEP, PA_A, 1, RIGHTS_DATA, ARCH_MAP_NORMAL),
              ARCH_ASPACE_OK);

    EXPECT_EQ(ops_with(OP_TLBI_PAGE_IS), 0u);
    EXPECT_EQ(arch_aspace_frame_at(unrun, VA_DEEP), PA_A);
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

    EXPECT_EQ(arch_aspace_model() & ARCH_ASPACE_MODEL_TAGGED, 0u);
    EXPECT_EQ(model_asid_bits(), 0u);

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
    EXPECT_NE(arch_aspace_model() & ARCH_ASPACE_MODEL_TAGGED, 0u);
    EXPECT_EQ(arch_aspace_model() & ARCH_ASPACE_MODEL_ASID, 0u);
    EXPECT_EQ(model_asid_bits(), 8u);

    set_tcr(tcr_a53());
    arch_aspace_destroy(narrow);
}

TEST_F(MapExec, TheWidthIsReadBackFromTheControlRegisterAndNotFromTheMachineAlone)
{
    // Keep ASIDBits at 16 and vary TCR.AS to check the effective width.
    EXPECT_EQ(model_asid_bits(), 16u);
    EXPECT_NE(arch_aspace_model() & ARCH_ASPACE_MODEL_ASID, 0u);

    set_tcr(tcr_a53_without_as());
    EXPECT_EQ(model_asid_bits(), 8u);
    EXPECT_EQ(arch_aspace_model() & ARCH_ASPACE_MODEL_ASID, 0u);

    set_tcr(tcr_a53());
    EXPECT_EQ(model_asid_bits(), 16u);
}

// Boot-root protection.

TEST_F(MapExec, DestroyingTheBootRootIsRefusedBeforeItReachesThePool)
{
    // The seam captures frame zero as the boot root. Check that destroy refuses
    // the handle returned by arch_aspace_boot without freeing any frame.
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

TEST_F(MapExec, TheBootRootIsLatchedOnTheFirstCaptureAndNotByALaterActivate)
{
    // The initially zero boot TTBR0 must stay latched after installing user roots.
    struct arch_aspace* const boot = arch_aspace_boot();

    set_cpu(PEER_CORE);
    arch_aspace_activate(elsewhere);
    set_cpu(RUNNING_CORE);

    EXPECT_EQ(arch_aspace_boot(), boot);
    EXPECT_NE(arch_aspace_boot(), space);
    EXPECT_NE(arch_aspace_boot(), elsewhere);
}
