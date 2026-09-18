// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Check RV64 maintenance operands, residency, ASIDs and first-use fences.
// Build mappings through the backend. System operations are recorded;
// these tests do not model a hardware TLB.

#include <kickos/arch/arch.h>

#include <gtest/gtest.h>

#include "rv64_sysops_seam.h"

using namespace kickos::testfix;

namespace
{
    // Peer synchronization requires the multicore code paths.
    static_assert(KICKOS_KERNEL_CORES > 1,
                  "map_fence must compile the multicore arm of resident_peers and "
                  "translation_rendezvous");
    static_assert(KICKOS_NUM_CORES > 1, "the peer set needs a hart other than this one");

    constexpr uint32_t RUNNING_CORE = 0;
    constexpr uint32_t PEER_CORE = 1;
    constexpr uint32_t PEER_MASK = 1u << PEER_CORE;

    // Sv39 uses three levels and 4 KiB pages.
    constexpr uintptr_t VA_DEEP = 0x40201000;  // root index 1, level-1 index 1, leaf index 1
    constexpr uintptr_t VA_OTHER = 0x40203000; // the same leaf table, a different slot
    // Cross a table boundary to check distinct invalidation operands.
    constexpr uintptr_t VA_CROSS_LOW = 0x401FF000;
    constexpr uintptr_t VA_CROSS_HIGH = 0x40200000;

    // Frames outside the pool exercise transient acquisition and are not freed.
    constexpr arch_phys_addr_t PA_A = 0x20000000;
    constexpr arch_phys_addr_t PA_B = 0x20001000;
    constexpr arch_phys_addr_t PA_C = 0x20002000;

    constexpr uint32_t RIGHTS_DATA = ARCH_MAP_R | ARCH_MAP_W;

    // satp at XLEN 64: MODE 63:60, ASID 59:44, PPN 43:0.
    constexpr unsigned SATP_MODE_SHIFT = 60;
    constexpr unsigned SATP_ASID_SHIFT = 44;
    constexpr uint64_t SATP_ASID_FIELD = 0xFFFFull;
    constexpr uint64_t SATP_PPN_MASK = (1ull << SATP_ASID_SHIFT) - 1u;
    constexpr unsigned GRANULE_SHIFT = 12;

    // Distinguish no register write from ASID 0.
    constexpr uint32_t NO_WRITE = 0xFFFFFFFFu;

    uint64_t last_satp()
    {
        int const at = last_index_of(OP_WRITE_SATP);
        if (at < 0)
        {
            return 0;
        }
        return op_at(static_cast<size_t>(at)).arg;
    }

    uint32_t last_identifier()
    {
        int const at = last_index_of(OP_WRITE_SATP);
        if (at < 0)
        {
            return NO_WRITE;
        }
        return static_cast<uint32_t>((op_at(static_cast<size_t>(at)).arg >> SATP_ASID_SHIFT)
                                     & SATP_ASID_FIELD);
    }

    // Physical addresses are offsets into the host window array.
    uint64_t root_ppn(struct arch_aspace* sp)
    {
        uintptr_t const pa = reinterpret_cast<uintptr_t>(sp) - window_delta();
        return static_cast<uint64_t>(pa) >> GRANULE_SHIFT;
    }

    struct arch_aspace* g_hook_space = nullptr;

    // Join during the edit to detect a peer mask sampled too late.
    void peer_joins_the_space()
    {
        set_cpu(PEER_CORE);
        arch_aspace_activate(g_hook_space);
        set_cpu(RUNNING_CORE);
    }

    class MapFence : public ::testing::Test
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

        void seed(uintptr_t va, arch_phys_addr_t pa, uint32_t rights)
        {
            ASSERT_EQ(arch_aspace_map(space, va, pa, 1, rights, ARCH_MAP_NORMAL), ARCH_ASPACE_OK);
            ops_clear();
        }

        // Release existing rows before changing ASID width; reset does not clear
        // the backend's residency table or allocated identifiers.
        void remachine(unsigned asid_bits)
        {
            arch_aspace_destroy(space);
            arch_aspace_destroy(elsewhere);
            space = nullptr;
            elsewhere = nullptr;
            sysops_reset(asid_bits);
            ops_clear();
        }

        // Switch away without clearing residency.
        void both_harts_leave()
        {
            set_cpu(PEER_CORE);
            arch_aspace_activate(elsewhere);
            set_cpu(RUNNING_CORE);
            arch_aspace_activate(elsewhere);
            ops_clear();
        }

        struct arch_aspace* space = nullptr;
        struct arch_aspace* elsewhere = nullptr;
    };
}

// Never-run spaces skip invalidation.

TEST_F(MapFence, ASpaceNoHartHasRunPaysNoMaintenanceAtAll)
{
    struct arch_aspace* const unrun = arch_aspace_create();
    ASSERT_NE(unrun, nullptr);
    uint32_t const before = frames_allocated();
    ops_clear();

    ASSERT_EQ(arch_aspace_map(unrun, VA_DEEP, PA_A, 1, RIGHTS_DATA, ARCH_MAP_NORMAL),
              ARCH_ASPACE_OK);

    EXPECT_EQ(ops_with(OP_SFENCE_PAGE), 0u);
    EXPECT_EQ(ops_with(OP_SFENCE_ALL), 0u);
    EXPECT_EQ(ops_with(OP_RENDEZVOUS), 0u);
    EXPECT_EQ(arch_aspace_frame_at(unrun, VA_DEEP), PA_A);
    EXPECT_EQ(frames_allocated() - before, 2u);
}

TEST_F(MapFence, ASpaceEveryHartHasLeftIsStillMaintained)
{
    seed(VA_DEEP, PA_A, RIGHTS_DATA);
    both_harts_leave();

    ASSERT_EQ(arch_aspace_map(space, VA_DEEP, PA_B, 1, RIGHTS_DATA, ARCH_MAP_NORMAL),
              ARCH_ASPACE_OK);

    ASSERT_EQ(ops_with(OP_SFENCE_PAGE), 2u);
    EXPECT_EQ(arg_of_nth(OP_SFENCE_PAGE, 0), VA_DEEP);
    EXPECT_EQ(arg_of_nth(OP_SFENCE_PAGE, 1), VA_DEEP);
    EXPECT_EQ(arch_aspace_frame_at(space, VA_DEEP), PA_B);
}

TEST_F(MapFence, NoSatpNamesTheSpaceTheEditorStillMaintains)
{
    // Current-root state must differ from retained residency here.
    seed(VA_DEEP, PA_A, RIGHTS_DATA);
    both_harts_leave();

    EXPECT_EQ(arch_aspace_active_cores(space), 0u);

    ASSERT_EQ(arch_aspace_map(space, VA_DEEP, PA_B, 1, RIGHTS_DATA, ARCH_MAP_NORMAL),
              ARCH_ASPACE_OK);

    EXPECT_EQ(ops_with(OP_SFENCE_PAGE), 2u);
    int const at = last_index_of(OP_RENDEZVOUS);
    ASSERT_GE(at, 0);
    EXPECT_EQ(op_at(static_cast<size_t>(at)).arg, PEER_MASK);
}

TEST_F(MapFence, EachPageOfARunIsFencedOverItsOwnAddress)
{
    // Check addresses as well as fence counts.
    seed(VA_CROSS_LOW, PA_A, RIGHTS_DATA);
    seed(VA_CROSS_HIGH, PA_B, RIGHTS_DATA);
    both_harts_leave();

    ASSERT_EQ(arch_aspace_map(space, VA_CROSS_LOW, PA_C, 2, RIGHTS_DATA, ARCH_MAP_NORMAL),
              ARCH_ASPACE_OK);

    ASSERT_EQ(ops_with(OP_SFENCE_PAGE), 4u);
    EXPECT_EQ(arg_of_nth(OP_SFENCE_PAGE, 0), VA_CROSS_LOW);
    EXPECT_EQ(arg_of_nth(OP_SFENCE_PAGE, 1), VA_CROSS_LOW);
    EXPECT_EQ(arg_of_nth(OP_SFENCE_PAGE, 2), VA_CROSS_HIGH);
    EXPECT_EQ(arg_of_nth(OP_SFENCE_PAGE, 3), VA_CROSS_HIGH);
}

// New non-leaf entries require a whole-hart fence; invalid PTEs may be cached.

TEST_F(MapFence, AFreshTableInAMaintainedSpaceFencesTheWholeHart)
{
    // Expect two non-leaf fences and one leaf fence.
    ASSERT_EQ(arch_aspace_map(space, VA_DEEP, PA_A, 1, RIGHTS_DATA, ARCH_MAP_NORMAL),
              ARCH_ASPACE_OK);

    EXPECT_EQ(ops_with(OP_SFENCE_ALL), 2u);
    ASSERT_EQ(ops_with(OP_SFENCE_PAGE), 1u);
    EXPECT_EQ(arg_of_nth(OP_SFENCE_PAGE, 0), VA_DEEP);
}

TEST_F(MapFence, ASecondPageUnderAStandingTableFencesNoWholeHart)
{
    // Existing tables require no non-leaf fence.
    seed(VA_DEEP, PA_A, RIGHTS_DATA);

    ASSERT_EQ(arch_aspace_map(space, VA_OTHER, PA_B, 1, RIGHTS_DATA, ARCH_MAP_NORMAL),
              ARCH_ASPACE_OK);

    EXPECT_EQ(ops_with(OP_SFENCE_ALL), 0u);
    ASSERT_EQ(ops_with(OP_SFENCE_PAGE), 1u);
    EXPECT_EQ(arg_of_nth(OP_SFENCE_PAGE, 0), VA_OTHER);
}

TEST_F(MapFence, AFreshTableInASpaceNoHartHasRunFencesNothing)
{
    struct arch_aspace* const unrun = arch_aspace_create();
    ASSERT_NE(unrun, nullptr);
    uint32_t const before = frames_allocated();
    ops_clear();

    ASSERT_EQ(arch_aspace_map(unrun, VA_DEEP, PA_A, 1, RIGHTS_DATA, ARCH_MAP_NORMAL),
              ARCH_ASPACE_OK);

    EXPECT_EQ(ops_with(OP_SFENCE_ALL), 0u);
    EXPECT_EQ(frames_allocated() - before, 2u);
}

// Destroy synchronizes resident peers before freeing tables.

TEST_F(MapFence, TheDestroyDoorbellReachesAHartThatHasLeft)
{
    seed(VA_DEEP, PA_A, RIGHTS_DATA);
    both_harts_leave();

    arch_aspace_destroy(space);
    space = nullptr;

    ASSERT_EQ(ops_with(OP_RENDEZVOUS), 1u);
    int const at = last_index_of(OP_RENDEZVOUS);
    ASSERT_GE(at, 0);
    EXPECT_EQ(op_at(static_cast<size_t>(at)).arg, PEER_MASK);
}

TEST_F(MapFence, TheDestroyDoorbellIsEmptyForASpaceNoHartRan)
{
    // This space has never been activated.
    struct arch_aspace* const fresh = arch_aspace_create();
    ASSERT_NE(fresh, nullptr);
    ASSERT_EQ(arch_aspace_map(fresh, VA_DEEP, PA_A, 1, RIGHTS_DATA, ARCH_MAP_NORMAL),
              ARCH_ASPACE_OK);
    ops_clear();

    arch_aspace_destroy(fresh);

    ASSERT_EQ(ops_with(OP_RENDEZVOUS), 1u);
    int const at = last_index_of(OP_RENDEZVOUS);
    ASSERT_GE(at, 0);
    EXPECT_EQ(op_at(static_cast<size_t>(at)).arg, 0u);
    EXPECT_EQ(ops_with(OP_SFENCE_ALL), 1u);
}

TEST_F(MapFence, TheDestroyDoorbellRingsBeforeAnyFrameGoesBack)
{
    seed(VA_DEEP, PA_A, RIGHTS_DATA);
    both_harts_leave();
    uint32_t const before = frames_freed();

    arch_aspace_destroy(space);
    space = nullptr;

    // Check the free count at notification time; frees are not in the operation trace.
    EXPECT_EQ(frames_freed_at_rendezvous(), before);
    EXPECT_GT(frames_freed(), before);
}

// Unmap maintenance.

TEST_F(MapFence, UnmapOfASpaceAHartHasLeftFencesEachPageAndRings)
{
    seed(VA_DEEP, PA_A, RIGHTS_DATA);
    both_harts_leave();

    ASSERT_EQ(arch_aspace_unmap(space, VA_DEEP, 1), ARCH_ASPACE_OK);

    ASSERT_EQ(ops_with(OP_SFENCE_PAGE), 1u);
    EXPECT_EQ(arg_of_nth(OP_SFENCE_PAGE, 0), VA_DEEP);
    ASSERT_EQ(ops_with(OP_RENDEZVOUS), 1u);
    EXPECT_EQ(arg_of_nth(OP_RENDEZVOUS, 0), PEER_MASK);
    EXPECT_EQ(arch_aspace_frame_at(space, VA_DEEP), 0u);
}

TEST_F(MapFence, UnmapOfASpaceNoHartHasRunFencesNothingAndStillRings)
{
    // Unmap still calls the rendezvous with an empty mask.
    struct arch_aspace* const unrun = arch_aspace_create();
    ASSERT_NE(unrun, nullptr);
    ASSERT_EQ(arch_aspace_map(unrun, VA_DEEP, PA_A, 1, RIGHTS_DATA, ARCH_MAP_NORMAL),
              ARCH_ASPACE_OK);
    ops_clear();

    ASSERT_EQ(arch_aspace_unmap(unrun, VA_DEEP, 1), ARCH_ASPACE_OK);

    EXPECT_EQ(ops_with(OP_SFENCE_PAGE), 0u);
    ASSERT_EQ(ops_with(OP_RENDEZVOUS), 1u);
    EXPECT_EQ(arg_of_nth(OP_RENDEZVOUS, 0), 0u);
    EXPECT_EQ(arch_aspace_frame_at(unrun, VA_DEEP), 0u);
}

// Sample peers before edits.

TEST_F(MapFence, ThePeerSetIsSampledBeforeTheEdits)
{
    // Only the initiating hart is resident until the hook runs.
    struct arch_aspace* const mine = arch_aspace_create();
    ASSERT_NE(mine, nullptr);
    arch_aspace_activate(mine);
    ASSERT_EQ(arch_aspace_map(mine, VA_DEEP, PA_A, 1, RIGHTS_DATA, ARCH_MAP_NORMAL),
              ARCH_ASPACE_OK);
    g_hook_space = mine;
    arm_mid_edit(peer_joins_the_space);
    ops_clear();

    ASSERT_EQ(arch_aspace_map(mine, VA_DEEP, PA_B, 1, RIGHTS_DATA, ARCH_MAP_NORMAL),
              ARCH_ASPACE_OK);

    int const at = last_index_of(OP_RENDEZVOUS);
    ASSERT_GE(at, 0);
    EXPECT_EQ(op_at(static_cast<size_t>(at)).arg, 0u);
}

TEST_F(MapFence, ThePeerJoiningIsWhatTheSamplingPointIsReadAgainst)
{
    // Also test a peer already resident before sampling.
    seed(VA_DEEP, PA_A, RIGHTS_DATA);

    ASSERT_EQ(arch_aspace_map(space, VA_DEEP, PA_B, 1, RIGHTS_DATA, ARCH_MAP_NORMAL),
              ARCH_ASPACE_OK);

    int const at = last_index_of(OP_RENDEZVOUS);
    ASSERT_GE(at, 0);
    EXPECT_EQ(op_at(static_cast<size_t>(at)).arg, PEER_MASK);
}

// Direct and transient acquisition.

TEST_F(MapFence, AcquireOfAFrameOutsideTheKernelWindowSpendsASlot)
{
    seed(VA_DEEP, PA_A, RIGHTS_DATA);

    // Do not dereference the simulated slot address.
    void* const p = arch_aspace_acquire(space, VA_DEEP + 0x40);
    ASSERT_NE(p, nullptr);

    uintptr_t const slot0 =
        window_va() + RUNNING_CORE * ARCH_ASPACE_ACQUIRE_MIN * arch_aspace_granule();
    EXPECT_EQ(reinterpret_cast<uintptr_t>(p), slot0 + 0x40);
    ASSERT_EQ(ops_with(OP_SFENCE_PAGE), 1u);
    EXPECT_EQ(arg_of_nth(OP_SFENCE_PAGE, 0), slot0);

    arch_aspace_release(space, VA_DEEP + 0x40);

    // The last release unmaps and fences the same slot address.
    ASSERT_EQ(ops_with(OP_SFENCE_PAGE), 2u);
    EXPECT_EQ(arg_of_nth(OP_SFENCE_PAGE, 1), slot0);
}

TEST_F(MapFence, AcquireOfAFrameInsideTheKernelWindowSpendsNone)
{
    // Direct RAM acquisition needs no mapping or fence.
    arch_phys_addr_t const inside =
        static_cast<arch_phys_addr_t>(window_pa_hi() - arch_aspace_granule());
    ASSERT_EQ(arch_aspace_map(space, VA_DEEP, inside, 1, RIGHTS_DATA, ARCH_MAP_NORMAL),
              ARCH_ASPACE_OK);
    ops_clear();

    void* const p = arch_aspace_acquire(space, VA_DEEP + 0x40);
    ASSERT_NE(p, nullptr);

    EXPECT_EQ(ops_with(OP_SFENCE_PAGE), 0u);
    arch_aspace_release(space, VA_DEEP + 0x40);
    EXPECT_EQ(ops_with(OP_SFENCE_PAGE), 0u);
}

// ASID width probing.

TEST_F(MapFence, TheProbeReportsTheFieldTheHartImplements)
{
    uint64_t const model = arch_aspace_model();

    EXPECT_EQ((model >> ARCH_ASPACE_MODEL_ASID_SHIFT) & ARCH_ASPACE_MODEL_FIELD_MASK, 16u);
    EXPECT_NE(model & ARCH_ASPACE_MODEL_ASID, 0u);
}

TEST_F(MapFence, ANarrowFieldIsReportedNarrowAndLosesTheVerdict)
{
    remachine(8);

    uint64_t const model = arch_aspace_model();

    EXPECT_EQ((model >> ARCH_ASPACE_MODEL_ASID_SHIFT) & ARCH_ASPACE_MODEL_FIELD_MASK, 8u);
    // Only the ASID-width verdict should change.
    EXPECT_EQ(model & ARCH_ASPACE_MODEL_ASID, 0u);
    EXPECT_NE(model & ARCH_ASPACE_MODEL_GRANULE, 0u);
    EXPECT_NE(model & ARCH_ASPACE_MODEL_PA, 0u);
}

TEST_F(MapFence, AHardwiredZeroFieldReportsNoIdentifierAtAll)
{
    // ASIDLEN may legally be zero.
    remachine(0);

    uint64_t const model = arch_aspace_model();

    EXPECT_EQ((model >> ARCH_ASPACE_MODEL_ASID_SHIFT) & ARCH_ASPACE_MODEL_FIELD_MASK, 0u);
    EXPECT_EQ(model & ARCH_ASPACE_MODEL_ASID, 0u);
}

// ASID allocation and switch fences.

TEST_F(MapFence, ActivateCarriesTheSpacesIdentifierBesideItsRoot)
{
    arch_aspace_activate(elsewhere);

    ASSERT_EQ(ops_with(OP_WRITE_SATP), 1u);
    EXPECT_NE(last_identifier(), 0u);
    // Check both PPN and ASID fields.
    EXPECT_EQ(last_satp() & SATP_PPN_MASK, root_ppn(elsewhere));
    EXPECT_NE(last_satp() >> SATP_MODE_SHIFT, 0u);
}

TEST_F(MapFence, TwoLiveSpacesAreGivenDifferentIdentifiers)
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

TEST_F(MapFence, TheIdentifierBelongsToTheSpaceAndNotToTheSwitch)
{
    arch_aspace_activate(space);
    uint32_t const first = last_identifier();
    arch_aspace_activate(elsewhere);
    arch_aspace_activate(space);

    ASSERT_NE(first, NO_WRITE);
    EXPECT_EQ(last_identifier(), first);
}

TEST_F(MapFence, ADestroyedSpacesIdentifierIsHandedOutAgain)
{
    struct arch_aspace* const first_space = arch_aspace_create();
    ASSERT_NE(first_space, nullptr);
    arch_aspace_activate(first_space);
    uint32_t const freed = last_identifier();
    ASSERT_NE(freed, NO_WRITE);
    ASSERT_NE(freed, 0u);
    // Destroy invalidates before releasing the ID.
    arch_aspace_destroy(first_space);

    struct arch_aspace* const second_space = arch_aspace_create();
    ASSERT_NE(second_space, nullptr);
    arch_aspace_activate(second_space);

    EXPECT_EQ(last_identifier(), freed);
    arch_aspace_destroy(second_space);
}

TEST_F(MapFence, SwitchingBetweenTwoRootsThisHartHasRunFencesNothing)
{
    // Activate both roots first so neither requires first-use publication.
    arch_aspace_activate(elsewhere);
    arch_aspace_activate(space);
    ops_clear();

    arch_aspace_activate(elsewhere);

    EXPECT_EQ(ops_with(OP_SFENCE_ALL), 0u);
    EXPECT_EQ(ops_with(OP_SFENCE_PAGE), 0u);
    EXPECT_EQ(ops_with(OP_WRITE_SATP), 1u);
}

TEST_F(MapFence, TheFirstUseOfASpaceOnThisHartFencesThoughItLeftATaggedRoot)
{
    // Seeding skips translation fences. First activation from a tagged root
    // must supply publication independently of the ASID-0 exit rule.
    ASSERT_EQ(arch_aspace_map(elsewhere, VA_DEEP, PA_A, 1, RIGHTS_DATA, ARCH_MAP_NORMAL),
              ARCH_ASPACE_OK);
    ASSERT_EQ(ops_with(OP_SFENCE_ALL), 0u);
    ASSERT_EQ(ops_with(OP_SFENCE_PAGE), 0u);
    ASSERT_NE(ops_with(OP_FENCE_W_W), 0u);

    // Verify the predecessor is tagged and already synchronized.
    arch_aspace_activate(space);
    ASSERT_NE(last_identifier(), NO_WRITE);
    ASSERT_NE(last_identifier(), 0u);
    ASSERT_EQ(ops_with(OP_SFENCE_ALL), 0u);
    ops_clear();

    arch_aspace_activate(elsewhere);

    EXPECT_EQ(ops_with(OP_SFENCE_ALL), 1u);
    EXPECT_NE(last_identifier(), 0u);
    // Require the fence after the root write.
    int const satp_at = last_index_of(OP_WRITE_SATP);
    int const fence_at = last_index_of(OP_SFENCE_ALL);
    ASSERT_GE(satp_at, 0);
    ASSERT_GE(fence_at, 0);
    EXPECT_GT(fence_at, satp_at);
}

TEST_F(MapFence, AHartJoiningASpaceAnotherHartPopulatedFencesOnItsFirstUse)
{
    // The destination ran on one hart, but the joining hart has not fenced it.
    struct arch_aspace* const mine = arch_aspace_create();
    ASSERT_NE(mine, nullptr);
    arch_aspace_activate(mine);
    ASSERT_EQ(arch_aspace_map(mine, VA_DEEP, PA_A, 1, RIGHTS_DATA, ARCH_MAP_NORMAL),
              ARCH_ASPACE_OK);
    // No peer publication occurred during mapping.
    int const rang = last_index_of(OP_RENDEZVOUS);
    ASSERT_GE(rang, 0);
    ASSERT_EQ(op_at(static_cast<size_t>(rang)).arg, 0u);

    set_cpu(PEER_CORE);
    ops_clear();
    arch_aspace_activate(mine);
    set_cpu(RUNNING_CORE);

    // The peer leaves a tagged root, so this fence must come from first use.
    EXPECT_EQ(ops_with(OP_SFENCE_ALL), 1u);
    EXPECT_NE(last_identifier(), 0u);

    // A second activation must skip the first-use fence.
    set_cpu(PEER_CORE);
    arch_aspace_activate(space);
    ops_clear();
    arch_aspace_activate(mine);
    set_cpu(RUNNING_CORE);

    EXPECT_EQ(ops_with(OP_SFENCE_ALL), 0u);
    EXPECT_EQ(ops_with(OP_WRITE_SATP), 1u);

    arch_aspace_destroy(mine);
}

TEST_F(MapFence, LeavingTheBootRootFencesEveryAddressSpace)
{
    // Leaving the boot root requires invalidation even for a previously used destination.
    arch_aspace_activate(arch_aspace_boot());
    EXPECT_EQ(ops_with(OP_SFENCE_ALL), 0u);
    ASSERT_EQ(last_identifier(), 0u);
    ops_clear();

    arch_aspace_activate(space);

    EXPECT_EQ(ops_with(OP_SFENCE_ALL), 1u);
    EXPECT_NE(last_identifier(), 0u);
}

TEST_F(MapFence, ASpaceBeyondTheRecordsRowsRunsUntaggedAndKeepsItsFence)
{
    // Fill all rows to test the untracked-root fallback. SetUp already uses two.
    constexpr size_t ROWS_LEFT = KICKOS_MAX_DOMAINS - 2;
    // Each root uses one of the 64 frame slots.
    static_assert(KICKOS_MAX_DOMAINS + 4 < 60, "the seam's frame pool cannot fill this record");
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
    EXPECT_EQ(ops_with(OP_SFENCE_ALL), 1u);

    arch_aspace_destroy(rowless);
    for (size_t i = 0; i < ROWS_LEFT; i++)
    {
        arch_aspace_destroy(filler[i]);
    }
}

TEST_F(MapFence, ASpaceTheRecordCannotFollowIsPublishedOnEveryEntry)
{
    // Untracked roots must publish on every activation.
    constexpr size_t ROWS_LEFT = KICKOS_MAX_DOMAINS - 2;
    static_assert(KICKOS_MAX_DOMAINS + 4 < 60, "the seam's frame pool cannot fill this record");
    struct arch_aspace* filler[ROWS_LEFT] = {};
    for (size_t i = 0; i < ROWS_LEFT; i++)
    {
        filler[i] = arch_aspace_create();
        ASSERT_NE(filler[i], nullptr);
        arch_aspace_activate(filler[i]);
    }
    struct arch_aspace* const rowless = arch_aspace_create();
    ASSERT_NE(rowless, nullptr);
    arch_aspace_activate(rowless);
    ASSERT_EQ(last_identifier(), 0u);

    // Re-enter from a tagged root to exclude the ASID-0 exit fence.
    arch_aspace_activate(space);
    ops_clear();
    arch_aspace_activate(rowless);

    EXPECT_EQ(ops_with(OP_SFENCE_ALL), 1u);
    EXPECT_EQ(last_identifier(), 0u);

    arch_aspace_destroy(rowless);
    for (size_t i = 0; i < ROWS_LEFT; i++)
    {
        arch_aspace_destroy(filler[i]);
    }
}

TEST_F(MapFence, DestroyingTheBootRootIsRefusedBeforeItReachesThePool)
{
    // Refusing the boot root must free no frames.
    uint32_t const freed = frames_freed();
    ops_clear();

    arch_aspace_destroy(arch_aspace_boot());

    EXPECT_EQ(frames_freed(), freed);
    EXPECT_EQ(ops_with(OP_SFENCE_ALL), 0u);
    EXPECT_EQ(ops_with(OP_RENDEZVOUS), 0u);

    // Ordinary roots must still be reclaimed.
    struct arch_aspace* const ordinary = arch_aspace_create();
    ASSERT_NE(ordinary, nullptr);
    uint32_t const before = frames_freed();
    ops_clear();
    arch_aspace_destroy(ordinary);
    EXPECT_GT(frames_freed(), before);
    EXPECT_EQ(ops_with(OP_SFENCE_ALL), 1u);
}

TEST_F(MapFence, ReturningToTheBootRootIsNotAFirstUse)
{
    // Every hart already uses the boot root at startup; returning to it needs
    // no first-use fence despite its lack of a residency row.
    arch_aspace_activate(arch_aspace_boot());
    EXPECT_EQ(ops_with(OP_SFENCE_ALL), 0u);
    ASSERT_EQ(last_identifier(), 0u);

    // Check repeated returns too.
    arch_aspace_activate(space);
    ops_clear();
    arch_aspace_activate(arch_aspace_boot());

    EXPECT_EQ(ops_with(OP_SFENCE_ALL), 0u);
    EXPECT_EQ(ops_with(OP_WRITE_SATP), 1u);
}

TEST_F(MapFence, AHardwiredZeroFieldTagsNothingAndLeavesEverySwitchFencing)
{
    // With no ASID field, all roots use ID 0 and every switch fences.
    remachine(0);

    struct arch_aspace* const a = arch_aspace_create();
    struct arch_aspace* const b = arch_aspace_create();
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    arch_aspace_activate(a);
    EXPECT_EQ(last_identifier(), 0u);
    ops_clear();
    arch_aspace_activate(b);

    EXPECT_EQ(last_identifier(), 0u);
    EXPECT_EQ(ops_with(OP_SFENCE_ALL), 1u);
    EXPECT_EQ(arch_aspace_model() & ARCH_ASPACE_MODEL_TAGGED, 0u);

    arch_aspace_destroy(a);
    arch_aspace_destroy(b);
}

TEST_F(MapFence, AnEightBitFieldStillTagsBecauseTheRowCountIsTheNarrowerBound)
{
    // The 8-bit field still fits this test configuration's identifiers.
    remachine(8);

    struct arch_aspace* const narrow = arch_aspace_create();
    ASSERT_NE(narrow, nullptr);
    arch_aspace_activate(narrow);

    ASSERT_NE(last_identifier(), NO_WRITE);
    EXPECT_NE(last_identifier(), 0u);
    EXPECT_LT(last_identifier(), 1u << 8);
    EXPECT_NE(arch_aspace_model() & ARCH_ASPACE_MODEL_TAGGED, 0u);
    EXPECT_EQ(arch_aspace_model() & ARCH_ASPACE_MODEL_ASID, 0u);

    arch_aspace_destroy(narrow);
}

TEST_F(MapFence, TheSixteenBitMachineTagsAndKeepsItsWidthVerdict)
{
    // The full-width field must satisfy both verdicts.
    EXPECT_NE(arch_aspace_model() & ARCH_ASPACE_MODEL_TAGGED, 0u);
    EXPECT_NE(arch_aspace_model() & ARCH_ASPACE_MODEL_ASID, 0u);
}
