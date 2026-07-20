// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Per-task capability table: a typed, rights-bearing, refcounted handle NAMING
// a global generational object (semaphore, PI mutex, or IPC endpoint). The
// cap table is a pure per-task naming+rights layer that WRAPS the unchanged global
// object pools (slotpool.h): a CapEntry stores (global-object-handle, type, rights)
// and cap_resolve is two-level -- the per-task cap-gen guard here, then the object
// pool's own object-gen guard. Object liveness is a global property (the pool +
// its parallel refs[]); capability possession is a per-task property (this table).
//
// Locking: none internal. Every entry point's precondition is CALLER HOLDS IrqLock,
// and a resolved object pointer is used under the SAME continuous lock (mirrors the
// KOS_SYS_sem_wait resolve-and-use invariant in syscall.cc). That, with the
// pin-by-own-cap refcount invariant, is why one holder's handle_close cannot free
// an object another holder just resolved.

#ifndef KICKOS_CAP_H
#define KICKOS_CAP_H

#include <stdint.h>

#include <kickos/config/system.h> // KICKOS_MAX_HANDLES (the codec must address it)

namespace kickos
{
    struct Thread; // kickos/thread.h -- embeds CapEntry handles[]; cap fns take Thread*

    // Handle-word index field width: 4 bits => <= 16 table slots (KICKOS_MAX_HANDLES
    // is 6/8 at these sizes); the rest of the word is the cap generation.
    static constexpr int KCAP_INDEX_BITS = 4;

    // A per-board KICKOS_MAX_HANDLES override that outgrows the index field would make
    // cap_install seat a slot the handle codec cannot address (index wraps mod 2^bits,
    // aliasing a live cap). Guard it, exactly as slotpool.h guards its own codec.
    static_assert(KICKOS_MAX_HANDLES <= (1 << KCAP_INDEX_BITS),
                  "cap handle index field cannot address the whole table");

    enum class CapType : uint8_t
    {
        CAP_EMPTY = 0, // an unused slot -- must be 0 so a zeroed TCB is an empty table
        CAP_SEM,
        CAP_MUTEX,   // PI mutex object pool
        CAP_ENDPOINT // synchronous IPC endpoint object pool
    };

    // Rights bits enforced at cap_resolve ((rights & need) == need); CAP_TRANSFER is
    // enforced at the delegate site. Every bit maps to a real check -- no dead field.
    enum CapRights : uint8_t
    {
        CAP_WAIT = 1 << 0,    // sem_wait / sem_trywait; endpoint recv
        CAP_SIGNAL = 1 << 1,  // sem_post; endpoint send
        CAP_TRANSFER = 1 << 2 // may be delegated into a child table (section 6)
    };

    // 8 bytes, 4-aligned; carries the object pool's handle codec verbatim (no
    // re-encoding). gen is the per-slot cap generation, bumped on close (the
    // per-task use-after-close ABA guard, at parity with the object pool's u16 gen).
    struct CapEntry
    {
        int32_t obj;    // global generational object handle (WRAP target); ignored if EMPTY
        uint8_t type;   // CapType
        uint8_t rights; // CapRights bits
        uint16_t gen;   // per-slot cap generation
    };
    static_assert(sizeof(CapEntry) == 8, "CapEntry must stay 8 bytes (frozen ABI, section 5)");

    // The one resolve chokepoint: validate a per-task cap handle and return the named
    // global object, or nullptr (bad index, empty, stale cap-gen, wrong type, or
    // missing rights). Returns void* (dispatch-on-type over the object pools); the
    // caller casts to the type it asked for. CAP_SEM/CAP_MUTEX/CAP_ENDPOINT all resolve.
    void* cap_resolve(Thread* c, int cap_handle, CapType want, uint8_t need);

    // Validate a cap handle and return its table entry (type-agnostic; for delegation
    // and close). nullptr on bad index / empty / stale cap-gen.
    CapEntry* cap_lookup(Thread* c, int cap_handle);

    // Install a cap naming `obj_handle` into the first free slot of c's table. Returns
    // the cap handle (cap-gen << KCAP_INDEX_BITS | index), or -1 if the table is full.
    // Does NOT touch the object refcount -- the caller owns that (sem_create sets refs=1
    // at alloc). Index 0 is reserved only in that defaults/delegation avoid it; a thread's
    // own sem_create may land there.
    int cap_install(Thread* c, int obj_handle, CapType type, uint8_t rights);

    // Install a cap at a SPECIFIC (assumed-free) index -- delegation's deterministic
    // placement (B1: delegated cap i -> child index i+1). Does NOT touch the refcount.
    void cap_install_at(Thread* c, int index, int obj_handle, CapType type, uint8_t rights);

    // Type-agnostic close: bump the slot's cap-gen (stale the handle), empty the entry,
    // then drop one reference to the named object (freeing it at refs -> 0). Returns 0,
    // or -1 if the handle does not resolve. Succeeds while other holders remain open.
    int handle_close(Thread* c, int cap_handle);

    // Exit teardown: close every non-EMPTY handle before the TCB slot is reclaimable
    // (else the thread leaks its object references). Noreturn-path safe: a close that
    // would drop refs to 0 with a waiter still parked LEAKS (floors refs at 1), never
    // strands -- but that branch is unreachable today (every parked waiter pins its
    // own cap, so refs >= 1). Caller holds IrqLock.
    void cap_teardown(Thread* c);

    // The privileged default cap set for a freshly spawned child. Installs NOTHING
    // today (index 0 reserved by convention for a future console cap; write() is a
    // direct syscall until then). Present so the spawn path has the seam.
    void cap_install_defaults(Thread* child);

    // Bump one reference to the object named by a global handle (delegation + create).
    // Dispatches on cap type; the handle MUST resolve (caller validated it). Caller
    // holds IrqLock. Unknown type traps in debug. Additive: each new pool gains one arm.
    // `rights` is the cap's rights bits: the endpoint arm bumps recv_holders when they
    // carry CAP_WAIT; the sem/mutex arms ignore it.
    void obj_ref_inc(CapType type, int obj_handle, uint8_t rights);
}

#endif
