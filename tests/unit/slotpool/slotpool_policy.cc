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
// Host-only: the policy is a property of the header, not of any board, and the slot index
// alloc() returns is the observable it turns on.

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

    // Completeness: a free slot must be found wherever the cursor rests, which is what a
    // scan running cursor..N-1 without wrapping fails.
    //
    // The lone free slot walks BACKWARD on purpose. Each reclaim parks the cursor just past
    // the slot it returned, so a forward walk keeps the cursor sitting on the next target and
    // never needs the wrap at all: it passes under a non-wrapping scan. Walking backward puts
    // the target below the cursor every time, so only a scan that wraps reaches it.
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

// N=4: the endpoint-pool default, and the floor the 16-20 KiB boards cut their
// semaphore and IRQ-handle pools to.
TEST(SlotPool, spread_pure_churn_tiny_board_pool)
{
    spread_pure_churn<4>("tiny-board pool");
}

// N=8: the mutex and IRQ-handle default (config/system.h).
TEST(SlotPool, spread_pure_churn_mid_pool)
{
    spread_pure_churn<8>("mid pool");
}

// N=16: the semaphore default (config/system.h).
TEST(SlotPool, spread_pure_churn_default_sem_pool)
{
    spread_pure_churn<16>("default sem pool");
}

// The same measurement with part of the pool permanently occupied: the factor is the
// number of FREE slots, not N.
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

// resolve()/free() semantics, which the allocation policy does not bear on.
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

namespace
{
    // An index the optimiser cannot fold. WITHOUT IT THESE ARMS WITNESS THE COMPILER: given a
    // constant out-of-range index, -Werror=array-bounds refuses an unbounded at() at compile
    // time, so the mutation that removes the bound never links and the previous binary is what
    // runs.
    int opaque(int v)
    {
        volatile int x = v;
        return x;
    }
}

// --- at() is TOTAL over its index -------------------------------------------------------
// The one accessor here that took an index on trust. Both halves of the refusal are host-only
// by construction: no board can call at() with an out-of-range index, because every kernel
// site derives one from alloc() or from a resolved object, and no board can call it on a freed
// slot either. What a board WOULD have shown is the corruption afterwards, not the refusal.
TEST(SlotPool, at_bounds_its_index)
{
    constexpr int N = 4;
    kickos::SlotPool<Obj, N> pool;

    EXPECT_EQ(pool.at(opaque(N)), nullptr) << "one past the last slot";
    EXPECT_EQ(pool.at(opaque(N + 1)), nullptr) << "past the array";
    EXPECT_EQ(pool.at(opaque(0xFFFF)), nullptr) << "the reserved all-ones index";
    // alloc() answers -1 for a full pool, and that -1 is an INDEX. Compared as unsigned, so
    // a signed `index >= N` would let it through and hand back slots_[-1].
    EXPECT_EQ(pool.at(opaque(-1)), nullptr) << "alloc()'s pool-full answer";
    EXPECT_EQ(pool.at(opaque(-N - 1)), nullptr) << "and any other negative index";

    // live() takes an index too, and answers the same question without a slot to dereference.
    EXPECT_FALSE(pool.live(opaque(N))) << "live() bounds its index as at() does";
    EXPECT_FALSE(pool.live(opaque(-1)));
}

// WHAT at() DOES NOT ANSWER, so a reader of the kernel's call sites knows what is still owed
// there. at() bounds the INDEX and nothing else: a freed slot is in range, so it answers with a
// pointer to its last occupant's fields. live() is the liveness question, and a sweep over
// indices has to ask it. Bounding liveness inside at() was measured at eight bytes of frame on
// the one inlining site that sits on an AMP node's SVCK chain, which has no spare byte.
TEST(SlotPool, at_bounds_the_index_and_not_liveness)
{
    constexpr int N = 4;
    kickos::SlotPool<Obj, N> pool;

    // An untouched pool: in range, so at() answers, and live() is what says nothing is there.
    for (int i = 0; i < N; i++)
    {
        EXPECT_NE(pool.at(i), nullptr) << "slot " << i << " is in range";
        EXPECT_FALSE(pool.live(i)) << "and live() is what refuses it";
    }

    int const index = pool.alloc();
    ASSERT_GE(index, 0);
    Obj* const seated = pool.at(index);
    EXPECT_NE(seated, nullptr) << "at(alloc()) is the claim's own slot";
    EXPECT_TRUE(pool.live(index));
    EXPECT_EQ(seated, pool.resolve(pool.handle_for(index)))
        << "and it is the slot that index's handle resolves to";

    // A FREED SLOT KEEPS ITS LAST CONTENTS and at() still hands it back: the two questions are
    // separate, and this is the caller obligation live() exists to discharge.
    seated->v = 0x5A;
    int const handle = pool.handle_for(index);
    pool.free(handle);
    EXPECT_EQ(pool.at(index), seated) << "a freed slot is in range, so at() answers";
    EXPECT_EQ(pool.at(index)->v, 0x5A) << "with its last occupant's fields standing";
    EXPECT_FALSE(pool.live(index)) << "and live() is the only thing that says so";
    EXPECT_EQ(pool.resolve(handle), nullptr) << "while the HANDLE stops resolving";
}

// AN INDEX IS NOT A HANDLE, and this is the confusion that panicked frame_run_create: it spent
// alloc()'s index as a handle, which resolve() answers only while that slot's generation is
// still 0. The second occupant of a slot resolved to nothing and the store went through null.
TEST(SlotPool, an_index_spent_as_a_handle_stops_resolving_after_one_recycle)
{
    constexpr int N = 4;
    kickos::SlotPool<Obj, N> pool;

    int const first = pool.alloc();
    ASSERT_EQ(first, 0) << "a fresh pool's first claim is slot 0";
    // The premise of the bug: at generation 0 the index and the handle ARE the same word, so
    // resolve(index) answers and the mistake is invisible.
    EXPECT_EQ(pool.handle_for(first), first);
    EXPECT_NE(pool.resolve(first), nullptr) << "resolve(index) answers, once";

    // Walk the ring so slot 0 is claimed a second time. free() bumps its generation.
    pool.free(pool.handle_for(first));
    int second = -1;
    for (int i = 0; i < N; i++)
    {
        int const k = pool.alloc();
        ASSERT_GE(k, 0);
        if (k == first)
        {
            second = k;
        }
    }
    ASSERT_EQ(second, first) << "the ring came back round to slot 0";

    EXPECT_NE(pool.handle_for(second), second) << "the handle now carries a generation";
    EXPECT_EQ(pool.resolve(second), nullptr)
        << "so the INDEX resolves to nothing, which is the null the creator stored through";
    EXPECT_NE(pool.at(second), nullptr) << "where at(), which takes an index, still answers";
    EXPECT_NE(pool.resolve(pool.handle_for(second)), nullptr) << "as does the real handle";
}

// handle_for takes an index too, and it is the last accessor of the set to bound one. -1 is a
// safe answer rather than a second sentinel: its low half is the reserved all-ones index, so
// the pool's own resolve() and free() already refuse it.
TEST(SlotPool, handle_for_bounds_its_index)
{
    constexpr int N = 4;
    kickos::SlotPool<Obj, N> pool;
    ASSERT_GE(pool.alloc(), 0);

    EXPECT_EQ(pool.handle_for(opaque(N)), -1) << "one past the last slot";
    EXPECT_EQ(pool.handle_for(opaque(-1)), -1) << "alloc()'s pool-full answer";
    EXPECT_EQ(pool.handle_for(opaque(0xFFFF)), -1) << "the reserved all-ones index";
    EXPECT_EQ(pool.resolve(pool.handle_for(opaque(N))), nullptr)
        << "and the refusal it answers resolves to nothing";
}

TEST(SlotPool, at_of_a_full_pools_alloc_is_the_pool_full_answer)
{
    constexpr int N = 4;
    kickos::SlotPool<Obj, N> pool;
    for (int i = 0; i < N; i++)
    {
        ASSERT_GE(pool.alloc(), 0);
    }
    // THE COLLAPSE THIS BUYS: a caller that claims and then wants the slot asks ONE question.
    // A full pool answers it with the null and needs no `index < 0` beside it.
    EXPECT_EQ(pool.at(pool.alloc()), nullptr) << "at(alloc()) on a full pool";
}

TEST(SlotPool, index_of_is_total_over_a_null_object)
{
    constexpr int N = 4;
    kickos::SlotPool<Obj, N> pool;
    int const index = pool.alloc();
    ASSERT_GE(index, 0);

    EXPECT_EQ(pool.index_of(pool.at(index)), index) << "a slot base answers its own index";
    // WHAT THE KERNEL RESTS ON: index_of(resolve(h)) is one refusal and not two, so the four
    // cap.cc helpers that turn a handle into a slot index have a single answer for "no live
    // object". The address compare below it already refuses a null on every real target; the
    // explicit test is what keeps the contract off that layout property.
    EXPECT_EQ(pool.index_of(nullptr), -1) << "no object, so no index";
    EXPECT_EQ(pool.index_of(pool.resolve(-1)), -1) << "and no index for an unresolvable handle";
}
