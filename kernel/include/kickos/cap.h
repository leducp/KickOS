// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Per-thread capability table: a typed, rights-bearing, refcounted handle naming a global
// generational object. cap_resolve is two-level: the per-thread cap-gen guard here, then the
// object pool's own object-gen guard. Object liveness is global; possession is per-thread.
//
// Locking: none internal. Every entry point's precondition is CALLER HOLDS IrqLock, and a
// resolved object pointer must be used under the SAME continuous lock. Two exceptions, both
// flagged below: cap_check_authority needs no lock, and cap_teardown's caller must NOT hold one.

#ifndef KICKOS_CAP_H
#define KICKOS_CAP_H

#include <stdint.h>

#include <kickos/config/cap_width.h> // KICKOS_MAX_HANDLES, KCAP_CHUNK_TARGET (generated)
#include <kickos/config/system.h>       // KICKOS_MAX_SPAWN_GRANTS, KICKOS_MAX_THREADS
#include <kickos/sys/cap_index.h>       // KICKOS_CAP_FIRST_DYNAMIC, KOS_CAP_AUTHORITY

// cmake/cap_table.cmake refuses a tree that would break any of these at CONFIGURE. They stay
// as the backstop for tests/unit/captable, which substitutes widths the sum never produces and is
// a legitimate consumer of this header.

// KICKOS_CAP_CHILD_WIDTH is the narrowest table in the image (root alone holds
// KICKOS_MAX_HANDLES), so the bounds below are stated against it.
static_assert(KICKOS_CAP_CHILD_WIDTH <= KICKOS_MAX_HANDLES,
              "the child width must fit the widest run the slab backs");

// The reserved range must leave at least one dynamic slot, or no own-create could ever
// succeed.
static_assert(KICKOS_CAP_CHILD_WIDTH > KICKOS_CAP_FIRST_DYNAMIC,
              "no dynamic cap slots left in a spawned child: raise KICKOS_MAX_SPAWN_GRANTS, "
              "or shrink the reserved range");

// Delegated cap i lands at child index i+1, so delegates spend the reserved plane. This assert
// keeps a full defaulted grant list inside the child width.
static_assert(KICKOS_MAX_SPAWN_GRANTS < KICKOS_CAP_CHILD_WIDTH,
              "a full grant list must fit the child table at indices 1..cap_count");

namespace kickos
{
    struct Thread; // kickos/thread.h

    // A cap handle is (gen << KCAP_INDEX_BITS) | index. The split is FIXED fleet-wide and
    // never derived from KICKOS_MAX_HANDLES, so a per-board RAM decision cannot renumber
    // the ABI. All 32 bits are spent: a live handle may have bit 31 set, so `h < 0` is not
    // an error test on one.
    static constexpr int KCAP_INDEX_BITS = 16;
    static constexpr int KCAP_GEN_BITS = 16;

    static_assert(KCAP_INDEX_BITS + KCAP_GEN_BITS == 32,
                  "the handle word is 32 bits: index and generation must exhaust it");

    static constexpr uint32_t KCAP_INDEX_MASK = (1u << KCAP_INDEX_BITS) - 1u;

    // THE CAPACITY RULE. This one index value is RESERVED and is never seated as a slot, so
    // a table holds at most KCAP_RESERVED_INDEX entries. Every word carrying it as its
    // index is then unmintable, which is what the two pseudo-handles below stand on.
    static constexpr uint32_t KCAP_RESERVED_INDEX = KCAP_INDEX_MASK;
    static_assert(KICKOS_MAX_HANDLES <= KCAP_RESERVED_INDEX,
                  "a table of 2^KCAP_INDEX_BITS slots would seat the reserved index and "
                  "mint KOS_CAP_AUTHORITY");

    // "No capability": what a refused mint leaves in its out-parameter, and the userspace
    // KOS_CAP_NONE that kos_recv_info carries for a plain send. A live handle can occupy
    // every bit, so only the capacity rule above keeps this value unmintable.
    static constexpr uint32_t KCAP_INVALID = 0xFFFFFFFFu;
    static_assert((KCAP_INVALID & KCAP_INDEX_MASK) == KCAP_RESERVED_INDEX,
                  "KCAP_INVALID must carry the reserved index");

    // The authority pseudo-handle must name nothing the codec can mint, or a real cap
    // could be narrowed as if it were the authority word.
    static_assert((KOS_CAP_AUTHORITY & KCAP_INDEX_MASK) == KCAP_RESERVED_INDEX,
                  "KOS_CAP_AUTHORITY must carry the reserved index");

    // CAP_REPLY names the parked caller by generational THREAD handle, and that handle gets
    // the WHOLE of CapEntry::obj. The caller's call_seq low byte rides the spare bits beside
    // the type and the rights instead (CapEntry below), so no width of the thread index can
    // truncate the thread generation.
    static constexpr int KCAP_REPLY_SEQ_BITS = 8;
    static constexpr int KCAP_REPLY_SEQ_LO_BITS = 5; // spare beside CapType's 3
    static constexpr int KCAP_REPLY_SEQ_HI_BITS = 3; // spare beside CapRights' 3
    static_assert(KCAP_REPLY_SEQ_LO_BITS + KCAP_REPLY_SEQ_HI_BITS == KCAP_REPLY_SEQ_BITS,
                  "the two halves of the packed call sequence must exhaust it");

    static constexpr int KCAP_TYPE_BITS = 3;
    static constexpr int KCAP_RIGHTS_BITS = 3;
    static_assert(KCAP_TYPE_BITS + KCAP_REPLY_SEQ_LO_BITS == 8,
                  "type plus its share of the call sequence must fill exactly one byte");

    enum class CapType : uint8_t
    {
        CAP_EMPTY = 0, // an unused slot: must be 0 so a zeroed TCB is an empty table
        CAP_SEM,
        CAP_MUTEX,    // PI mutex object pool
        CAP_ENDPOINT, // synchronous IPC endpoint object pool
        CAP_REPLY,    // one-shot L4-style reply cap; obj NAMES the parked caller by
                      // generational thread handle (no object pool, no refcount)
        CAP_IRQ,      // a tier-1 interrupt-line binding; `obj` names a slot in the
                      // binding pool
        CAP_FRAME,    // a RUN of physical frames; `obj` names a frame-run pool slot, and the
                      // run's physical base is a field of that object, never a number here
        CAP_ASPACE,   // an address space; `obj` names a DOMAIN slot by generational handle
        CAP_KIND_MAX  // never stored: STAYS LAST, and the assert below reads it
    };
    // Keyed on the sentinel and never on the last kind by name: an assert naming CAP_IRQ
    // passes over every kind added after it.
    static_assert(static_cast<uint8_t>(CapType::CAP_KIND_MAX) <= (1u << KCAP_TYPE_BITS),
                  "a CapType no longer fits the entry's type field: the call sequence packed "
                  "beside it would be overwritten");

    // Rights bits enforced at cap_resolve ((rights & need) == need); CAP_TRANSFER is
    // enforced at the delegate site instead.
    enum CapRights : uint8_t
    {
        CAP_WAIT = 1 << 0,    // sem_wait / sem_trywait; endpoint recv
        CAP_SIGNAL = 1 << 1,  // sem_post; endpoint send
        CAP_TRANSFER = 1 << 2 // may be delegated into a child table (section 6)
    };
    // The only place the full set is written. A bitmask has no sentinel, so a right left out
    // of this mask is one the assert below cannot see.
    static constexpr uint8_t CAP_RIGHTS_ALL = CAP_WAIT | CAP_SIGNAL | CAP_TRANSFER;
    static_assert(CAP_RIGHTS_ALL < (1u << KCAP_RIGHTS_BITS),
                  "a rights bit no longer fits the entry's rights field: the call sequence "
                  "packed beside it would be overwritten");

    // The thread's authority word, held in Thread::authority: its own field, sharing no
    // numbering with CapRights. Mirrored in <kickos/sys/abi.h> as KOS_AUTH_*; the two
    // must move together.
    enum CapAuthority : uint8_t
    {
        AUTH_MEMORY = 1 << 0,  // ram_alloc, the spawn-time MMIO window grant, mem_self_grant
        AUTH_PINMUX = 1 << 1,  // pinmux_set
        AUTH_PSTATE = 1 << 2,  // cpu_clock_set
        AUTH_IRQ = 1 << 3,     // irq_claim (the tier-1 mint), irq_attach, irq_unmask
        AUTH_SYSTEM = 1 << 4,  // shutdown, reboot
        AUTH_CONSOLE = 1 << 5  // console_publish
    };

    // One bit per distinct holder: a bit merging two holders grants each the other's power.

    // arch_periph_enable carries NO authority bit and must not be given one: it is gated on
    // the caller holding a live ARCH_MPU_DEV region whose base matches the block exactly
    // (caller_holds_mmio_block, syscall_mem.cc).

    static constexpr uint8_t CAP_AUTH_ALL = static_cast<uint8_t>(
        AUTH_MEMORY | AUTH_PINMUX | AUTH_PSTATE | AUTH_IRQ | AUTH_SYSTEM | AUTH_CONSOLE);

    // Carries the object pool's handle codec verbatim (no re-encoding). gen is bumped on
    // close: the per-thread use-after-close ABA guard.
    // INVARIANT: a cap names its object ONLY by generational handle, never by a physical
    // address or a region base, and no physaddr is ever stored here or delivered as a
    // badge or payload.
    struct alignas(8) CapEntry
    {
        // A full 32-bit generational handle: an object-pool one, or a THREAD one for a
        // CAP_REPLY. Routinely NEGATIVE, so `obj < 0` is not an error test on it. A DEAD
        // entry holds the run's free-list links here instead (kcap_free_link below).
        int32_t obj;
        uint8_t type : KCAP_TYPE_BITS;
        uint8_t seq_lo : KCAP_REPLY_SEQ_LO_BITS; // CAP_REPLY call sequence, low half
        uint8_t rights : KCAP_RIGHTS_BITS;
        uint8_t seq_hi : KCAP_REPLY_SEQ_HI_BITS; // CAP_REPLY call sequence, high half
        uint16_t gen;                            // per-slot cap generation
    };
    static_assert(sizeof(CapEntry) == 8, "CapEntry must stay 8 bytes (frozen ABI, section 5)");
    // The field layout alone gives alignof 4: the alignas is added, not implied. A dead
    // entry must hold a pointer-width free-list link on a 64-bit target.
    static_assert(alignof(CapEntry) == 8, "CapEntry must be 8-aligned, not merely 8 bytes");
    // handle_close and cap_teardown bump this counter UNMASKED, which is correct only while
    // the field width IS the storage width. One bit narrower and the slot mints handles
    // that can never resolve.
    static_assert(KCAP_GEN_BITS == 8 * sizeof(CapEntry::gen),
                  "the cap generation field must be exactly its uint16_t storage width");

    // CAP_REPLY's packing, in one place: `obj` holds the caller's thread handle whole and
    // unshifted, and only the call sequence is split.
    inline void cap_reply_seq_seat(CapEntry* e, uint8_t seq8)
    {
        e->seq_lo = static_cast<uint8_t>(seq8 & ((1u << KCAP_REPLY_SEQ_LO_BITS) - 1u));
        e->seq_hi = static_cast<uint8_t>(seq8 >> KCAP_REPLY_SEQ_LO_BITS);
    }
    inline uint8_t cap_reply_seq(CapEntry const& e)
    {
        uint32_t const hi = static_cast<uint32_t>(e.seq_hi) << KCAP_REPLY_SEQ_LO_BITS;
        return static_cast<uint8_t>(static_cast<uint32_t>(e.seq_lo) | hi);
    }
    inline uint32_t cap_reply_handle(CapEntry const& e)
    {
        return static_cast<uint32_t>(e.obj);
    }

    // A thread's table is up to KCAP_RUN_CHUNKS chunks of KCAP_CHUNK_SLOTS entries with a
    // thread-relative index, taken all-or-nothing at spawn and returned at slot reclaim. How
    // many chunks is PER THREAD: root takes KCAP_ROOT_CHUNKS, every child KCAP_CHILD_CHUNKS.
    //
    // cap_install NEVER allocates: a client mints reply capabilities into a SERVER's table, so
    // an install that could take a chunk would drain the arena at the victim's expense. Spawn is
    // the only refusal point.
    //
    // Only attach (spawn) and detach (reclaim) touch the list; cap_install, cap_lookup and
    // cap_teardown work entirely inside the thread's own run.
#if KICKOS_MAX_HANDLES <= KCAP_CHUNK_TARGET
#define KCAP_RUN_CHUNKS 1
#define KCAP_CHUNK_SLOTS KICKOS_MAX_HANDLES
#else
#define KCAP_CHUNK_SLOTS KCAP_CHUNK_TARGET
#define KCAP_CHUNK_SHIFT 3
#define KCAP_RUN_CHUNKS ((KICKOS_MAX_HANDLES + KCAP_CHUNK_SLOTS - 1) / KCAP_CHUNK_SLOTS)
#endif

#if KCAP_RUN_CHUNKS > 1
    static_assert((1u << KCAP_CHUNK_SHIFT) == KCAP_CHUNK_SLOTS,
                  "the segmented index split is a shift and a mask, so the chunk width must "
                  "be exactly 2^KCAP_CHUNK_SHIFT");
#endif

    // How many chunks a run of `width` addressable slots reserves. A task addresses exactly
    // `width` slots and cap_install refuses above it, so the last chunk's tail is reserved
    // and unreachable. Do not reclaim the tail by widening the capacity: the configure-time
    // sum would stop predicting where a table fills.
    constexpr uint32_t kcap_chunks_for(uint32_t width)
    {
#if KCAP_RUN_CHUNKS == 1
        (void)width;
        return 1;
#else
        return (width + KCAP_CHUNK_SLOTS - 1u) >> KCAP_CHUNK_SHIFT;
#endif
    }

    static constexpr uint32_t KCAP_ROOT_CHUNKS = KCAP_RUN_CHUNKS;
    static constexpr uint32_t KCAP_CHILD_CHUNKS = kcap_chunks_for(KICKOS_CAP_CHILD_WIDTH);
    static_assert(KCAP_ROOT_CHUNKS * KCAP_CHUNK_SLOTS - KICKOS_MAX_HANDLES < KCAP_CHUNK_SLOTS,
                  "a run rounds up by less than one whole chunk, or the chunk count is not "
                  "a ceiling division");

    // KCAP_RUN_OFF_POOL comes from the generated config/cap_width.h.
    static constexpr uint16_t KCAP_RUN_OFF_POOL_COUNT = KCAP_RUN_OFF_POOL;

    // One run per possible holder. A run is returned at SLOT RECLAIM and not at exit, so an
    // EXITED slot still holds its own. Short by one and a spawn is refused while a thread slot
    // is still free, which nothing downstream tells apart from a full pool: both -KOS_ENOMEM.
    static constexpr uint16_t KCAP_RUN_COUNT = KICKOS_THREAD_SLOTS + KCAP_RUN_OFF_POOL_COUNT;

    // Every run holder is GUARANTEED the child width, plus root's own widening on top: a
    // spawn can never be refused for want of a chunk, because every spawn asks for exactly
    // the child width. Nothing else is backed, and nothing else can ask.
    // MIRRORS _kickos_cap_slab in cmake/cap_table.cmake; the two must move together.
    static constexpr uint32_t KCAP_SLAB_CHUNKS =
        static_cast<uint32_t>(KCAP_RUN_COUNT) * KCAP_CHILD_CHUNKS
        + (KCAP_ROOT_CHUNKS - KCAP_CHILD_CHUNKS);

    static constexpr uint32_t kcap_slab_entries()
    {
        return KCAP_SLAB_CHUNKS * KCAP_CHUNK_SLOTS;
    }

    static_assert(KCAP_RUN_COUNT > KICKOS_THREAD_SLOTS,
                  "KCAP_RUN_COUNT wrapped its uint16_t: KICKOS_THREAD_SLOTS is within "
                  "KCAP_RUN_OFF_POOL of the type's ceiling and the slab would carve one run");

    // A task's chunk directory. It lives in the TCB, not inside the run itself.
    struct CapRun
    {
        CapEntry* chunk[KCAP_RUN_CHUNKS];
    };

    // The entry a thread-relative index names. `index` must already be bound-tested.
    inline CapEntry* cap_slot(CapRun const& run, uint32_t index)
    {
#if KCAP_RUN_CHUNKS == 1
        return run.chunk[0] + index;
#else
        return run.chunk[index >> KCAP_CHUNK_SHIFT] + (index & (KCAP_CHUNK_SLOTS - 1u));
#endif
    }

    // Does this thread hold a run at all? A run is all-or-nothing, so chunk 0 answers for the
    // whole directory. The runless set is idle, whose directory is created empty, and any
    // thread-pool slot no live thread occupies; an EXITED slot's run is returned at reclaim.
    inline bool cap_run_held(CapRun const& run)
    {
        return run.chunk[0] != nullptr;
    }

    // The slab's free chunk list: the link lives in the dead chunk itself. Not internally
    // locked; the caller holds IrqLock.
    struct CapChunkList
    {
        static_assert(sizeof(CapEntry) >= sizeof(void*),
                      "a free chunk must be able to hold its own free-list link");

        CapEntry* head;

        static CapEntry** link_of(CapEntry* chunk)
        {
            return reinterpret_cast<CapEntry**>(chunk);
        }

        void push(CapEntry* chunk)
        {
            *link_of(chunk) = head;
            head = chunk;
        }

        // Reserve `chunks` of a run: every chunk or none, and a short list is left exactly as
        // it was. Does NOT zero the chunks: the link it just overwrote is still in entry 0, so
        // the caller must clear them before the run is used as a table.
        //
        // Directory entries at and above what was taken are CLEARED, and a REFUSAL clears the
        // whole directory: an entry left standing makes cap_run_held answer true for a run
        // holding nothing, and the give() that follows injects a foreign chunk into the list.
        [[nodiscard]] bool take(CapRun* run, uint32_t chunks)
        {
            uint32_t taken = 0;
            while (taken < chunks and head != nullptr)
            {
                run->chunk[taken] = head;
                head = *link_of(head);
                taken++;
            }
            if (taken < chunks)
            {
                while (taken > 0)
                {
                    taken--;
                    push(run->chunk[taken]);
                }
            }
            // `taken` is `chunks` on success and 0 on a refusal, so one loop clears the tail
            // of a narrow run and the whole directory of a refused one.
            for (uint32_t i = taken; i < KCAP_RUN_CHUNKS; i++)
            {
                run->chunk[i] = nullptr;
            }
            return taken == chunks;
        }

        // Return every chunk of a run and clear the directory. A run that holds nothing is a
        // no-op, so an unwind path may call it unconditionally.
        void give(CapRun* run)
        {
            for (uint32_t i = 0; i < KCAP_RUN_CHUNKS; i++)
            {
                if (run->chunk[i] == nullptr)
                {
                    continue;
                }
                push(run->chunk[i]);
                run->chunk[i] = nullptr;
            }
        }
    };

    // "No slot": not an index, and distinct from every index a run of at most
    // KICKOS_MAX_HANDLES slots can form.
    static constexpr uint32_t KCAP_NO_SLOT = 0xFFFFFFFFu;

    // --- the run's free list -----------------------------------------------------------
    //
    // A CIRCULAR, DOUBLY LINKED list of the run's free DYNAMIC slots, threaded through the `obj`
    // word of the dead entries themselves: the low half is the next free slot, the high half the
    // previous, each a slot index BIASED BY ONE so 0 is the sentinel and a zeroed run carries no
    // list. Thread::cap_free_head names the head.
    //
    // The reserved index plane is never in the list, so no pop can hand a well-known index to an
    // own create.
    //
    // A release goes to the TAIL: the slot handed out next is the one free the longest, so with F
    // free slots each slot's cap-gen advances once per F mints.
    static constexpr uint16_t KCAP_FREE_NONE = 0;
    static_assert(KICKOS_MAX_HANDLES <= UINT16_MAX,
                  "a slot index biased by one must fit the uint16_t free-list link halves");

    inline uint16_t kcap_free_ref(uint32_t index)
    {
        return static_cast<uint16_t>(index + 1u);
    }
    inline uint32_t kcap_free_index(uint16_t ref)
    {
        return static_cast<uint32_t>(ref) - 1u;
    }
    inline uint16_t kcap_free_next(CapEntry const* e)
    {
        return static_cast<uint16_t>(static_cast<uint32_t>(e->obj) & 0xFFFFu);
    }
    inline uint16_t kcap_free_prev(CapEntry const* e)
    {
        return static_cast<uint16_t>(static_cast<uint32_t>(e->obj) >> 16);
    }
    inline void kcap_free_link(CapEntry* e, uint16_t prev, uint16_t next)
    {
        e->obj = static_cast<int32_t>((static_cast<uint32_t>(prev) << 16)
                                      | static_cast<uint32_t>(next));
    }
    inline void kcap_free_set_next(CapEntry* e, uint16_t next)
    {
        kcap_free_link(e, kcap_free_prev(e), next);
    }
    inline void kcap_free_set_prev(CapEntry* e, uint16_t prev)
    {
        kcap_free_link(e, prev, kcap_free_next(e));
    }

    // Thread every dynamic slot of a freshly zeroed run onto the list in ascending order and
    // return the head. The ONE writer of the initial order.
    inline uint16_t cap_run_free_build(CapRun const& run, uint32_t capacity)
    {
        uint32_t const first = KICKOS_CAP_FIRST_DYNAMIC;
        if (capacity <= first)
        {
            return KCAP_FREE_NONE; // no run, or no dynamic slot in it
        }
        uint32_t const last = capacity - 1u;
        for (uint32_t i = first; i <= last; i++)
        {
            uint32_t prev = i - 1u;
            uint32_t next = i + 1u;
            if (i == first)
            {
                prev = last;
            }
            if (i == last)
            {
                next = first;
            }
            kcap_free_link(cap_slot(run, i), kcap_free_ref(prev), kcap_free_ref(next));
        }
        return kcap_free_ref(first);
    }

    // The head free slot WITHOUT taking it, or KCAP_NO_SLOT when the table is full.
    inline uint32_t cap_run_peek_free(uint16_t head)
    {
        if (head == KCAP_FREE_NONE)
        {
            return KCAP_NO_SLOT;
        }
        return kcap_free_index(head);
    }

    // Take `index` out of the list. An index below the first dynamic one is a no-op: the
    // reserved plane is not in the list, yet default spawn placement seats delegated cap 0 on
    // index 1 (KOS_CAP_CLOCK). Every other index MUST already be linked.
    inline void cap_run_free_unlink(CapRun const& run, uint32_t index, uint16_t* head)
    {
        if (index < KICKOS_CAP_FIRST_DYNAMIC)
        {
            return;
        }
        CapEntry* e = cap_slot(run, index);
        uint16_t const self = kcap_free_ref(index);
        uint16_t const p = kcap_free_prev(e);
        uint16_t const n = kcap_free_next(e);
        if (n == self)
        {
            // A node whose successor is itself is the list's ONLY node, so it is necessarily
            // the head: clearing the head here needs no test of it.
            *head = KCAP_FREE_NONE;
            return;
        }
        kcap_free_set_next(cap_slot(run, kcap_free_index(p)), n);
        kcap_free_set_prev(cap_slot(run, kcap_free_index(n)), p);
        if (*head == self)
        {
            *head = n;
        }
    }

    // Put `index` back at the TAIL (see the release note above). Reserved indices stay out
    // of the list, so closing the kernel's stdout slot leaves it empty and unreachable to
    // an own create.
    inline void cap_run_free_release(CapRun const& run, uint32_t index, uint16_t* head)
    {
        if (index < KICKOS_CAP_FIRST_DYNAMIC)
        {
            // Out of the list, but still a DEAD entry, so it must not keep the object handle
            // it named: leave the null link pair a zeroed run carries. That keeps "a dead
            // entry's obj holds the free-list links" true of every dead entry.
            kcap_free_link(cap_slot(run, index), KCAP_FREE_NONE, KCAP_FREE_NONE);
            return;
        }
        uint16_t const self = kcap_free_ref(index);
        CapEntry* e = cap_slot(run, index);
        if (*head == KCAP_FREE_NONE)
        {
            kcap_free_link(e, self, self);
            *head = self;
            return;
        }
        CapEntry* h = cap_slot(run, kcap_free_index(*head));
        uint16_t const tail = kcap_free_prev(h);
        kcap_free_link(e, tail, *head);
        kcap_free_set_next(cap_slot(run, kcap_free_index(tail)), self);
        kcap_free_set_prev(h, self);
    }

    // Reserve a run WIDE ENOUGH for `width` addressable slots and thread its free list: false
    // leaves `run` empty, `*free_head` KCAP_FREE_NONE, `*out_width` 0 and the slab untouched. On
    // success `*out_width` is the capacity actually seated, which on the flat path is the ceiling
    // whatever was asked. Caller holds IrqLock. The width travels with the head: a capacity left
    // naming a run this call did not build has cap_lookup index a null chunk pointer.
    [[nodiscard]] bool cap_slab_attach(CapRun* run, uint32_t width, uint16_t* free_head,
                                       uint16_t* out_width);

    // Return a run to the slab and clear `*free_head` AND `*out_width`. Caller holds IrqLock.
    // A run holding nothing is a no-op, so an unwind path may call it unconditionally. A head
    // left naming a slot in a chunk this call gave away answers cap_run_peek_free for a table
    // that no longer exists.
    void cap_slab_detach(CapRun* run, uint16_t* free_head, uint16_t* out_width);

    // Carve the slab and thread the free list. Call once, before any thread exists.
    void cap_slab_init();

    // Forget the published stdout target, so no cap_install_defaults seat and no note-site
    // comparison can match a handle from a previous life. It drops the NAME and not the kernel's
    // reference on that endpoint, so it is sound ONLY as part of resetting the endpoint pool.
    void cap_console_reset();

    // The one resolve chokepoint: validate a per-thread cap handle and return the named
    // global object, or nullptr (bad index, empty, stale cap-gen, wrong type, or
    // missing rights). Returns void* (dispatch-on-type over the object pools); the
    // caller casts to the type it asked for. CAP_SEM/CAP_MUTEX/CAP_ENDPOINT all resolve.
    void* cap_resolve(Thread* c, uint32_t cap_handle, CapType want, uint8_t need);

    // As cap_resolve, but distinguishes WHY it failed so a syscall can return the right
    // taxonomy code: on nullptr, *err is KOS_EBADF (bad index / empty / stale gen /
    // wrong type / stale object) or KOS_EPERM (the cap lacks a required right). *err is
    // 0 on success. cap_resolve is this with the reason discarded.
    void* cap_resolve_e(Thread* c, uint32_t cap_handle, CapType want, uint8_t need, int* err);

    // Validate a cap handle and return its table entry (type-agnostic; for delegation
    // and close). nullptr on bad index / empty / stale cap-gen.
    CapEntry* cap_lookup(Thread* c, uint32_t cap_handle);

    // Install a cap naming `obj_handle` into the head free slot of c's table. 0 and the cap
    // handle in *out_cap (cap-gen << KCAP_INDEX_BITS | index), or -KOS_EMFILE with *out_cap left
    // KCAP_INVALID when the table is full.
    //
    // Allocates nothing, and must not start: a client drives this on a SERVER's table, so a full
    // run has to refuse rather than reach for a chunk. Does not touch the object refcount, which
    // the caller owns. Indices 0 .. KICKOS_CAP_FIRST_DYNAMIC-1 are the reserved range, which the
    // free list never holds.
    [[nodiscard]] int cap_install(Thread* c, int obj_handle, CapType type, uint8_t rights,
                                  uint32_t* out_cap);

    // Install a cap at a SPECIFIC index: delegation's deterministic placement (B1: delegated
    // cap i -> child index i+1). Does NOT touch the refcount. The slot must be EMPTY, and it
    // is asserted: writing over a live entry would leak its reference, and unlinking a slot
    // the free list does not hold would cut the list in two.
    void cap_install_at(Thread* c, int index, int obj_handle, CapType type, uint8_t rights);

    // Mint a one-shot CAP_REPLY into c's table naming parked caller `caller`: its whole
    // 32-bit generational thread handle in the entry's obj, its call_seq low byte in the
    // entry's spare bits (cap_reply_seq_seat). Refuses -KOS_EMFILE with *out_cap left
    // KCAP_INVALID when the table is full OR c already holds KICKOS_CAP_REPLY_MAX live reply
    // caps. `caller` MUST be a thread-pool slot, which every thread that can reach a syscall
    // is: idle is the one TCB outside the pool and it issues none. Caller holds IrqLock.
    [[nodiscard]] int cap_install_reply(Thread* c, Thread* caller, uint32_t* out_cap);

    // Live inbound CAP_REPLY entries in c's table. O(1) where the counter is stored; on the
    // flat path a scan bounded by KCAP_CHUNK_TARGET. Caller holds IrqLock: the flat path
    // reads a PEER's whole table, and the segmented one a counter a peer increments.
    uint32_t cap_reply_live(Thread const* c);

    // Account one CAP_REPLY entry leaving c's table. EVERY release path that empties one must
    // call it, or c's next caller is refused against a capability that is already gone. A
    // no-op where cap_reply_live counts by scanning. Caller holds IrqLock.
    void cap_reply_released(Thread* c);

    // The probe-before-mint predicate for the reply cap: never pop a receiver a reply cap cannot
    // be minted into. True iff c has a free dynamic slot AND is below KICKOS_CAP_REPLY_MAX live
    // reply caps. Caller holds IrqLock. Must not grow a cap_run_held test: a runless thread's
    // head is already KCAP_FREE_NONE.
    bool cap_can_take_reply(Thread* c);

    // Type-agnostic close: bump the slot's cap-gen (stale the handle), empty the entry,
    // then drop one reference to the named object (freeing it at refs -> 0). Returns 0,
    // or -KOS_EBADF if the handle does not resolve. Succeeds while other holders remain open.
    int handle_close(Thread* c, uint32_t cap_handle);

    // Slots released per IrqLock hold in cap_teardown: the cap on the interrupt-masked window.
    // At or above KICKOS_CAP_CHILD_WIDTH every sweep takes exactly one chunk and the
    // release-and-resume path becomes dead code no board exercises.
    static constexpr int KCAP_TEARDOWN_CHUNK = 4;
    static_assert(KCAP_TEARDOWN_CHUNK < KICKOS_CAP_CHILD_WIDTH,
                  "teardown chunk must not span a spawned child's whole table, or no board "
                  "ever exercises the preemption point");

    // Exit teardown: close every non-EMPTY handle before the TCB slot is reclaimable, or the
    // thread leaks its object references. A close that would drop refs to 0 with a waiter still
    // parked floors refs at 1 rather than stranding.
    //
    // PREEMPTIBLE, and the ONE entry point here whose caller must NOT hold IrqLock: it takes and
    // releases its own, KCAP_TEARDOWN_CHUNK slots at a time. `c->dying` must already be set,
    // which is what keeps the sweep safe across the gaps: the slot is not reclaimable, no peer
    // can mint a cap into the table, and c stays on the ready structure, so a preempted sweep
    // resumes and stays TOTAL.
    //
    // EVERY CAP_IRQ ENTRY IS RELEASED BEFORE THE FIRST GAP, in the same masked window as the
    // depth bump: a line released inside the chunked loop would be observable as not-yet-released
    // by the supervisor that loop's own EPIPE wake releases. That pass may not be chunked, so it
    // may not scan either: Thread::cap_irq_live gates it. Every writer of a CAP_IRQ entry must
    // move that count, or a line is held past the first gap.
    void cap_teardown(Thread* c);

    // True while any thread is inside cap_teardown. NOT a statement about IRQ ownership: a
    // counted sweep has already released every line it held (cap_teardown above), so the
    // exit_current retry it still gates is conservative rather than load-bearing.
    bool cap_teardown_active();

    // Seat (or re-seat) thread t's reserved stdout slot (index 0) as a SEND-ONLY (CAP_SIGNAL)
    // copy of console endpoint `target`. This and cap_install_defaults are the only paths that
    // seat the reserved slot, and neither bumps its cap-gen; a thread that closes handle 0 itself
    // does bump it, and a re-seat afterwards no longer answers KOS_CAP_STDOUT. Takes the new ref
    // BEFORE dropping any prior one, so re-seating the same endpoint never transiently frees it.
    // Caller holds IrqLock. False = nothing seated and any prior seat left exactly as it was.
    bool cap_seat_stdout(Thread* t, int target);

    // The privileged default cap set for a freshly spawned child. Pre-publish it installs
    // NOTHING (index 0 empty; write() falls back to kconsole_write). Post-publish it seats
    // a send-only (CAP_SIGNAL) copy of the console endpoint at index 0. See D4.
    void cap_install_defaults(Thread* child);

    // Move the kernel's stdout-target ref to `obj_handle` AND seat `publisher`'s own slot 0 on
    // it. Caller holds IrqLock. Both refs are taken before either seat moves, and the new ones
    // before the old are dropped, so a re-publish of the same endpoint never transiently frees it
    // and a ceiling refusal leaves the prior arrangement intact. The kernel's own ref carries
    // rights 0, the publisher's CAP_SIGNAL. False = nothing changed.
    bool cap_console_publish(Thread* publisher, int obj_handle);

    // The published console endpoint's global handle. False = nothing has been published, and
    // *out is untouched. The sentinel stays private to cap.cc: it is tested by EQUALITY, never
    // by sign, because a live handle whose slot generation has reached 32768 is NEGATIVE.
    bool cap_console_target(int* out);

    // Hand kernel-owned text to the published console endpoint, and ONLY to a receiver already
    // parked: the caller must not park, so there is no send_waiters arm, no deadline and no
    // retry. Returns the bytes handed over, or 0.
    //
    // NEITHER REFUSAL MAY BE AN ASSERT: this runs inside the fault reporter, so a panic here
    // re-enters kputs -> kconsole_write from inside the report it was writing. Neither may be an
    // early return: the receiver is off recv_waiters before the first copy, so a refusal answers
    // zero and still wakes it.
    //
    // `len` is NOT range-checked; the bound is the receiver's, the copy being min(len, w->ipc.len)
    // over a capacity endpoint_recv clamps to KOS_EP_MSG_MAX.
    //
    // Takes its own IrqLock and may wake a strictly higher-priority receiver, so the caller must
    // tolerate being switched out mid-record, in ordinary thread context with no IrqLock held.
    int32_t cap_console_deliver(char const* buf, size_t len);

    // Bump one reference to the object named by a global handle. The handle MUST resolve: the
    // caller validated it. Caller holds IrqLock. `rights` is the cap's rights bits: the endpoint
    // arm bumps recv_holders when they carry CAP_WAIT; the sem/mutex arms ignore it.
    //
    // False = REFUSED: a counter this bump would move is at its uint8_t ceiling, so neither moved.
    // True also covers "no counter to move".
    [[nodiscard]] bool obj_ref_inc(CapType type, int obj_handle, uint8_t rights);

    // The exact inverse of a SUCCESSFUL obj_ref_inc with the same arguments. For unwinding a
    // partially-taken batch (the spawn delegation loop), never as a general release: it runs
    // no close protocol and frees nothing. Caller holds IrqLock.
    void obj_ref_undo(CapType type, int obj_handle, uint8_t rights);

    // The single authority chokepoint: may thread `c` ask the kernel to do `need` (one or more
    // AUTH_* bits)? True if it is privileged, or if its authority word carries every requested
    // bit. nullptr is false. The ONE cap.h entry point that does NOT require IrqLock: it reads
    // `c`'s own TCB word, written only by that thread or by its parent before it first runs.
    bool cap_check_authority(Thread* c, uint8_t need);

    // Seat (or re-seat) thread t's authority word. Non-delegable: it is TCB state, not a
    // capability, so there is no entry a cap_grant could copy. The kernel is its only
    // writer. auth == 0 clears it. Caller holds IrqLock.
    void cap_seat_authority(Thread* t, uint8_t auth);

    // Narrow thread c's authority word in place: auth &= mask, never widening (the same
    // rule a cap_grant mask and kos_thread_params::authority obey). `cap_handle` must be
    // KOS_CAP_AUTHORITY. Returns 0, -KOS_EBADF (c holds no authority), or -KOS_EINVAL
    // (the handle names something else). Caller holds IrqLock.
    int cap_narrow_authority(Thread* c, uint32_t cap_handle, uint8_t mask);

    // Resolve a generational thread handle plus a call sequence to the parked caller thread,
    // or nullptr if it is stale. The full one-shot guard: index in range, thread-gen match,
    // state == BLOCKED, call_state == REPLY_WAIT, and seq8 matching the caller's live
    // call_seq. THE ONE BODY OF THAT GUARD: a CAP_REPLY entry carries the pair, and so does a
    // far node's reply tag, and a second copy of these clauses is a second answer to whether
    // a reply may land. Caller holds IrqLock. Decodes through UNSIGNED shifts: a fully aged
    // thread generation sets bit 31, and an arithmetic shift would corrupt it.
    //
    // TOTAL OVER A ZEROED ThreadPool, which is what a node running no kernel of its own has:
    // `next` is 0 there, so the first clause refuses every index.
    Thread* cap_reply_thread(uint32_t handle, uint8_t seq8);

    // The guard above over what a CAP_REPLY entry carries. Used by kos_reply and the
    // reply-cap death arm.
    Thread* cap_reply_caller(CapEntry const& e);
}

#endif
