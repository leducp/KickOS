// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Storage + allocation-policy gate for the capability table's two code paths
// (kernel/include/kickos/cap.h): the segmented chunk directory, the all-or-nothing chunk
// reservation, and the run's free-slot list.
//
// Host-only of necessity, twice over. The chunk geometry is chosen by an #if on the summed
// table width, so one build compiles ONE geometry and this source is built once per geometry
// (CAPTABLE_GATE_WIDTH). And no cap-table slot index is observable from userspace beyond the
// low bits of a handle, so no on-target arm can see a directory mapping or a free-list length.
//
// Everything measured here goes through the header's own inline functions: cap_slot,
// CapChunkList::take/give, cap_run_free_build/peek/unlink/release.

#include <stdint.h>
#include <stdio.h>

#include <kickos/config/system.h>

// The tree compiles one geometry, summed at configure; this gate needs all of them, so it
// restates the width and the child width from its target's CAPTABLE_GATE_* before cap.h
// reads them. The generated header is included FIRST so that cap.h's own include of it is a
// no-op and cannot put the configured values back.
#include <kickos/config/cap_width.h>
#undef KICKOS_MAX_HANDLES
#define KICKOS_MAX_HANDLES CAPTABLE_GATE_WIDTH
#undef KICKOS_CAP_CHILD_WIDTH
#define KICKOS_CAP_CHILD_WIDTH CAPTABLE_GATE_CHILD
#undef KICKOS_CAP_REPLY_MAX
#define KICKOS_CAP_REPLY_MAX 1

#include <kickos/cap.h>

using kickos::cap_run_free_build;
using kickos::cap_run_free_release;
using kickos::cap_run_free_unlink;
using kickos::cap_run_held;
using kickos::cap_run_peek_free;
using kickos::cap_slot;
using kickos::CapChunkList;
using kickos::CapEntry;
using kickos::CapRun;
using kickos::CapType;
using kickos::kcap_free_index;
using kickos::kcap_free_next;
using kickos::KCAP_FREE_NONE;
using kickos::kcap_free_prev;
using kickos::kcap_free_ref;
using kickos::KCAP_NO_SLOT;
using kickos::KCAP_CHILD_CHUNKS;
using kickos::kcap_chunks_for;
using kickos::KCAP_ROOT_CHUNKS;
using kickos::KCAP_RUN_COUNT;
using kickos::KCAP_SLAB_CHUNKS;

namespace
{
    constexpr uint32_t WIDTH = KICKOS_MAX_HANDLES;
    constexpr uint32_t CHILD = KICKOS_CAP_CHILD_WIDTH;
    constexpr uint32_t FIRST = KICKOS_CAP_FIRST_DYNAMIC;
    constexpr uint32_t SPAN = WIDTH - FIRST;
    // What the widest run reserves. Not a capacity: the last chunk's tail is paid for and
    // unaddressable.
    constexpr uint32_t RUN_SLOTS = KCAP_ROOT_CHUNKS * KCAP_CHUNK_SLOTS;

    int g_failures = 0;

    void check(bool ok, char const* what)
    {
        if (ok)
        {
            return;
        }
        printf("not ok - %s\n", what);
        g_failures++;
    }

    constexpr uint32_t RUNS = 4;
    CapEntry g_slab[RUN_SLOTS * RUNS];

    // Thread `chunks` chunks onto a fresh list, lowest address first (cap_slab_init's order).
    void list_of(CapChunkList* list, uint32_t chunks)
    {
        list->head = nullptr;
        for (uint32_t c = chunks; c > 0; c--)
        {
            list->push(&g_slab[(c - 1) * KCAP_CHUNK_SLOTS]);
        }
    }

    uint32_t list_len(CapChunkList const& list)
    {
        uint32_t n = 0;
        CapEntry* p = list.head;
        while (p != nullptr)
        {
            n++;
            p = *CapChunkList::link_of(p);
        }
        return n;
    }

    void snapshot(CapChunkList const& list, CapEntry** out, uint32_t max)
    {
        uint32_t n = 0;
        CapEntry* p = list.head;
        while (p != nullptr and n < max)
        {
            out[n] = p;
            n++;
            p = *CapChunkList::link_of(p);
        }
    }

    void set_type(CapRun const& run, uint32_t index, CapType type)
    {
        cap_slot(run, index)->type = static_cast<uint8_t>(type);
    }

    bool is_empty(CapRun const& run, uint32_t index)
    {
        return cap_slot(run, index)->type == static_cast<uint8_t>(CapType::CAP_EMPTY);
    }

    // Mint into `index` the way cap_install_at does (unlink, then write), and release it the
    // way handle_close does (gen bump, empty, release at the tail).
    void install_at(CapRun const& run, uint32_t index, uint16_t* head)
    {
        cap_run_free_unlink(run, index, head);
        set_type(run, index, CapType::CAP_SEM);
    }
    void close_at(CapRun const& run, uint32_t index, uint16_t* head)
    {
        cap_slot(run, index)->gen++;
        set_type(run, index, CapType::CAP_EMPTY);
        cap_run_free_release(run, index, head);
    }
    // cap_install: the head free slot, then the write above. KCAP_NO_SLOT on a full table.
    uint32_t take(CapRun const& run, uint16_t* head)
    {
        uint32_t const index = cap_run_peek_free(*head);
        if (index == KCAP_NO_SLOT)
        {
            return KCAP_NO_SLOT;
        }
        install_at(run, index, head);
        return index;
    }

    // Zero a reserved run and thread its free list: cap_slab_attach, which take() does not do.
    uint16_t clear(CapRun const& run)
    {
        for (uint32_t i = 0; i < RUN_SLOTS; i++)
        {
            CapEntry* e = cap_slot(run, i);
            e->obj = 0;
            e->type = static_cast<uint8_t>(CapType::CAP_EMPTY);
            e->rights = 0;
            e->gen = 0;
        }
        return cap_run_free_build(run, WIDTH);
    }

    // Length, plus whether the list is a well-formed circular doubly linked list over EMPTY
    // dynamic slots only. Bounded so a cut or looped list cannot hang the gate.
    uint32_t free_list_walk(CapRun const& run, uint16_t head, bool* well_formed)
    {
        *well_formed = true;
        if (head == KCAP_FREE_NONE)
        {
            return 0;
        }
        bool seen[KICKOS_MAX_HANDLES] = {};
        uint32_t n = 0;
        uint16_t ref = head;
        while (n <= SPAN)
        {
            uint32_t const index = kcap_free_index(ref);
            if (index < FIRST or index >= WIDTH or seen[index] or not is_empty(run, index))
            {
                *well_formed = false;
                return n;
            }
            seen[index] = true;
            n++;
            CapEntry* e = cap_slot(run, index);
            // The back link must name us from the successor, or an unlink through it would
            // splice the wrong pair.
            uint16_t const next = kcap_free_next(e);
            if (kcap_free_prev(cap_slot(run, kcap_free_index(next))) != ref)
            {
                *well_formed = false;
                return n;
            }
            ref = next;
            if (ref == head)
            {
                return n;
            }
        }
        *well_formed = false;
        return n;
    }

    uint32_t free_count(CapRun const& run)
    {
        uint32_t n = 0;
        for (uint32_t i = FIRST; i < WIDTH; i++)
        {
            if (is_empty(run, i))
            {
                n++;
            }
        }
        return n;
    }

    // --- the geometry the #if selected -------------------------------------------------

    // Which geometry this binary compiled: without this, a regression that collapsed the #if
    // to one path would still pass every other case here.
    void case_geometry()
    {
        printf("# width %u (child %u): %u chunk(s) of %u = %u slot(s) reserved\n", WIDTH,
               CHILD, static_cast<unsigned>(KCAP_RUN_CHUNKS),
               static_cast<unsigned>(KCAP_CHUNK_SLOTS), static_cast<unsigned>(RUN_SLOTS));
        check(KCAP_RUN_CHUNKS == CAPTABLE_GATE_CHUNKS,
              "the #if selected the chunk count this target was built to exercise");
        check(RUN_SLOTS >= WIDTH, "a run reserves at least the addressable width");
        check(RUN_SLOTS - WIDTH < KCAP_CHUNK_SLOTS, "a run rounds up by under one chunk");
        if (KCAP_RUN_CHUNKS == 1)
        {
            check(RUN_SLOTS == WIDTH, "the flat run rounds nothing up");
        }
        check(kcap_chunks_for(WIDTH) == KCAP_ROOT_CHUNKS,
              "the widest run's chunk count is what kcap_chunks_for says of the width");
        // KCAP_CHILD_CHUNKS IS kcap_chunks_for(CHILD), so comparing the two proves nothing.
        // The ceiling property it has to have is an independent fact.
        check(KCAP_CHILD_CHUNKS * KCAP_CHUNK_SLOTS >= CHILD
                  and KCAP_CHILD_CHUNKS * KCAP_CHUNK_SLOTS - CHILD < KCAP_CHUNK_SLOTS,
              "the child run reserves the child width and rounds up by under one chunk");
        check(KCAP_CHILD_CHUNKS <= KCAP_ROOT_CHUNKS, "the child class is never the wider one");
        // EXACT, never a lower bound: a `>=` here would let a surplus term reappear in the
        // formula with nothing failing.
        check(KCAP_SLAB_CHUNKS
                  == KCAP_RUN_COUNT * KCAP_CHILD_CHUNKS + KCAP_ROOT_CHUNKS - KCAP_CHILD_CHUNKS,
              "the slab is one child-width run per holder plus root's widening, exactly");
    }

    // A run NARROWER than the widest: exactly its own chunks come off the list, and the
    // directory entries above it are cleared, so give() returns what was taken and cap_slot
    // can never reach a chunk the run does not hold.
    void case_partial_take_clears_the_tail()
    {
        if (KCAP_CHILD_CHUNKS == KCAP_ROOT_CHUNKS)
        {
            printf("# child and widest run are the same size: no partial take exists\n");
            return;
        }
        CapChunkList list;
        list_of(&list, KCAP_ROOT_CHUNKS);
        CapRun run = {};
        // Seed the tail with a live-looking pointer: a take that did not clear it would leave
        // give() returning a chunk this run never held.
        for (uint32_t c = 0; c < KCAP_RUN_CHUNKS; c++)
        {
            run.chunk[c] = &g_slab[0];
        }
        check(list.take(&run, KCAP_CHILD_CHUNKS), "a narrow run comes off the list");
        check(list_len(list) == KCAP_ROOT_CHUNKS - KCAP_CHILD_CHUNKS,
              "a narrow run takes exactly its own chunks");
        bool tail_clear = true;
        for (uint32_t c = KCAP_CHILD_CHUNKS; c < KCAP_RUN_CHUNKS; c++)
        {
            if (run.chunk[c] != nullptr)
            {
                tail_clear = false;
            }
        }
        check(tail_clear, "the directory above a narrow run is cleared");
        if (not tail_clear)
        {
            // Do NOT walk on: give() would push the seed pointer a second time, cycling the
            // free list, and list_len below would never terminate. The gate would report a
            // TIMEOUT instead of the failure just recorded.
            return;
        }
        check(cap_run_held(run), "a narrow run still reads as held");
        list.give(&run);
        check(list_len(list) == KCAP_ROOT_CHUNKS, "give() returns exactly what was taken");
        check(not cap_run_held(run), "give() clears a narrow run's directory too");
    }

    // Every addressable index maps to its own entry, and the segmented mapping is the chunk
    // and offset the split claims. The directory is built out of NON-ADJACENT chunks, so an
    // accessor that indexed one flat block fails here.
    void case_slot_mapping_is_a_bijection()
    {
        CapRun run = {};
        for (uint32_t c = 0; c < KCAP_RUN_CHUNKS; c++)
        {
            // Reverse order: chunk 0 gets the HIGHEST address, so adjacency cannot stand in
            // for the directory lookup.
            run.chunk[c] = &g_slab[(KCAP_RUN_CHUNKS - 1 - c) * KCAP_CHUNK_SLOTS];
        }
        check(cap_run_held(run), "a directory with chunk 0 set reads as held");

        bool overlap = false;
        for (uint32_t i = 0; i < WIDTH; i++)
        {
            CapEntry* want = run.chunk[i / KCAP_CHUNK_SLOTS] + (i % KCAP_CHUNK_SLOTS);
            if (cap_slot(run, i) != want)
            {
                printf("not ok - index %u maps to the wrong entry\n", i);
                g_failures++;
                return;
            }
            for (uint32_t j = 0; j < i; j++)
            {
                if (cap_slot(run, j) == cap_slot(run, i))
                {
                    overlap = true;
                }
            }
        }
        check(not overlap, "no two indices alias one entry");

        CapRun none = {};
        check(not cap_run_held(none), "an empty directory reads as no run");
    }

    // --- the reservation ---------------------------------------------------------------

    // A run short of one chunk is refused, and the refusal leaves the free list exactly as it
    // was: no chunk may be stranded in a half-built table. Only reachable when a run is more
    // than one chunk.
    void case_short_take_is_all_or_nothing()
    {
        if (KCAP_RUN_CHUNKS == 1)
        {
            printf("# flat run: a short take has no prefix to strand\n");
            return;
        }
        CapChunkList list;
        list_of(&list, KCAP_RUN_CHUNKS - 1);
        CapEntry* before[RUNS * 8] = {};
        CapEntry* after[RUNS * 8] = {};
        uint32_t const len = list_len(list);
        snapshot(list, before, RUNS * 8);

        // SEEDED, not zeroed: a zeroed CapRun cannot tell "the refusal cleared the directory"
        // from "it was already clear".
        CapRun run;
        for (uint32_t c = 0; c < KCAP_RUN_CHUNKS; c++)
        {
            run.chunk[c] = &g_slab[0];
        }
        check(not list.take(&run, KCAP_ROOT_CHUNKS), "a list short of one chunk refuses the run");
        check(not cap_run_held(run), "a refused run holds nothing");
        bool cleared = true;
        for (uint32_t c = 0; c < KCAP_RUN_CHUNKS; c++)
        {
            if (run.chunk[c] != nullptr)
            {
                cleared = false;
            }
        }
        check(cleared, "a refused run leaves no chunk in its directory");
        check(list_len(list) == len, "a refused take returns every chunk it took");
        snapshot(list, after, RUNS * 8);
        bool same = true;
        for (uint32_t i = 0; i < len; i++)
        {
            if (before[i] != after[i])
            {
                same = false;
            }
        }
        check(same, "a refused take restores the free list's ORDER, not just its length");
        if (not cleared)
        {
            return; // the give() below would push the seed pointer and cycle the list
        }
        list.give(&run);
        check(list_len(list) == len, "and give() on a refused run adds nothing to the list");
    }

    // The refusal at the FIRST chunk, on EVERY geometry including the flat one: the list is
    // empty, so not one directory entry is written and the unwind loop runs zero times. Only
    // the unconditional clear can make the postcondition hold here. Seeded for the reason
    // above: a zeroed CapRun cannot tell a cleared directory from one that never held
    // anything.
    void case_refused_take_clears_the_directory()
    {
        CapChunkList empty;
        empty.head = nullptr;
        CapRun stale;
        for (uint32_t c = 0; c < KCAP_RUN_CHUNKS; c++)
        {
            stale.chunk[c] = &g_slab[0];
        }
        check(not empty.take(&stale, KCAP_ROOT_CHUNKS), "an empty list refuses at chunk 0");
        bool cleared = true;
        for (uint32_t c = 0; c < KCAP_RUN_CHUNKS; c++)
        {
            if (stale.chunk[c] != nullptr)
            {
                cleared = false;
            }
        }
        check(cleared, "and a chunk-0 refusal clears the whole directory");
        check(not cap_run_held(stale), "so cap_run_held answers false for a run holding nothing");
        if (not cleared)
        {
            return; // give() would push the seed pointer, and the walk below would not end
        }
        empty.give(&stale);
        check(empty.head == nullptr, "and give() on it returns nothing to the free list");
    }

    // The list serves whole runs and nothing else: RUNS runs, then a refusal, and one give()
    // buys exactly one more take().
    void case_reservation_is_exact_and_reversible()
    {
        CapChunkList list;
        list_of(&list, KCAP_ROOT_CHUNKS * RUNS);
        CapRun held[RUNS] = {};
        for (uint32_t r = 0; r < RUNS; r++)
        {
            check(list.take(&held[r], KCAP_ROOT_CHUNKS),
                  "the list serves a whole run while chunks remain");
        }
        check(list.head == nullptr, "RUNS runs consume the list exactly");
        CapRun over = {};
        check(not list.take(&over, KCAP_ROOT_CHUNKS), "an empty list refuses a run");
        check(not list.take(&over, KCAP_ROOT_CHUNKS), "the refusal is idempotent");

        list.give(&held[1]);
        check(list_len(list) == KCAP_ROOT_CHUNKS, "give() returns every chunk of a run");
        check(not cap_run_held(held[1]), "give() clears the directory it returned");
        CapRun again = {};
        check(list.take(&again, KCAP_ROOT_CHUNKS), "one returned run buys exactly one more");
        check(not list.take(&over, KCAP_ROOT_CHUNKS), "and no more than one");

        list.give(&again);
        for (uint32_t r = 0; r < RUNS; r++)
        {
            list.give(&held[r]); // a run that was already given is a no-op
        }
        check(list_len(list) == KCAP_ROOT_CHUNKS * RUNS, "every chunk comes back");
    }

    // --- the free-slot list ------------------------------------------------------------

    // Reserve one run out of the slab and clear it. Every policy case below starts here.
    uint16_t fresh(CapChunkList* list, CapRun* run)
    {
        list_of(list, KCAP_ROOT_CHUNKS);
        check(list->take(run, KCAP_ROOT_CHUNKS), "the policy cases get their run");
        return clear(*run);
    }

    // A freshly threaded run holds every dynamic slot exactly once, in ascending order, and
    // nothing below the reserved plane. The list crosses chunk boundaries on the segmented
    // path, so a builder that indexed one flat block fails here.
    void case_fresh_list_is_every_dynamic_slot_ascending()
    {
        CapChunkList list;
        CapRun run = {};
        uint16_t head = fresh(&list, &run);

        bool ok = false;
        uint32_t const len = free_list_walk(run, head, &ok);
        check(ok, "a fresh list is a well-formed circular list over empty dynamic slots");
        check(len == SPAN, "a fresh list holds every dynamic slot exactly once");
        check(head == kcap_free_ref(FIRST), "and its head is the first dynamic index");

        uint16_t ref = head;
        bool ascending = true;
        for (uint32_t i = FIRST; i < WIDTH; i++)
        {
            if (kcap_free_index(ref) != i)
            {
                ascending = false;
            }
            ref = kcap_free_next(cap_slot(run, kcap_free_index(ref)));
        }
        check(ascending, "a fresh list is threaded in ascending index order");
        check(ref == head, "and closes back on its head");
    }

    // Churn one transient capability over the dynamic window while `resident` low slots stay
    // held, and report the busiest slot's share. The held slots are what separate a tail
    // release from a policy that merely LOOKS spread on an empty table.
    void case_spread_with_resident_set(uint32_t resident)
    {
        CapChunkList list;
        CapRun run = {};
        uint16_t head = fresh(&list, &run);

        for (uint32_t i = 0; i < resident; i++)
        {
            check(take(run, &head) != KCAP_NO_SLOT, "the resident set fits the dynamic window");
        }

        uint32_t counts[KICKOS_MAX_HANDLES] = {};
        uint32_t const cycles = 300;
        for (uint32_t c = 0; c < cycles; c++)
        {
            uint32_t const index = take(run, &head);
            if (index == KCAP_NO_SLOT or index >= WIDTH or index < FIRST)
            {
                printf("not ok - churn %u returned index %u\n", c, index);
                g_failures++;
                return;
            }
            counts[index]++;
            close_at(run, index, &head);
        }

        uint32_t busiest = 0;
        uint32_t used = 0;
        for (uint32_t i = 0; i < WIDTH; i++)
        {
            if (counts[i] > busiest)
            {
                busiest = counts[i];
            }
            if (counts[i] > 0)
            {
                used++;
            }
        }
        uint32_t const free_slots = SPAN - resident;
        // The transient walks the free slots in order, so the busiest takes one cycle in
        // free_slots, plus at most one for a partial final lap. First-fit takes ALL of them.
        uint32_t const bound = (cycles / free_slots) + 1;
        printf("# width %u, %u resident: %u free slot(s) shared, busiest %u of %u (bound %u)\n",
               WIDTH, resident, used, busiest, cycles, bound);
        check(used == free_slots, "every free slot takes a share of the recycles");
        check(busiest <= bound, "no slot takes more than its lap of the recycles");
    }

    // A release goes to the TAIL, so the slot handed out next is the one that has been free
    // the LONGEST. A head insertion (first-fit) breaks this while still passing an "every
    // slot gets a share" count.
    void case_release_goes_to_the_tail()
    {
        if (SPAN < 3)
        {
            printf("# width %u: too narrow to distinguish head from tail insertion\n", WIDTH);
            return;
        }
        CapChunkList list;
        CapRun run = {};
        uint16_t head = fresh(&list, &run);

        uint32_t const a = take(run, &head);
        uint32_t const b = take(run, &head);
        uint32_t const c = take(run, &head);
        check(a == FIRST and b == FIRST + 1 and c == FIRST + 2,
              "the first three mints walk the fresh list in order");
        // Release out of order: b then a. A tail release must hand them back in THAT order.
        close_at(run, b, &head);
        close_at(run, a, &head);
        // Drain the slots that were still ahead of them.
        for (uint32_t i = 0; i + 3 < SPAN; i++)
        {
            check(take(run, &head) != KCAP_NO_SLOT, "the untouched slots come first");
        }
        check(take(run, &head) == b, "the first slot released comes back first");
        check(take(run, &head) == a, "and the second one second");
        check(take(run, &head) == KCAP_NO_SLOT, "the window is then full");
    }

    // The delegation case: a caller-named index is unlinked from the MIDDLE of the list, and
    // the list must stay total over the slots that are still free. A mis-spliced unlink drops
    // or duplicates a slot here.
    void case_named_index_unlinks_from_the_middle()
    {
        if (SPAN < 3)
        {
            printf("# width %u: too narrow for a middle unlink\n", WIDTH);
            return;
        }
        CapChunkList list;
        CapRun run = {};
        uint16_t head = fresh(&list, &run);

        // Every dynamic index in turn, so the head, an interior slot and the tail are all
        // covered rather than only whichever one this width happens to make interior.
        for (uint32_t target = FIRST; target < WIDTH; target++)
        {
            head = clear(run);
            install_at(run, target, &head); // cap_install_at's placement
            bool ok = false;
            uint32_t const len = free_list_walk(run, head, &ok);
            if (not ok or len != SPAN - 1 or len != free_count(run))
            {
                printf("not ok - unlinking index %u left a list of %u (want %u, %u free)\n",
                       target, len, SPAN - 1, free_count(run));
                g_failures++;
                return;
            }
            close_at(run, target, &head);
            uint32_t const back = free_list_walk(run, head, &ok);
            if (not ok or back != SPAN)
            {
                printf("not ok - releasing index %u left a list of %u (want %u)\n", target, back,
                       SPAN);
                g_failures++;
                return;
            }
        }
        printf("# width %u: every dynamic index unlinks and returns without cutting the list\n",
               WIDTH);
    }

    // A lone free slot must be reachable whatever the rest of the table looks like, and the
    // full table must refuse without side effects.
    void case_lone_free_slot_is_reachable()
    {
        CapChunkList list;
        CapRun run = {};
        uint16_t head = fresh(&list, &run);

        for (uint32_t i = FIRST; i < WIDTH; i++)
        {
            check(take(run, &head) != KCAP_NO_SLOT, "the window fills");
        }
        check(head == KCAP_FREE_NONE, "a full window holds an empty free list");
        check(take(run, &head) == KCAP_NO_SLOT, "a full window refuses");

        for (uint32_t round = 0; round < 2; round++)
        {
            uint32_t target = WIDTH;
            while (target > FIRST)
            {
                target--;
                close_at(run, target, &head);
                uint32_t const got = take(run, &head);
                if (got != target)
                {
                    printf("not ok - lone free slot %u not found (got %u)\n", target, got);
                    g_failures++;
                    return;
                }
            }
        }
        printf("# width %u: the lone free slot is the one a full-but-one table hands back\n",
               WIDTH);
    }

    // The list never enters the reserved plane, and a refusal on a full table does not touch
    // it either. Seeded with a marker so "unchanged" is distinguishable from "empty".
    void case_reserved_plane_is_never_handed_out()
    {
        CapChunkList list;
        CapRun run = {};
        uint16_t head = fresh(&list, &run);
        for (uint32_t i = 0; i < FIRST; i++)
        {
            set_type(run, i, CapType::CAP_ENDPOINT); // the kernel's stdout seat's shape
        }

        for (uint32_t n = 0; n < SPAN; n++)
        {
            uint32_t const index = take(run, &head);
            if (index < FIRST or index >= WIDTH)
            {
                printf("not ok - claim %u landed on index %u\n", n, index);
                g_failures++;
                return;
            }
        }
        check(take(run, &head) == KCAP_NO_SLOT,
              "a table full to its last dynamic slot refuses");
        check(take(run, &head) == KCAP_NO_SLOT, "and keeps refusing");
        bool reserved_intact = true;
        for (uint32_t i = 0; i < FIRST; i++)
        {
            if (cap_slot(run, i)->type != static_cast<uint8_t>(CapType::CAP_ENDPOINT))
            {
                reserved_intact = false;
            }
        }
        check(reserved_intact, "the reserved plane is what it was before the refusals");

        // Closing a reserved slot must not put it in the list: it is the kernel's to re-seat,
        // and an own create that could land there would alias a well-known name.
        //
        // clear() zeroes every obj, so without this marker the case passes either way.
        CapEntry* reserved = cap_slot(run, KOS_CAP_STDOUT);
        reserved->obj = 0x5AA5;
        close_at(run, KOS_CAP_STDOUT, &head);
        check(head == KCAP_FREE_NONE, "releasing a reserved index adds nothing to the list");
        // Out of the list, but still a DEAD entry, so obj must carry the null link pair or the
        // "a dead entry holds its free-list links in obj" invariant is not universal.
        check(reserved->obj == 0, "and the closed reserved entry keeps no stale object handle");
        check(take(run, &head) == KCAP_NO_SLOT, "so the table is still full");
    }

    // A runless task has capacity 0, and a run whose list was never threaded is the same
    // thing: both must refuse. KCAP_FREE_NONE is 0, so a zeroed TCB fails CLOSED instead of
    // handing out slot 0, the kernel's stdout seat.
    void case_no_list_is_refused()
    {
        uint16_t head = KCAP_FREE_NONE;
        check(cap_run_peek_free(head) == KCAP_NO_SLOT, "an empty list refuses");

        CapRun none = {};
        check(cap_run_free_build(none, 0) == KCAP_FREE_NONE, "capacity 0 threads no list");
        check(cap_run_free_build(none, FIRST) == KCAP_FREE_NONE,
              "a table of nothing but reserved slots threads no list");

        CapChunkList list;
        CapRun run = {};
        list_of(&list, KCAP_ROOT_CHUNKS);
        check(list.take(&run, KCAP_ROOT_CHUNKS), "the unthreaded case gets its run");
        for (uint32_t i = 0; i < RUN_SLOTS; i++)
        {
            CapEntry* e = cap_slot(run, i);
            e->obj = 0;
            e->type = static_cast<uint8_t>(CapType::CAP_EMPTY);
            e->rights = 0;
            e->gen = 0;
        }
        uint16_t unthreaded = KCAP_FREE_NONE;
        check(cap_run_peek_free(unthreaded) == KCAP_NO_SLOT,
              "a zeroed run with no list refuses instead of offering slot 0");
    }
}

int main()
{
    case_geometry();
    case_slot_mapping_is_a_bijection();
    case_partial_take_clears_the_tail();
    case_short_take_is_all_or_nothing();
    case_refused_take_clears_the_directory();
    case_reservation_is_exact_and_reversible();
    case_no_list_is_refused();
    case_fresh_list_is_every_dynamic_slot_ascending();
    case_release_goes_to_the_tail();
    case_named_index_unlinks_from_the_middle();
    case_reserved_plane_is_never_handed_out();
    case_lone_free_slot_is_reachable();
    // 0 resident is the empty-table case every policy passes; 2 is what separates them, and
    // SPAN-1 is the single-free-slot edge.
    case_spread_with_resident_set(0);
    case_spread_with_resident_set(2);
    case_spread_with_resident_set(SPAN - 1);

    if (g_failures != 0)
    {
        printf("FAIL: %d capability-table policy check(s) failed\n", g_failures);
        return 1;
    }
    printf("ok - capability-table storage and free-list policy (width %u)\n", WIDTH);
    return 0;
}
