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
// KOS_SYS_SEM_WAIT resolve-and-use invariant in syscall.cc). That, with the
// pin-by-own-cap refcount invariant, is why one holder's handle_close cannot free
// an object another holder just resolved.

#ifndef KICKOS_CAP_H
#define KICKOS_CAP_H

#include <stdint.h>

#include <kickos/config/system.h> // KICKOS_MAX_HANDLES (the codec must address it)
#include <kickos/sys/cap_index.h> // KICKOS_CAP_FIRST_DYNAMIC (frozen reserved range)

// The frozen reserved range must leave at least one dynamic slot, or no own-create could
// ever succeed. Every board's KICKOS_MAX_HANDLES (floor 9: FIRST_DYNAMIC + main's 2
// permanent caps + a 3-own-cap test peak) is budgeted against this.
static_assert(KICKOS_MAX_HANDLES > KICKOS_CAP_FIRST_DYNAMIC,
              "no dynamic cap slots left: raise KICKOS_MAX_HANDLES or shrink the reserved range");

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
        CAP_MUTEX,    // PI mutex object pool
        CAP_ENDPOINT, // synchronous IPC endpoint object pool
        CAP_REPLY,    // one-shot L4-style reply cap; obj NAMES the parked caller by
                      // generational thread handle (no object pool, no refcount)
        CAP_AUTHORITY // the right to ask the kernel to act on the machine; its entire
                      // content is the AUTH_* rights bits below. POOLLESS (`obj`
                      // unused, no refcount): resolves via cap_lookup, NEVER via
                      // cap_resolve_e (which would chase `obj` in a pool)
    };

    // Rights bits enforced at cap_resolve ((rights & need) == need); CAP_TRANSFER is
    // enforced at the delegate site. Every bit maps to a real check -- no dead field.
    //
    // ONE byte serves two families, disambiguated by the entry's TYPE: the low three
    // are OBJECT rights, the high five AUTHORITY rights. They must not overlap:
    // CAP_TRANSFER is read type-agnostically at the delegation site.
    //
    // Five authority bits fit here, which is a property of this byte and not a budget for
    // the type: a CAP_AUTHORITY entry leaves `obj` unused, so a wider authority word moves
    // there and CapEntry stays 8 bytes. CAP_TRANSFER living in this byte is also the only
    // thing stopping an authority cap being copied wholesale into a child table, so such a
    // move needs an explicit type test at the delegation site first.
    // Mirrored in <kickos/sys/abi.h> as KOS_AUTH_*.
    enum CapRights : uint8_t
    {
        CAP_WAIT = 1 << 0,    // sem_wait / sem_trywait; endpoint recv
        CAP_SIGNAL = 1 << 1,  // sem_post; endpoint send
        CAP_TRANSFER = 1 << 2, // may be delegated into a child table (section 6)

        AUTH_MEMORY = 1 << 3, // ram_alloc + the spawn-time MMIO window grant
        AUTH_PINMUX = 1 << 4, // pinmux_set
        AUTH_CLOCK = 1 << 5,  // cpu_clock_set
        AUTH_IRQ = 1 << 6,    // irq_attach, irq_unmask
        AUTH_DEVICE = 1 << 7  // console_publish, shutdown, reboot
    };

    // arch_periph_enable carries NO authority bit: it is gated on the caller holding a live
    // ARCH_MPU_DEV region whose base matches the block exactly (caller_holds_mmio_block,
    // syscall_mem.cc). Its callers are the bus drivers, so AUTH_DEVICE would have handed
    // shutdown and reboot to every unprivileged driver in the fleet.

    // Every AUTH_* bit: what a privileged thread is implicitly allowed.
    static constexpr uint8_t CAP_AUTH_ALL = static_cast<uint8_t>(
        AUTH_MEMORY | AUTH_PINMUX | AUTH_CLOCK | AUTH_IRQ | AUTH_DEVICE);

    // 8 bytes, 4-aligned; carries the object pool's handle codec verbatim (no
    // re-encoding). gen is the per-slot cap generation, bumped on close (the
    // per-task use-after-close ABA guard, at parity with the object pool's u16 gen).
    // INVARIANT (address-space-agnostic; MMU-era load-bearing): a cap names its
    // object ONLY by generational handle -- never by a physical address or a region
    // base. No physaddr is ever stored here or delivered as a badge/payload, so the
    // cap layer carries no single-physical-space assumption for the MMU era to undo.
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
    // Does NOT touch the object refcount -- the caller owns that (sem_create sets refs=1
    // at alloc). Indices 0 .. KICKOS_CAP_FIRST_DYNAMIC-1 are the frozen reserved range
    // (0 = kernel stdout, B3): the scan starts at KICKOS_CAP_FIRST_DYNAMIC (4), so an own
    // create never lands in the reserved range (own caps live in [FIRST_DYNAMIC .. MAX-1]).
    int cap_install(Thread* c, int obj_handle, CapType type, uint8_t rights);

    // Install a cap at a SPECIFIC (assumed-free) index -- delegation's deterministic
    // placement (B1: delegated cap i -> child index i+1). Does NOT touch the refcount.
    void cap_install_at(Thread* c, int index, int obj_handle, CapType type, uint8_t rights);

    // Type-agnostic close: bump the slot's cap-gen (stale the handle), empty the entry,
    // then drop one reference to the named object (freeing it at refs -> 0). Returns 0,
    // or -KOS_EBADF if the handle does not resolve. Succeeds while other holders remain open.
    int handle_close(Thread* c, int cap_handle);

    // Exit teardown: close every non-EMPTY handle before the TCB slot is reclaimable
    // (else the thread leaks its object references). Noreturn-path safe: a close that
    // would drop refs to 0 with a waiter still parked LEAKS (floors refs at 1), never
    // strands -- but that branch is unreachable today (every parked waiter pins its
    // own cap, so refs >= 1). Caller holds IrqLock.
    void cap_teardown(Thread* c);

    // Seat (or re-seat) thread t's reserved stdout slot (index 0) as a SEND-ONLY
    // (CAP_SIGNAL) copy of console endpoint `target`. Written DIRECTLY (cap_install_at
    // rejects index 0): this and cap_install_defaults are the sole writers of the
    // reserved slot, and the slot-0 cap-gen is never bumped (kernel-only policy). Takes
    // the new ref BEFORE dropping any prior one (cap_console_publish order) so re-seating
    // the same endpoint never transiently frees it. Caller holds IrqLock.
    void cap_seat_stdout(Thread* t, int target);

    // The privileged default cap set for a freshly spawned child. Pre-publish it installs
    // NOTHING (index 0 empty; write() falls back to kconsole_write). Post-publish it seats
    // a send-only (CAP_SIGNAL) copy of the console endpoint at index 0. See D4.
    void cap_install_defaults(Thread* child);

    // Move the kernel's stdout-target ref to `obj_handle` (the console handover publish
    // path, D3/S3). Caller holds IrqLock. Takes the new ref before dropping the old, so a
    // re-publish of the same endpoint never transiently frees it; ref carries rights 0.
    void cap_console_publish(int obj_handle);

    // Bump one reference to the object named by a global handle (delegation + create).
    // Dispatches on cap type; the handle MUST resolve (caller validated it). Caller
    // holds IrqLock. Unknown type traps in debug. Additive: each new pool gains one arm.
    // `rights` is the cap's rights bits: the endpoint arm bumps recv_holders when they
    // carry CAP_WAIT; the sem/mutex arms ignore it. CAP_REPLY is a no-op (no pool ref).
    void obj_ref_inc(CapType type, int obj_handle, uint8_t rights);

    // True if c's table has a free dynamic slot (one cap_install could take). Same
    // scan span as cap_install; the probe-before-mint predicate for the reply cap
    // (B3: never pop a receiver a reply cap cannot be minted into). Caller holds IrqLock.
    bool cap_has_free_slot(Thread* c);

    // The single authority chokepoint: may thread `c` ask the kernel to do `need` (one or
    // more AUTH_* bits)? True if it is privileged, or if it holds a CAP_AUTHORITY at
    // KOS_CAP_AUTHORITY carrying EVERY requested bit. nullptr is false (no caller context).
    // Gates are enumerable: grep cap_check_authority.
    //
    // Locking: the ONE cap.h entry point that does NOT require IrqLock. It reads `c`'s
    // own table, which is written only by that thread or by its parent BEFORE it first
    // runs, so there is no concurrent writer on a single core. An SMP kernel must
    // revisit this alongside the rest of the IrqLock-as-mutual-exclusion contract.
    bool cap_check_authority(Thread* c, uint8_t need);

    // Seat (or re-seat) thread t's authority cap at KOS_CAP_AUTHORITY with `rights`
    // (AUTH_* bits). Seated WITHOUT CAP_TRANSFER, so it can never be delegated into a
    // child table; index 2 has exactly one writer, the kernel. rights == 0 clears the
    // slot. Holds no pool reference. Caller holds IrqLock.
    void cap_seat_authority(Thread* t, uint8_t rights);

    // Resolve a CAP_REPLY entry's packed obj word to the parked caller thread, or
    // nullptr if it is stale. The full one-shot guard: index in range, thread-gen
    // match, state == BLOCKED, call_state == REPLY_WAIT, and the packed seq8 matches
    // the caller's live call_seq. Used by kos_reply, the reply-cap death arm, and the
    // effective-priority funnel. Caller holds IrqLock. Decodes with MASKED shifts (the
    // seq8 top bit makes obj negative -- an arithmetic shift would corrupt the handle).
    Thread* cap_reply_caller(int32_t obj);
}

#endif
