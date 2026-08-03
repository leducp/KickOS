// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Per-task capability table: a typed, rights-bearing, refcounted handle NAMING a global
// generational object (semaphore, PI mutex, IPC endpoint, IRQ binding). A CapEntry stores
// (global-object-handle, type, rights) and cap_resolve is two-level: the per-task cap-gen
// guard here, then the object pool's own object-gen guard. Object liveness is a global
// property (the pool plus its parallel refs[]); capability possession is per-task.
//
// Locking: none internal. Every entry point's precondition is CALLER HOLDS IrqLock, and a
// resolved object pointer must be used under the SAME continuous lock. That, with the
// pin-by-own-cap refcount invariant, is why one holder's handle_close cannot free an
// object another holder just resolved. Two exceptions, both flagged below:
// cap_check_authority needs no lock, and cap_teardown's caller must NOT hold one.

#ifndef KICKOS_CAP_H
#define KICKOS_CAP_H

#include <stdint.h>

#include <kickos/config/system.h> // KICKOS_MAX_HANDLES (the codec must address it)
#include <kickos/sys/cap_index.h> // KICKOS_CAP_FIRST_DYNAMIC, KOS_CAP_AUTHORITY

// The reserved range must leave at least one dynamic slot, or no own-create could ever
// succeed. Board floor is 7: FIRST_DYNAMIC, main's 2 permanent caps, a 3-own-cap test peak.
static_assert(KICKOS_MAX_HANDLES > KICKOS_CAP_FIRST_DYNAMIC,
              "no dynamic cap slots left: raise KICKOS_MAX_HANDLES or shrink the reserved range");

// Delegated cap i lands at child index i+1. This assert is the ONLY bound thread_spawn's
// grant loop relies on; there is no runtime check.
static_assert(KICKOS_MAX_SPAWN_GRANTS < KICKOS_MAX_HANDLES,
              "a full grant list must fit the child table at indices 1..cap_count");

namespace kickos
{
    struct Thread; // kickos/thread.h: embeds CapEntry handles[]; cap fns take Thread*

    // Bounds the generation half of the handle word: CapEntry::gen is a uint16, so a
    // wider remainder cannot be spent.
    static constexpr int KCAP_GEN_BITS = 16;

    // Smallest index field the codec ever uses; the field only ever grows. Narrowing it
    // renumbers every handle value on the small boards and buys nothing, since the
    // generation is capped by its uint16 storage and not by the remainder.
    static constexpr int KCAP_INDEX_BITS_MIN = 4;

    static constexpr int kcap_index_bits_for(int slots)
    {
        int bits = KCAP_INDEX_BITS_MIN;
        while ((1 << bits) < slots)
        {
            bits = bits + 1;
        }
        return bits;
    }

    // Index field width, DERIVED so that a per-board KICKOS_MAX_HANDLES override cannot
    // outgrow it and make cap_install seat a slot the codec cannot address (the index
    // would wrap mod 2^bits and alias a live cap). The rest of the word is the cap
    // generation, and the field grows only up to the sign wall below.
    static constexpr int KCAP_INDEX_BITS = kcap_index_bits_for(KICKOS_MAX_HANDLES);

    // THE SIGN WALL. A cap handle comes back through a syscall whose negative returns are
    // error codes, so the widest encodable handle must stay positive in int32. At 15 index
    // bits it is exactly INT32_MAX; a 16th bit makes every wide handle negative and so
    // indistinguishable from an error return.
    static_assert(KCAP_INDEX_BITS + KCAP_GEN_BITS <= 31,
                  "cap handle would go negative in int32: KICKOS_MAX_HANDLES exceeds 2^15");
    static_assert((static_cast<uint32_t>(0xFFFFu) << 15 | 0x7FFFu) == 0x7FFFFFFFu,
                  "15 index bits + 16 generation bits is exactly INT32_MAX");
    static_assert(KICKOS_MAX_HANDLES <= (1 << KCAP_INDEX_BITS),
                  "cap handle index field cannot address the whole table");

    // The authority pseudo-handle must name nothing the codec can mint, or a real cap
    // could be narrowed as if it were the authority word.
    static_assert(KOS_CAP_AUTHORITY
                      > ((static_cast<int64_t>((1 << KCAP_GEN_BITS) - 1) << KCAP_INDEX_BITS)
                         | (KICKOS_MAX_HANDLES - 1)),
                  "KOS_CAP_AUTHORITY collides with an encodable cap handle");

    enum class CapType : uint8_t
    {
        CAP_EMPTY = 0, // an unused slot: must be 0 so a zeroed TCB is an empty table
        CAP_SEM,
        CAP_MUTEX,    // PI mutex object pool
        CAP_ENDPOINT, // synchronous IPC endpoint object pool
        CAP_REPLY,    // one-shot L4-style reply cap; obj NAMES the parked caller by
                      // generational thread handle (no object pool, no refcount)
        CAP_IRQ       // a tier-1 interrupt-line binding; `obj` names a slot in the
                      // binding pool
    };

    // Rights bits enforced at cap_resolve ((rights & need) == need); CAP_TRANSFER is
    // enforced at the delegate site instead.
    enum CapRights : uint8_t
    {
        CAP_WAIT = 1 << 0,    // sem_wait / sem_trywait; endpoint recv
        CAP_SIGNAL = 1 << 1,  // sem_post; endpoint send
        CAP_TRANSFER = 1 << 2 // may be delegated into a child table (section 6)
    };

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

    // Where the cut falls: docs/design-unprivileged-root.md section 5.1.

    // arch_periph_enable carries NO authority bit and must not be given one: it is gated on
    // the caller holding a live ARCH_MPU_DEV region whose base matches the block exactly
    // (caller_holds_mmio_block, syscall_mem.cc). See design-unprivileged-root.md section 7.

    static constexpr uint8_t CAP_AUTH_ALL = static_cast<uint8_t>(
        AUTH_MEMORY | AUTH_PINMUX | AUTH_PSTATE | AUTH_IRQ | AUTH_SYSTEM | AUTH_CONSOLE);

    // Carries the object pool's handle codec verbatim (no re-encoding). gen is bumped on
    // close: the per-task use-after-close ABA guard.
    // INVARIANT: a cap names its object ONLY by generational handle, never by a physical
    // address or a region base, and no physaddr is ever stored here or delivered as a
    // badge or payload.
    struct CapEntry
    {
        int32_t obj;    // global generational object handle (WRAP target); ignored if EMPTY.
                        // A CAP_REPLY packs a 24-bit thread handle plus an 8-bit call
                        // sequence here and is routinely NEGATIVE: no field of this entry
                        // may be narrowed without re-cutting that packing.
        uint8_t type;   // CapType
        uint8_t rights; // CapRights bits
        uint16_t gen;   // per-slot cap generation
    };
    static_assert(sizeof(CapEntry) == 8, "CapEntry must stay 8 bytes (frozen ABI, section 5)");

    // A task's table is a contiguous RUN of CapEntry with a task-relative index, taken from
    // a statically partitioned slab at spawn and returned at slot reclaim.
    //
    // Fixed size classes, no splitting and no coalescing, so the slab CANNOT fragment: a
    // freed run returns to its class list whole, and per-class refusal depends only on the
    // concurrent multiset of requests, never on order or history.
    //
    // Only attach (spawn) and detach (reclaim) touch the slab; cap_install, cap_lookup and
    // cap_teardown work entirely inside the task's own run, so a client driving a server's
    // reply mint can fill the SERVER's run and reach nothing else.
    struct CapClass
    {
        uint16_t slots;
        uint16_t count;
    };

    static constexpr CapClass KCAP_CLASSES[] = {
        {KICKOS_CAP_CLASS0_SLOTS, KICKOS_CAP_CLASS0_COUNT},
        {KICKOS_CAP_CLASS1_SLOTS, KICKOS_CAP_CLASS1_COUNT},
        {KICKOS_CAP_CLASS2_SLOTS, KICKOS_CAP_CLASS2_COUNT}};
    static constexpr int KCAP_CLASS_MAX = 3;

    static constexpr int kcap_class_count()
    {
        int n = 0;
        for (int i = 0; i < KCAP_CLASS_MAX; i++)
        {
            if (KCAP_CLASSES[i].count != 0 and KCAP_CLASSES[i].slots != 0)
            {
                n = n + 1;
            }
        }
        return n;
    }

    static constexpr uint32_t kcap_slab_entries()
    {
        uint32_t total = 0;
        for (int i = 0; i < KCAP_CLASS_MAX; i++)
        {
            if (KCAP_CLASSES[i].count != 0 and KCAP_CLASSES[i].slots != 0)
            {
                total = total
                        + static_cast<uint32_t>(KCAP_CLASSES[i].slots)
                              * static_cast<uint32_t>(KCAP_CLASSES[i].count);
            }
        }
        return total;
    }

    static constexpr uint16_t kcap_smallest_class_slots()
    {
        uint16_t s = 0xFFFFu;
        for (int i = 0; i < KCAP_CLASS_MAX; i++)
        {
            if (KCAP_CLASSES[i].count != 0 and KCAP_CLASSES[i].slots != 0
                and KCAP_CLASSES[i].slots < s)
            {
                s = KCAP_CLASSES[i].slots;
            }
        }
        return s;
    }

    static constexpr uint16_t kcap_largest_class_slots()
    {
        uint16_t s = 0;
        for (int i = 0; i < KCAP_CLASS_MAX; i++)
        {
            if (KCAP_CLASSES[i].count != 0 and KCAP_CLASSES[i].slots > s)
            {
                s = KCAP_CLASSES[i].slots;
            }
        }
        return s;
    }

    // Live classes are CONTIGUOUS from 0 (the no-gap assert below), which is what lets every
    // loop here run to kcap_class_count() instead of to KCAP_CLASS_MAX.
    static constexpr int KCAP_CLASSES_LIVE = kcap_class_count();

    static_assert(kcap_class_count() >= 1, "class 0 is required: a board must offer one run size");
    // The selftest derives `cap_capacity`'s PARTIAL permission from the CMake variable, so a
    // mix that reached the compiler by any other route would make that expectation silently
    // wrong instead of failing here.
    static_assert((KICKOS_CAP_MULTICLASS != 0) == (kcap_class_count() > 1),
                  "KICKOS_CAP_MULTICLASS disagrees with the class table: a board's mix and its "
                  "test expectation must be declared together in the root CMakeLists");
    static_assert(KICKOS_CAP_CLASS2_COUNT == 0 or KICKOS_CAP_CLASS1_COUNT != 0,
                  "cap classes may not have a gap: class 2 requires class 1");
    // Ascending, and enforced rather than assumed: attach() picks the FIRST class that
    // fits, which is only the smallest fitting class if the table is sorted.
    static_assert(KICKOS_CAP_CLASS1_COUNT == 0 or KICKOS_CAP_CLASS1_SLOTS > KICKOS_CAP_CLASS0_SLOTS,
                  "cap classes must be strictly ascending in slots");
    static_assert(KICKOS_CAP_CLASS2_COUNT == 0 or KICKOS_CAP_CLASS2_SLOTS > KICKOS_CAP_CLASS1_SLOTS,
                  "cap classes must be strictly ascending in slots");
    // The codec's index field is derived from KICKOS_MAX_HANDLES, so no class may exceed
    // it or a seated slot would be unaddressable.
    static_assert(KICKOS_CAP_CLASS0_SLOTS <= KICKOS_MAX_HANDLES
                      and KICKOS_CAP_CLASS1_SLOTS <= KICKOS_MAX_HANDLES
                      and KICKOS_CAP_CLASS2_SLOTS <= KICKOS_MAX_HANDLES,
                  "a cap class larger than KICKOS_MAX_HANDLES is not addressable by the codec");
    // Every spawned child gets a stdout seat at index 0 and its delegates above it, so the
    // smallest class has to hold the reserved plane at minimum.
    static_assert(kcap_smallest_class_slots() > KICKOS_CAP_FIRST_DYNAMIC,
                  "the smallest cap class cannot hold the reserved plane plus one dynamic slot");
    // kmain asks the slab for a KICKOS_MAX_HANDLES run for root and kpanics if none fits, so
    // a mix whose widest class stops short of the ceiling does not degrade: the board does
    // not boot. Equality, not >=, because a class wider than the ceiling is unaddressable
    // (asserted above).
    static_assert(kcap_largest_class_slots() == KICKOS_MAX_HANDLES,
                  "some cap class must be exactly KICKOS_MAX_HANDLES: root asks the slab for "
                  "that run at boot and kpanics when no class can serve it");
    static_assert(KICKOS_CAP_DEFAULT_CAPACITY <= kcap_largest_class_slots(),
                  "the default spawn capacity exceeds the largest class");

    // Take a run of at least `want` entries. REFUSE, NEVER SPILL: nullptr when the class
    // that fits is empty, even if a wider class has runs free. On success *cls receives the
    // class index and *capacity the run's real (rounded-up) size. Caller holds IrqLock.
    CapEntry* cap_slab_attach(uint16_t want, uint8_t* cls, uint16_t* capacity);

    // Return a run to its class. Caller holds IrqLock. A null run is a no-op, so an
    // unwind path may call it unconditionally.
    void cap_slab_detach(CapEntry* run, uint8_t cls);

    // Carve the slab and thread the per-class free lists. Called once from kmain before
    // any thread exists.
    void cap_slab_init();

    // The one resolve chokepoint: validate a per-task cap handle and return the named
    // global object, or nullptr (bad index, empty, stale cap-gen, wrong type, or
    // missing rights). Returns void* (dispatch-on-type over the object pools); the
    // caller casts to the type it asked for. CAP_SEM/CAP_MUTEX/CAP_ENDPOINT all resolve.
    void* cap_resolve(Thread* c, int cap_handle, CapType want, uint8_t need);

    // As cap_resolve, but distinguishes WHY it failed so a syscall can return the right
    // taxonomy code: on nullptr, *err is KOS_EBADF (bad index / empty / stale gen /
    // wrong type / stale object) or KOS_EPERM (the cap lacks a required right). *err is
    // 0 on success. cap_resolve is this with the reason discarded.
    void* cap_resolve_e(Thread* c, int cap_handle, CapType want, uint8_t need, int* err);

    // Validate a cap handle and return its table entry (type-agnostic; for delegation
    // and close). nullptr on bad index / empty / stale cap-gen.
    CapEntry* cap_lookup(Thread* c, int cap_handle);

    // Install a cap naming `obj_handle` into the first free slot of c's table. Returns
    // the cap handle (cap-gen << KCAP_INDEX_BITS | index), or -1 if the table is full.
    // Does NOT touch the object refcount: the caller owns that (sem_create sets refs=1
    // at alloc). Indices 0 .. KICKOS_CAP_FIRST_DYNAMIC-1 are the reserved range
    // (0 = kernel stdout, B3): the scan starts at KICKOS_CAP_FIRST_DYNAMIC, so an own
    // create never lands in the reserved range (own caps live in [FIRST_DYNAMIC .. MAX-1]).
    int cap_install(Thread* c, int obj_handle, CapType type, uint8_t rights);

    // Install a cap at a SPECIFIC (assumed-free) index: delegation's deterministic
    // placement (B1: delegated cap i -> child index i+1). Does NOT touch the refcount.
    void cap_install_at(Thread* c, int index, int obj_handle, CapType type, uint8_t rights);

    // Type-agnostic close: bump the slot's cap-gen (stale the handle), empty the entry,
    // then drop one reference to the named object (freeing it at refs -> 0). Returns 0,
    // or -KOS_EBADF if the handle does not resolve. Succeeds while other holders remain open.
    int handle_close(Thread* c, int cap_handle);

    // Slots released per IrqLock hold in cap_teardown: the cap on the interrupt-masked
    // window, not on the sweep's total work.
    //
    // It must stay below the SMALLEST CAP CLASS on the board, not below KICKOS_MAX_HANDLES.
    // A task's sweep is bounded by the run it was given, so at or above the smallest class
    // the tasks holding that class take exactly one chunk and the release-and-resume path
    // becomes dead code that no board exercises.
    static constexpr int KCAP_TEARDOWN_CHUNK = 4;
    static_assert(KCAP_TEARDOWN_CHUNK < kcap_smallest_class_slots(),
                  "teardown chunk must not span the whole table, or no board ever "
                  "exercises the preemption point");

    // Exit teardown: close every non-EMPTY handle before the TCB slot is reclaimable, or the
    // thread leaks its object references. A close that would drop refs to 0 with a waiter
    // still parked LEAKS (floors refs at 1) rather than stranding; that branch is
    // unreachable today, since every parked waiter pins its own cap.
    //
    // PREEMPTIBLE, and the ONE entry point here whose caller must NOT hold IrqLock: it takes
    // and releases its own, KCAP_TEARDOWN_CHUNK slots at a time. `c->dying` must already be
    // set, and that is what keeps the sweep safe across the gaps: the slot is not reclaimable
    // (only an EXITED one is), no peer can mint a cap into the table, and c stays on the
    // ready structure, so a preempted sweep resumes and stays TOTAL.
    //
    // An RR slice expiring in sched::tick_rr is the only thing that switches a dying thread
    // out at a chunk boundary, and so the only way two threads are ever in here at once
    // (g_teardown_depth, cap.cc).
    void cap_teardown(Thread* c);

    // True while any thread is inside cap_teardown. A preempted sweep may still hold an IRQ
    // cap on the published line, so the console reclaim defers re-initialising the UART
    // until the last sweep finishes. See console_tx.h.
    bool cap_teardown_active();

    // Seat (or re-seat) thread t's reserved stdout slot (index 0) as a SEND-ONLY
    // (CAP_SIGNAL) copy of console endpoint `target`. Written DIRECTLY (cap_install_at
    // rejects index 0): this and cap_install_defaults are the sole writers of the
    // reserved slot, and the slot-0 cap-gen is never bumped (kernel-only policy). Takes
    // the new ref BEFORE dropping any prior one (cap_console_publish order) so re-seating
    // the same endpoint never transiently frees it. Caller holds IrqLock.
    // False = the endpoint's refcount is at its uint8_t ceiling: NOTHING is seated and
    // any prior seat is left exactly as it was.
    //
    // PRECONDITION, unchecked: `t` must hold a cap run (`cap_capacity > 0`). It writes
    // `t->handles[0]` with NO bound test, so a capacity-0 thread is a null deref. Today
    // every caller satisfies it structurally: a spawn that gets no run fails before
    // cap_install_defaults runs, and the only capacity-0 thread is idle, which neither
    // publishes nor is spawned. Anything that can create a thread WITHOUT a run must add
    // the guard back.
    bool cap_seat_stdout(Thread* t, int target);

    // The privileged default cap set for a freshly spawned child. Pre-publish it installs
    // NOTHING (index 0 empty; write() falls back to kconsole_write). Post-publish it seats
    // a send-only (CAP_SIGNAL) copy of the console endpoint at index 0. See D4.
    void cap_install_defaults(Thread* child);

    // Move the kernel's stdout-target ref to `obj_handle` AND seat `publisher`'s own
    // slot 0 on it (the console handover publish path, D3/S3). Caller holds IrqLock.
    // Both refs are taken before either seat moves, and the new ones before the old are
    // dropped, so a re-publish of the same endpoint never transiently frees it and a
    // ceiling refusal leaves the whole prior arrangement intact. The kernel's own ref
    // carries rights 0 (identity, no WAIT), the publisher's CAP_SIGNAL.
    // False = the endpoint's refcount is at its uint8_t ceiling; nothing changed.
    bool cap_console_publish(Thread* publisher, int obj_handle);

    // Bump one reference to the object named by a global handle (delegation and create).
    // The handle MUST resolve: the caller validated it. Caller holds IrqLock. Unknown type
    // traps in debug. `rights` is the cap's rights bits: the endpoint arm bumps recv_holders
    // when they carry CAP_WAIT; the sem/mutex arms ignore it. CAP_REPLY is a no-op.
    //
    // False = REFUSED: a counter this bump would move is at its uint8_t ceiling, so NEITHER
    // counter moved (endpoint_refs and recv_holders advance together or not at all). True
    // also covers "no counter to move" (poolless type, or a handle that no longer resolves).
    [[nodiscard]] bool obj_ref_inc(CapType type, int obj_handle, uint8_t rights);

    // The exact inverse of a SUCCESSFUL obj_ref_inc with the same arguments. For unwinding a
    // partially-taken batch (the spawn delegation loop), never as a general release: it runs
    // no close protocol and frees nothing. Caller holds IrqLock.
    void obj_ref_undo(CapType type, int obj_handle, uint8_t rights);

    // True if c's table has a free dynamic slot (one cap_install could take). Same
    // scan span as cap_install; the probe-before-mint predicate for the reply cap
    // (B3: never pop a receiver a reply cap cannot be minted into). Caller holds IrqLock.
    bool cap_has_free_slot(Thread* c);

    // The single authority chokepoint: may thread `c` ask the kernel to do `need` (one or
    // more AUTH_* bits)? True if it is privileged, or if its authority word carries EVERY
    // requested bit. nullptr is false (no caller context).
    // Gates are enumerable: grep cap_check_authority.
    //
    // Locking: the ONE cap.h entry point that does NOT require IrqLock. It reads `c`'s
    // own TCB word, which is written only by that thread or by its parent BEFORE it first
    // runs, so there is no concurrent writer on a single core. An SMP kernel must
    // revisit this alongside the rest of the IrqLock-as-mutual-exclusion contract.
    bool cap_check_authority(Thread* c, uint8_t need);

    // Seat (or re-seat) thread t's authority word. Non-delegable: it is TCB state, not a
    // capability, so there is no entry a cap_grant could copy. The kernel is its only
    // writer. auth == 0 clears it. Caller holds IrqLock.
    void cap_seat_authority(Thread* t, uint8_t auth);

    // Narrow thread c's authority word in place: auth &= mask, never widening (the same
    // rule a cap_grant mask and kos_thread_params::authority obey). `cap_handle` must be
    // KOS_CAP_AUTHORITY. Returns 0, -KOS_EBADF (c holds no authority), or -KOS_EINVAL
    // (the handle names something else). Caller holds IrqLock.
    int cap_narrow_authority(Thread* c, int cap_handle, uint8_t mask);

    // Resolve a CAP_REPLY entry's packed obj word to the parked caller thread, or
    // nullptr if it is stale. The full one-shot guard: index in range, thread-gen
    // match, state == BLOCKED, call_state == REPLY_WAIT, and the packed seq8 matches
    // the caller's live call_seq. Used by kos_reply, the reply-cap death arm, and the
    // effective-priority funnel. Caller holds IrqLock. Decodes with MASKED shifts (the
    // seq8 top bit makes obj negative: an arithmetic shift would corrupt the handle).
    Thread* cap_reply_caller(int32_t obj);
}

#endif
