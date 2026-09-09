// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The arena-ownership table, over the REAL kernel/mem/ramown.cc and a base+limit region
// seam this file sets. What only a host gate can drive: the table's own exhaustion, which
// on a board would need the arena to outlast the record count, and a task slot RECYCLED
// under a bumped generation.

#include <kickos/ramown.h>
#include <kickos/task.h>

#include <gtest/gtest.h>

#include <stddef.h>
#include <stdint.h>

namespace
{
    // A base+limit backend: every 32-byte multiple is nameable, which is the leg where the
    // rounding a descriptor pays can carry an interior base past its block.
    constexpr size_t MIN_REGION = 32u;
    constexpr uintptr_t ARENA_BASE = 0x20010000u;
    constexpr size_t ARENA_SIZE = 0x4000u;

    size_t g_arena_used = 0;
}

extern "C"
{
    size_t arch_mpu_min_region(void) { return MIN_REGION; }
    int arch_mpu_region_pow2(void) { return 0; }

    uintptr_t arch_ram_base(void) { return ARENA_BASE; }
    size_t arch_ram_size(void) { return ARENA_SIZE; }

    // The bump allocator's shape, not its code: natural alignment and a watermark that
    // never goes back. The watermark is what an arm reads to say whether a refusal SPENT
    // arena.
    void* arch_ram_alloc(size_t size)
    {
        if (size == 0)
        {
            return nullptr;
        }
        size_t const rsz = arch_ram_region_size(size);
        size_t const ralign = arch_ram_region_align(size);
        uintptr_t const cur = ARENA_BASE + g_arena_used;
        uintptr_t const aligned = (cur + (ralign - 1u)) & ~static_cast<uintptr_t>(ralign - 1u);
        size_t const off = static_cast<size_t>(aligned - ARENA_BASE);
        if (aligned < cur or off > ARENA_SIZE or rsz > ARENA_SIZE - off)
        {
            return nullptr;
        }
        g_arena_used = off + rsz;
        return reinterpret_cast<void*>(aligned);
    }
}

namespace kickos
{
    // The pool the fixture owns, and the real handle codec over it: index BIASED BY ONE in
    // the low field, generation above it. Freeing a slot bumps the generation, which is the
    // whole reason a record can name a dead task and be nameable by nobody.
    namespace
    {
        constexpr size_t FAKE_TASKS = 4;
        Task g_tasks[FAKE_TASKS];
    }

    kos_task_t task_handle(Task const* t)
    {
        if (t == nullptr)
        {
            return KOS_TASK_NONE;
        }
        for (size_t i = 0; i < FAKE_TASKS; i++)
        {
            if (&g_tasks[i] == t)
            {
                return (static_cast<kos_task_t>(t->gen) << TASK_INDEX_BITS)
                       | static_cast<kos_task_t>(i + 1u);
            }
        }
        return KOS_TASK_NONE;
    }
}

namespace
{
    using kickos::ram_owner_alloc;
    using kickos::ram_owner_nameable;
    using kickos::Task;

    Task* task_a() { return &kickos::g_tasks[0]; }
    Task* task_b() { return &kickos::g_tasks[1]; }

    class OwnerTable : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            // The table is file-static in ramown.cc with no reset seam, exactly as the
            // arena has none, so the gate spends fresh slots per case rather than clearing
            // it. Each case therefore asserts over the blocks IT allocated.
            for (kickos::Task& t : kickos::g_tasks)
            {
                t = kickos::Task{};
            }
        }
    };

    TEST_F(OwnerTable, ReservingTaskNamesItsWholeBlock)
    {
        void* const p = ram_owner_alloc(task_a(), 128);
        ASSERT_NE(p, nullptr);
        uintptr_t const b = reinterpret_cast<uintptr_t>(p);
        EXPECT_TRUE(ram_owner_nameable(task_a(), b, 128));
    }

    TEST_F(OwnerTable, ReservingTaskNamesAnInteriorSubRange)
    {
        void* const p = ram_owner_alloc(task_a(), 128);
        ASSERT_NE(p, nullptr);
        uintptr_t const b = reinterpret_cast<uintptr_t>(p);
        EXPECT_TRUE(ram_owner_nameable(task_a(), b + MIN_REGION, MIN_REGION));
        EXPECT_TRUE(ram_owner_nameable(task_a(), b + 96u, MIN_REGION));
    }

    // SEC-1 ITSELF: the block is in the arena and one descriptor names it, so every other
    // admission arm admits it. Only ownership refuses.
    TEST_F(OwnerTable, SiblingTaskCannotNameIt)
    {
        void* const p = ram_owner_alloc(task_a(), 128);
        ASSERT_NE(p, nullptr);
        uintptr_t const b = reinterpret_cast<uintptr_t>(p);
        EXPECT_FALSE(ram_owner_nameable(task_b(), b, 128));
        EXPECT_FALSE(ram_owner_nameable(task_b(), b + MIN_REGION, MIN_REGION));
    }

    TEST_F(OwnerTable, NoTaskOwnsAnythingAndReservesNothing)
    {
        void* const p = ram_owner_alloc(task_a(), 128);
        ASSERT_NE(p, nullptr);
        EXPECT_FALSE(ram_owner_nameable(nullptr, reinterpret_cast<uintptr_t>(p), 128));
        // A thread with no task cannot reserve either: the block would be owned by nobody,
        // and so nameable by nobody, for the life of the image.
        size_t const before = g_arena_used;
        EXPECT_EQ(ram_owner_alloc(nullptr, 128), nullptr);
        EXPECT_EQ(g_arena_used, before);
    }

    // The kernel's own arena blocks (the boot stacks, the thread pool's default stacks,
    // the sim's guard page) reach arch_ram_alloc directly and are recorded nowhere.
    TEST_F(OwnerTable, AKernelPlacedBlockIsNobodys)
    {
        void* const kernel_block = arch_ram_alloc(128);
        ASSERT_NE(kernel_block, nullptr);
        uintptr_t const b = reinterpret_cast<uintptr_t>(kernel_block);
        EXPECT_FALSE(ram_owner_nameable(task_a(), b, 128));
        EXPECT_FALSE(ram_owner_nameable(task_b(), b, 128));
        EXPECT_FALSE(ram_owner_nameable(nullptr, b, 128));
    }

    // Two adjacent blocks with a base the region seam admits for the SUM: geometry cannot
    // see the seam between them, so a caller naming both at once is refused here or nowhere.
    TEST_F(OwnerTable, ARangeSpanningTwoBlocksIsRefusedToBoth)
    {
        void* const first = ram_owner_alloc(task_a(), 128);
        void* const second = ram_owner_alloc(task_b(), 128);
        ASSERT_NE(first, nullptr);
        ASSERT_NE(second, nullptr);
        uintptr_t const a = reinterpret_cast<uintptr_t>(first);
        uintptr_t const c = reinterpret_cast<uintptr_t>(second);
        ASSERT_GT(c, a);
        // Derived, not assumed: the alignment run-up between two blocks depends on the
        // granule and on KICKOS_TLS, so a hard-coded stride would only test one posture.
        EXPECT_FALSE(ram_owner_nameable(task_a(), a, static_cast<size_t>(c - a) + 128u));
        EXPECT_FALSE(ram_owner_nameable(task_b(), a, static_cast<size_t>(c - a) + 128u));
        // And the tail of the first plus the head of the second, wholly inside neither.
        EXPECT_FALSE(ram_owner_nameable(task_a(), a + 96u,
                                        static_cast<size_t>(c + MIN_REGION - (a + 96u))));
        EXPECT_FALSE(ram_owner_nameable(task_b(), a + 96u,
                                        static_cast<size_t>(c + MIN_REGION - (a + 96u))));
    }

    // The COMMITTED extent decides, not the named one: a descriptor is sized with
    // arch_ram_region_size. No live call site can present this base, their own
    // arch_ram_region_admissible refusing a base off the granule first, so this pins the
    // predicate's contract rather than a reachable escalation, and is why it rounds.
    TEST_F(OwnerTable, ARoundedExtentLeavingTheBlockIsRefused)
    {
        void* const p = ram_owner_alloc(task_a(), 128);
        ASSERT_NE(p, nullptr);
        uintptr_t const b = reinterpret_cast<uintptr_t>(p);
        EXPECT_TRUE(b + 16u + 112u <= b + 128u);              // the NAMED range fits
        EXPECT_EQ(arch_ram_region_size(112), 128u);           // the COMMITTED one does not
        EXPECT_FALSE(ram_owner_nameable(task_a(), b + 16u, 112));
    }

    TEST_F(OwnerTable, SizeZeroAndAWrappingWindowAreRefused)
    {
        void* const p = ram_owner_alloc(task_a(), 128);
        ASSERT_NE(p, nullptr);
        EXPECT_FALSE(ram_owner_nameable(task_a(), reinterpret_cast<uintptr_t>(p), 0));
        EXPECT_FALSE(ram_owner_nameable(task_a(), UINTPTR_MAX - 8u, 64));
        size_t const before = g_arena_used;
        EXPECT_EQ(ram_owner_alloc(task_a(), 0), nullptr);
        EXPECT_EQ(g_arena_used, before);
    }

    // A record outlives its task, and the handle it carries is (slot, generation): the next
    // occupant of the slot is a different task and inherits nothing.
    TEST_F(OwnerTable, ARecycledTaskSlotInheritsNothing)
    {
        Task* const slot = task_a();
        void* const p = ram_owner_alloc(slot, 128);
        ASSERT_NE(p, nullptr);
        uintptr_t const b = reinterpret_cast<uintptr_t>(p);
        ASSERT_TRUE(ram_owner_nameable(slot, b, 128));
        *slot = Task{};
        slot->gen = 1; // what free_task's bump leaves behind
        EXPECT_FALSE(ram_owner_nameable(slot, b, 128));
    }

    // THE TABLE'S OWN EXHAUSTION AND NOT THE ARENA'S. Both refuse with NULL, both must
    // spend no arena doing it (the allocator never takes a block back), and kos_ram_alloc
    // has no route out for an errno, so the only thing that tells them apart is a RAW
    // arch_ram_alloc that still succeeds, that call bypassing the table. It runs LAST,
    // moving the watermark the spends-nothing arms read.
    //
    // `seated` is what THIS case seated, never the table's occupancy: with no reset seam a
    // whole-binary run arrives with slots already taken, ctest's per-case entries with none.
    TEST_F(OwnerTable, AFullTableAnswersNullAndSpendsNoArena)
    {
        ASSERT_GE(ARENA_SIZE / MIN_REGION, KICKOS_RAM_OWNER_SLOTS + 1u)
            << "this fixture's arena cannot outlast the table plus one probe block, so the "
               "refusal below would be the ARENA's and this case would witness nothing";
        size_t seated = 0;
        while (seated < KICKOS_RAM_OWNER_SLOTS + 1u)
        {
            if (ram_owner_alloc(task_a(), MIN_REGION) == nullptr)
            {
                break;
            }
            seated++;
        }
        ASSERT_GT(seated, 0u) << "the FIRST call refused, so no full table was reached here";
        ASSERT_LT(seated, KICKOS_RAM_OWNER_SLOTS + 1u)
            << "no refusal within the ceiling: the table seated more records than it has slots";
        size_t const before = g_arena_used;
        EXPECT_EQ(ram_owner_alloc(task_a(), MIN_REGION), nullptr);
        EXPECT_EQ(g_arena_used, before);
        EXPECT_EQ(ram_owner_alloc(task_b(), MIN_REGION), nullptr);
        EXPECT_EQ(g_arena_used, before);
        EXPECT_NE(arch_ram_alloc(MIN_REGION), nullptr)
            << "the ARENA was spent too, so the refusals above name no one subject";
    }
}
