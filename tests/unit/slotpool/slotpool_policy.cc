// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Wrap-distance witness for SlotPool's allocation policy (kernel/include/kickos/slotpool.h).
//
// gen_[i] advances once per free() of slot i, so the ABA guard's strength is set by how
// evenly a create/destroy workload spreads its recycles over the pool. This gate measures
// that spread through the PUBLIC API only: alloc() returns the index it claimed, and every
// case here frees exactly what it allocated, so the per-slot allocation count IS the number
// of generation bumps that slot took. Nothing decodes the handle, so widening INDEX_BITS
// cannot turn this into a silent pass.
//
// It also covers the two consequences of the 16-bit index: a pool of more than 256 slots,
// and a handle whose aged generation sets bit 31. A sign test on a handle would silently
// reject the second as an error.
//
// Host-only: the policy is a property of the header, not of any board, and the object index
// is not observable from userspace on a target (a thread sees cap handles, never slot indices),
// so no on-target selftest arm can witness this.

#include <stdint.h>
#include <stdio.h>

#include <gtest/gtest.h>

#include <kickos/slotpool.h>

namespace
{
    struct Obj
    {
        int v;
    };

    // Churn one transient object `cycles` times over a pool holding `resident` slots live,
    // and return the busiest slot's share. Under next-fit the transient walks the free
    // slots, so the busiest takes cycles/(N-resident); under first-fit it takes all of them.
    template <int N>
    void busiest_slot(int cycles, int resident, int* busiest_out, int* distinct_out)
    {
        kickos::SlotPool<Obj, N> pool;
        for (int i = 0; i < resident; i++)
        {
            ASSERT_GE(pool.alloc(), 0) << "resident set fits the pool";
        }

        int counts[N] = {};
        for (int c = 0; c < cycles; c++)
        {
            int const index = pool.alloc();
            ASSERT_TRUE(index >= 0 and index < N) << "churn alloc returned " << index << " at cycle "
                                                 << c;
            counts[index]++;
            pool.free(pool.handle_for(index));
        }

        int busiest = 0;
        int distinct = 0;
        for (int i = 0; i < N; i++)
        {
            if (counts[i] > busiest)
            {
                busiest = counts[i];
            }
            if (counts[i] > 0)
            {
                distinct++;
            }
        }
        *busiest_out = busiest;
        *distinct_out = distinct;
    }

    // The headline property, run at each pool size the fleet actually configures.
    // With nothing else live, a perfect spread puts ceil(cycles/N) on the busiest slot;
    // first-fit puts all `cycles` on slot 0.
    template <int N>
    void spread_pure_churn(char const* label)
    {
        int const cycles = 100 * N;
        int distinct = 0;
        int busiest = 0;
        ASSERT_NO_FATAL_FAILURE(busiest_slot<N>(cycles, 0, &busiest, &distinct));
        int const perfect = (cycles + N - 1) / N;

        printf("# %s N=%d cycles=%d busiest=%d distinct=%d wrap-distance-factor=%d\n", label, N,
               cycles, busiest, distinct, cycles / busiest);
        EXPECT_LE(busiest, perfect)
            << "pure churn spreads evenly (no slot takes more than its share)";
        EXPECT_EQ(distinct, N) << "pure churn touches every slot";
        EXPECT_EQ(cycles / busiest, N) << "wrap distance improves by the full factor N";
    }

    // Completeness: a free slot must be found wherever the cursor rests, which is the leg
    // that fails if the scan runs cursor..N-1 without wrapping.
    //
    // The lone free slot walks BACKWARD on purpose. Each reclaim parks the cursor just past
    // the slot it returned, so a forward walk keeps the cursor sitting exactly on the next
    // target and never needs the wrap at all. A forward version of this case passes under
    // a non-wrapping scan and proves nothing. Walking backward puts the target below the
    // cursor every time, so only a scan that wraps can reach it.
    template <int N>
    void lone_free_slot_found_from_any_cursor(char const* label)
    {
        kickos::SlotPool<Obj, N> pool;
        for (int i = 0; i < N; i++)
        {
            ASSERT_GE(pool.alloc(), 0) << "fill: every slot claimable";
        }
        ASSERT_EQ(pool.alloc(), -1) << "a full pool reports -1";

        for (int round = 0; round < 2; round++)
        {
            for (int target = N - 1; target >= 0; target--)
            {
                pool.free(pool.handle_for(target));
                ASSERT_EQ(pool.alloc(), target) << label << ": lone free slot " << target
                                                << " not found";
            }
        }
        printf("# %s N=%d: lone free slot reachable from every cursor position\n", label, N);
    }

    // At file scope: the widest pool is far too large for a stack frame.
    constexpr int WIDE = 1024;
    constexpr int WIDEST = (1 << 16) - 1; // SlotPool refuses an N that would seat all-ones
    kickos::SlotPool<Obj, WIDE> g_wide;
    kickos::SlotPool<Obj, WIDEST> g_widest;

    // Every slot of a wide pool is claimable, distinct, and resolves; the top slot is the one
    // an index field one bit too narrow would alias onto another.
    template <int N>
    void pool_past_the_old_wall(kickos::SlotPool<Obj, N>& pool, char const* label)
    {
        int top = -1;
        for (int i = 0; i < N; i++)
        {
            int const index = pool.alloc();
            ASSERT_TRUE(index >= 0 and index < N) << label << ": claim " << i << " returned "
                                                  << index;
            top = index;
        }
        EXPECT_EQ(pool.alloc(), -1) << "a full wide pool reports -1";
        EXPECT_EQ(top, N - 1) << "the last claim is the top slot";
        int const handle = pool.handle_for(N - 1);
        EXPECT_EQ(pool.resolve(handle), pool.at(N - 1)) << "the top slot's handle resolves to it";
        EXPECT_EQ(pool.resolve(handle - 1), pool.at(N - 2))
            << "and its neighbour is a different slot, so the index field is wide enough";
        printf("# %s N=%d: every slot claimable, top handle 0x%08x\n", label, N,
               static_cast<unsigned>(handle));
    }
}

// N=4: microbit sems/endpoints/irq_bindings, bluepill-c8 sems/mutexes/irq_bindings.
TEST(SlotPool, spread_pure_churn_tiny_board_pool)
{
    spread_pure_churn<4>("tiny-board pool");
}

// N=8: mutexes fleet-wide, frdmk64f irq_bindings.
TEST(SlotPool, spread_pure_churn_mid_pool)
{
    spread_pure_churn<8>("mid pool");
}

// N=16: frdmk64f sems (the system.h default).
TEST(SlotPool, spread_pure_churn_default_sem_pool)
{
    spread_pure_churn<16>("default sem pool");
}

// The same measurement with part of the pool permanently occupied: the factor is the
// number of FREE slots, not N. This is the honest, non-best-case leg.
TEST(SlotPool, spread_with_resident_set)
{
    int const cycles = 500;
    int const resident = 3;
    int distinct = 0;
    int busiest = 0;
    ASSERT_NO_FATAL_FAILURE(busiest_slot<8>(cycles, resident, &busiest, &distinct));
    int const free_slots = 8 - resident;
    int const perfect = (cycles + free_slots - 1) / free_slots;

    printf("# resident=%d of N=8 cycles=%d busiest=%d distinct=%d factor=%d\n", resident, cycles,
           busiest, distinct, cycles / busiest);
    EXPECT_LE(busiest, perfect) << "churn against a resident set spreads over the free slots";
    EXPECT_EQ(distinct, free_slots) << "churn touches exactly the free slots";
}

// Two sizes: N=4 is the tiny-board pool, N=5 breaks any stride coincidence with N=4.
TEST(SlotPool, lone_free_slot_found_from_any_cursor_n4)
{
    lone_free_slot_found_from_any_cursor<4>("wrap");
}

TEST(SlotPool, lone_free_slot_found_from_any_cursor_n5)
{
    lone_free_slot_found_from_any_cursor<5>("wrap");
}

// Exhaustion is exact and reversible: N distinct slots, then -1, then all N again.
TEST(SlotPool, exhaustion_is_exact)
{
    kickos::SlotPool<Obj, 4> pool;
    for (int round = 0; round < 3; round++)
    {
        bool seen[4] = {};
        int handles[4] = {};
        for (int i = 0; i < 4; i++)
        {
            int const index = pool.alloc();
            ASSERT_TRUE(index >= 0 and index < 4 and not seen[index])
                << "round " << round << " claim " << i << " returned " << index;
            seen[index] = true;
            handles[i] = pool.handle_for(index);
        }
        ASSERT_EQ(pool.alloc(), -1) << "pool full after N claims";
        for (int i = 0; i < 4; i++)
        {
            pool.free(handles[i]);
        }
    }
}

// resolve()/free() semantics must be untouched by the policy change.
TEST(SlotPool, resolve_semantics)
{
    kickos::SlotPool<Obj, 4> pool;
    int const index = pool.alloc();
    ASSERT_GE(index, 0) << "claim for the resolve check";
    int const handle = pool.handle_for(index);

    EXPECT_EQ(pool.resolve(handle), pool.at(index)) << "a live handle resolves to its slot";
    // A fully aged handle IS negative (below), so this is not "a negative handle never
    // resolves". What makes -1 unresolvable is its all-ones INDEX, which the pool never
    // seats.
    EXPECT_EQ(pool.resolve(-1), nullptr) << "an all-ones handle names the reserved index";

    pool.free(handle);
    EXPECT_EQ(pool.resolve(handle), nullptr) << "a freed handle stops resolving";

    int const index2 = pool.alloc();
    EXPECT_GE(index2, 0) << "reclaim after free";
    EXPECT_EQ(pool.resolve(handle), nullptr) << "the stale handle stays dead after the reclaim";
    EXPECT_NE(pool.resolve(pool.handle_for(index2)), nullptr) << "the fresh handle resolves";
}

// A slot recycled past 32768 times mints a handle with bit 31 set: negative as an int,
// and it must still resolve and still free. A `handle < 0` guard breaks here and nowhere
// else, since no in-tree workload recycles one slot that many times.
TEST(SlotPool, aged_generation_handle_is_negative_and_live)
{
    kickos::SlotPool<Obj, 4> pool;
    // free() bumps the slot's generation; alloc() next-fit walks the ring, so 4 slots per
    // lap. Drive slot 0's generation past the sign bit.
    for (uint32_t g = 0; g < 0x8000u; g++)
    {
        for (int s = 0; s < 4; s++)
        {
            int const index = pool.alloc();
            ASSERT_GE(index, 0) << "aged churn keeps finding a slot";
            pool.free(pool.handle_for(index));
        }
    }
    int const index = pool.alloc();
    ASSERT_GE(index, 0) << "a claim after the aged churn";
    int const handle = pool.handle_for(index);
    printf("# aged handle for slot %d = 0x%08x (negative=%d)\n", index,
           static_cast<unsigned>(handle), static_cast<int>(handle < 0));
    EXPECT_LT(handle, 0) << "an aged generation sets bit 31: the handle is negative";
    EXPECT_EQ(pool.resolve(handle), pool.at(index))
        << "and a negative live handle still resolves";
    pool.free(handle);
    EXPECT_EQ(pool.resolve(handle), nullptr) << "and a negative handle still frees its slot";
}

TEST(SlotPool, pool_past_the_old_wall_wide)
{
    pool_past_the_old_wall<WIDE>(g_wide, "wide pool");
}

TEST(SlotPool, pool_past_the_old_wall_widest)
{
    pool_past_the_old_wall<WIDEST>(g_widest, "widest pool");
}
