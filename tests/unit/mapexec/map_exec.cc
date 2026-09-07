// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A map whose break-before-make replaces a valid EXECUTABLE leaf owes the instruction-side
// rendezvous, and these arms read the WIRING of that debt in arch/arm64/armv8a: whether
// map_into consults the predicate at all, whether the debt survives the recursion the out
// parameter is threaded through, whether arch_aspace_map rings with the peer set that stood
// BEFORE its edits, and whether a FAILED map's rollback pays it too, that rollback being
// itself a removal, of leaves the same call installed.
//
// removal_owes_rendezvous is not the subject: a test of the predicate alone would pass over
// whether map_into ever asks it, which is what these arms are for.
//
// THE PRE-STATE IS BUILT BY THE BACKEND ITSELF: no descriptor here is hand-written, so no arm
// rests on a field layout retyped beside the one under test. That leaves the premise "this leaf
// is executable" to be proven rather than assumed, and the unmap arms at the end are what prove
// it: unmap's own rendezvous rings over exactly the leaves these arms call executable and stays
// silent over the ones they call not.

#include <kickos/arch/arch.h>

#include <gtest/gtest.h>

#include "aspace_sysops_seam.h"

using namespace kickos::testfix;

namespace
{
    // The split helpers this gate exists to reach the multicore arm of. At one kernel core
    // removal_owes_rendezvous is `return false`, peer_cores is 0 and the rendezvous is empty, so
    // every arm below would pass over a backend that never rang.
    static_assert(KICKOS_KERNEL_CORES > 1,
                  "map_exec must compile the multicore arm of removal_owes_rendezvous, "
                  "peer_cores and instruction_side_rendezvous");
    static_assert(KICKOS_NUM_CORES > 1, "the peer set needs a core other than this one");

    constexpr uint32_t RUNNING_CORE = 0;
    constexpr uint32_t PEER_CORE = 1;
    constexpr uint32_t PEER_MASK = 1u << PEER_CORE;

    // TCR_EL1.T0SZ is 25, so the walk starts at level 1 and the leaf sits at level 3: EVERY
    // address below reaches its leaf two recursive calls down from the root, which is what makes
    // the out parameter's thread-through load-bearing.
    constexpr uintptr_t VA_DEEP = 0x40201000;  // level-1 index 1, level-2 index 1, level-3 index 1
    constexpr uintptr_t VA_OTHER = 0x40203000; // the same level-3 table, a different slot
    // A run of two pages either side of a level-2 boundary: two DIFFERENT level-3 tables, so the
    // two leaves are replaced in two separate recursive calls.
    constexpr uintptr_t VA_CROSS_LOW = 0x401FF000;  // level-2 index 0, level-3 index 511
    constexpr uintptr_t VA_CROSS_HIGH = 0x40200000; // level-2 index 1, level-3 index 0
    // VA_CROSS_LOW's own level-3 table, a slot below it: seeding here builds that table without
    // mapping anything inside the run the arms below hand to arch_aspace_map, which refuses a
    // partially mapped range.
    constexpr uintptr_t VA_CROSS_BELOW = 0x401FE000; // level-2 index 0, level-3 index 510

    constexpr arch_phys_addr_t PA_A = 0x20000000;
    constexpr arch_phys_addr_t PA_B = 0x20001000;
    constexpr arch_phys_addr_t PA_C = 0x20002000;

    constexpr uint32_t RIGHTS_EXEC = ARCH_MAP_R | ARCH_MAP_X;
    constexpr uint32_t RIGHTS_DATA = ARCH_MAP_R | ARCH_MAP_W;

    // The hook's subject, set before the map it perturbs.
    struct arch_aspace* g_hook_space = nullptr;

    // A peer leaves the space BETWEEN the peer set being sampled and the rendezvous being rung.
    // A set sampled after the edits is empty by then, and the mask the rendezvous carries is
    // what tells the two sampling points apart.
    void peer_leaves_the_space()
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
            // Both cores run `space`, so the running core has exactly one peer holding it.
            set_cpu(PEER_CORE);
            arch_aspace_activate(space);
            set_cpu(RUNNING_CORE);
            arch_aspace_activate(space);
            ops_clear();
        }

        // The pre-state one arm replaces, built by the backend and not by hand.
        void seed(uintptr_t va, arch_phys_addr_t pa, uint32_t rights)
        {
            ASSERT_EQ(arch_aspace_map(space, va, pa, 1, rights, ARCH_MAP_NORMAL), ARCH_ASPACE_OK);
            ops_clear();
        }

        struct arch_aspace* space = nullptr;
        struct arch_aspace* elsewhere = nullptr;
    };
}

// --- claim 1: the flag is set for a valid leaf whose UXN is clear -------------------------

TEST_F(MapExec, MapOverAnExecutableLeafRingsTheInstructionSide)
{
    seed(VA_DEEP, PA_A, RIGHTS_EXEC);

    ASSERT_EQ(arch_aspace_map(space, VA_DEEP, PA_B, 1, RIGHTS_DATA, ARCH_MAP_NORMAL),
              ARCH_ASPACE_OK);

    EXPECT_EQ(ops_with(OP_RENDEZVOUS), 1u);
    // The peer set the running core owes, and no other.
    int const at = last_index_of(OP_RENDEZVOUS);
    ASSERT_GE(at, 0);
    EXPECT_EQ(op_at(static_cast<size_t>(at)).arg, PEER_MASK);
    // The replacement landed: the debt is not a refusal in disguise.
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

// --- claim 2: it is not set for a valid leaf with UXN set, nor for an invalid one ---------

TEST_F(MapExec, MapOverANonExecutableLeafRingsNothing)
{
    seed(VA_DEEP, PA_A, RIGHTS_DATA);

    ASSERT_EQ(arch_aspace_map(space, VA_DEEP, PA_B, 1, RIGHTS_DATA, ARCH_MAP_NORMAL),
              ARCH_ASPACE_OK);

    // The break-before-make ran, so the arm is not silent for want of a replacement.
    EXPECT_EQ(ops_with(OP_TLBI_PAGE_IS), 2u);
    EXPECT_EQ(ops_with(OP_RENDEZVOUS), 0u);
}

TEST_F(MapExec, MapIntoAnInvalidLeafRingsNothing)
{
    ASSERT_EQ(arch_aspace_map(space, VA_DEEP, PA_A, 1, RIGHTS_EXEC, ARCH_MAP_NORMAL),
              ARCH_ASPACE_OK);

    // An executable leaf INSTALLED over nothing owes no instruction side: no fetch can have
    // come from a slot that held no valid descriptor.
    EXPECT_EQ(ops_with(OP_RENDEZVOUS), 0u);
    EXPECT_EQ(ops_with(OP_TLBI_PAGE_IS), 1u);
}

// --- claim 3: the debt survives map_into's recursion -------------------------------------

TEST_F(MapExec, TheDebtSurvivesTheDEEPERRecursionOfTwo)
{
    // The executable leaf is the SECOND of the run, in the second level-3 table: the debt is
    // raised in a recursive call the first one already returned from.
    seed(VA_CROSS_LOW, PA_A, RIGHTS_DATA);
    seed(VA_CROSS_HIGH, PA_B, RIGHTS_EXEC);

    ASSERT_EQ(arch_aspace_map(space, VA_CROSS_LOW, PA_C, 2, RIGHTS_DATA, ARCH_MAP_NORMAL),
              ARCH_ASPACE_OK);

    EXPECT_EQ(ops_with(OP_RENDEZVOUS), 1u);
    EXPECT_EQ(ops_with(OP_TLBI_PAGE_IS), 4u);
}

TEST_F(MapExec, TheDebtSurvivesTheEARLIERRecursionOfTwo)
{
    // The mirror: the executable leaf is the FIRST of the run, so the debt has to outlive a
    // second recursive call that raises none of its own.
    seed(VA_CROSS_LOW, PA_A, RIGHTS_EXEC);
    seed(VA_CROSS_HIGH, PA_B, RIGHTS_DATA);

    ASSERT_EQ(arch_aspace_map(space, VA_CROSS_LOW, PA_C, 2, RIGHTS_DATA, ARCH_MAP_NORMAL),
              ARCH_ASPACE_OK);

    EXPECT_EQ(ops_with(OP_RENDEZVOUS), 1u);
    EXPECT_EQ(ops_with(OP_TLBI_PAGE_IS), 4u);
}

// --- claim 4: rung after the publishing barrier, with the set sampled before the edits ----

TEST_F(MapExec, TheRendezvousFollowsTheBarrierThatPublishesTheEdits)
{
    seed(VA_DEEP, PA_A, RIGHTS_EXEC);

    ASSERT_EQ(arch_aspace_map(space, VA_DEEP, PA_B, 1, RIGHTS_DATA, ARCH_MAP_NORMAL),
              ARCH_ASPACE_OK);

    size_t const n = ops_count();
    ASSERT_GE(n, 2u);
    // Last, and immediately behind the DSB the descriptor writes are published by: a peer
    // synchronizing on a table it cannot yet see is a rendezvous spent for nothing.
    EXPECT_EQ(op_at(n - 1).tag, OP_RENDEZVOUS);
    EXPECT_EQ(op_at(n - 2).tag, OP_DSB_ISHST);
    EXPECT_LT(static_cast<size_t>(last_index_of(OP_TLBI_PAGE_IS)), n - 1);
}

TEST_F(MapExec, ThePeerSetIsSampledBeforeTheEdits)
{
    seed(VA_DEEP, PA_A, RIGHTS_EXEC);
    g_hook_space = elsewhere;
    arm_mid_edit(peer_leaves_the_space);

    ASSERT_EQ(arch_aspace_map(space, VA_DEEP, PA_B, 1, RIGHTS_DATA, ARCH_MAP_NORMAL),
              ARCH_ASPACE_OK);

    int const at = last_index_of(OP_RENDEZVOUS);
    ASSERT_GE(at, 0);
    EXPECT_EQ(op_at(static_cast<size_t>(at)).arg, PEER_MASK);
}

TEST_F(MapExec, ThePeerLeavingIsWhatTheSamplingPointIsReadAgainst)
{
    // The other half of the arm above: once the peer has gone, a set sampled at EITHER point is
    // empty, so the mask that arm asserts is a sampling point and not a constant.
    seed(VA_DEEP, PA_A, RIGHTS_EXEC);
    set_cpu(PEER_CORE);
    arch_aspace_activate(elsewhere);
    set_cpu(RUNNING_CORE);
    ops_clear();

    ASSERT_EQ(arch_aspace_map(space, VA_DEEP, PA_B, 1, RIGHTS_DATA, ARCH_MAP_NORMAL),
              ARCH_ASPACE_OK);

    int const at = last_index_of(OP_RENDEZVOUS);
    ASSERT_GE(at, 0);
    EXPECT_EQ(op_at(static_cast<size_t>(at)).arg, 0u);
}

// --- claim 5: a FAILED map's rollback is a removal and owes the same debt -----------------

TEST_F(MapExec, TheRollbackOfAFailedMapRingsForTheExecutableLeafItRemoves)
{
    // The low page's slot is EMPTY, so the only executable leaf in this call is the one it
    // INSTALLS, over which no debt is raised going in. Its level-3 table already stands, so
    // the low page maps on a refused pool; the high page needs a table of its own and fails,
    // and the unwind removes that freshly installed executable leaf, which a peer in this
    // space may have fetched from in between.
    seed(VA_CROSS_BELOW, PA_A, RIGHTS_DATA);
    set_frame_budget(0);

    ASSERT_EQ(arch_aspace_map(space, VA_CROSS_LOW, PA_C, 2, RIGHTS_EXEC, ARCH_MAP_NORMAL),
              ARCH_ASPACE_ENOMEM);

    // The rollback ran: neither page is mapped.
    EXPECT_EQ(arch_aspace_frame_at(space, VA_CROSS_LOW), 0u);
    EXPECT_EQ(arch_aspace_frame_at(space, VA_CROSS_HIGH), 0u);
    EXPECT_EQ(ops_with(OP_RENDEZVOUS), 1u);
    int const at = last_index_of(OP_RENDEZVOUS);
    ASSERT_GE(at, 0);
    EXPECT_EQ(op_at(static_cast<size_t>(at)).arg, PEER_MASK);
    // LAST, so it stands behind the unwind's own invalidation and its barriers: a peer
    // synchronizing before the leaves are gone would synchronize on the state being undone.
    EXPECT_EQ(static_cast<size_t>(at), ops_count() - 1u);
}

TEST_F(MapExec, TheRollbackOfANonExecutableMapRingsNothing)
{
    // The discriminator for the arm above: the same failure and the same unwind, with nothing
    // executable removed, so the rendezvous there is the leaf's property and not the failure's.
    seed(VA_CROSS_BELOW, PA_A, RIGHTS_DATA);
    set_frame_budget(0);

    ASSERT_EQ(arch_aspace_map(space, VA_CROSS_LOW, PA_C, 2, RIGHTS_DATA, ARCH_MAP_NORMAL),
              ARCH_ASPACE_ENOMEM);

    EXPECT_EQ(arch_aspace_frame_at(space, VA_CROSS_LOW), 0u);
    EXPECT_EQ(ops_with(OP_RENDEZVOUS), 0u);
}

// --- claim 6: the unwind's precondition is CHECKED, not assumed (arch.h) ------------------

TEST_F(MapExec, APartiallyMappedRangeIsRefusedBeforeAnyEdit)
{
    // The low page is mapped and the high one is not, and the high page's table is refused.
    // Admitted, this call would replace the low leaf and then lose it to the unwind, which
    // clears from `va` up and has no narrower form: the frame below is what the refusal saves.
    seed(VA_CROSS_LOW, PA_A, RIGHTS_DATA);
    set_frame_budget(0);

    // EXPECT and not ASSERT: the post-state below is what the refusal is FOR, and an abort
    // here would leave it unread.
    EXPECT_EQ(arch_aspace_map(space, VA_CROSS_LOW, PA_C, 2, RIGHTS_DATA, ARCH_MAP_NORMAL),
              ARCH_ASPACE_EINVAL);

    // Nothing edited: the pre-existing leaf still names its own frame and no maintenance ran.
    EXPECT_EQ(arch_aspace_frame_at(space, VA_CROSS_LOW), PA_A);
    EXPECT_EQ(arch_aspace_frame_at(space, VA_CROSS_HIGH), 0u);
    EXPECT_EQ(ops_count(), 0u);
}

TEST_F(MapExec, AWhollyMappedRangeIsStillARemapAndPasses)
{
    // The discriminator: the refusal above must not reach the caller that remaps an
    // already-complete grant, whose whole range holds leaves.
    seed(VA_CROSS_LOW, PA_A, RIGHTS_DATA);
    seed(VA_CROSS_HIGH, PA_B, RIGHTS_DATA);

    ASSERT_EQ(arch_aspace_map(space, VA_CROSS_LOW, PA_C, 2, RIGHTS_DATA, ARCH_MAP_NORMAL),
              ARCH_ASPACE_OK);

    arch_phys_addr_t const g = static_cast<arch_phys_addr_t>(arch_aspace_granule());
    EXPECT_EQ(arch_aspace_frame_at(space, VA_CROSS_LOW), PA_C);
    EXPECT_EQ(arch_aspace_frame_at(space, VA_CROSS_HIGH), PA_C + g);
}

// --- the premise, proven by the mirror map_into's rendezvous is modelled on ---------------

TEST_F(MapExec, UnmapOfTheSameLeafRingsToo)
{
    seed(VA_DEEP, PA_A, RIGHTS_EXEC);

    ASSERT_EQ(arch_aspace_unmap(space, VA_DEEP, 1), ARCH_ASPACE_OK);

    // The oracle for every arm above that calls a RIGHTS_EXEC leaf executable.
    EXPECT_EQ(ops_with(OP_RENDEZVOUS), 1u);
}

TEST_F(MapExec, UnmapOfANonExecutableLeafRingsNothing)
{
    seed(VA_DEEP, PA_A, RIGHTS_DATA);

    ASSERT_EQ(arch_aspace_unmap(space, VA_DEEP, 1), ARCH_ASPACE_OK);

    // And for every arm that calls a RIGHTS_DATA leaf not executable.
    EXPECT_EQ(ops_with(OP_RENDEZVOUS), 0u);
}
