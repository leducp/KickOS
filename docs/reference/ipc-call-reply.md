<!-- SPDX-License-Identifier: CECILL-C -->
# Synchronous call/reply IPC -- the reply capability

The exact contract for the L4-style call/reply fastpath layered on the endpoint
rendezvous (`../book/endpoints-synchronous-ipc-by-rendezvous.md` narrates the endpoint;
`../book/synchronous-call-and-reply.md` narrates the why of this layer). Code source of
truth: `kernel/syscall/syscall.cc` (`endpoint_call` / `endpoint_reply` / `endpoint_recv`),
`kernel/syscall/cap.cc` (the `CAP_REPLY` arm + `cap_reply_caller`), `kernel/sync/sync.cc`
(`thread_effective_prio`), `user/include/kickos/sys/abi.h` (numbers + `kos_recv_info`),
`user/include/kickos/sys.h` (the C stubs). If a page and the code disagree, the page is
the bug.

## The reply capability (`CAP_REPLY`)

A call parks the CALLER until a reply arrives. While parked it cannot run, so its TCB
(ipc descriptor, priority, generation) is stable kernel-visible state. The design
exploits this: **there is no reply-object pool.** A `CAP_REPLY` entry in the SERVER's
handle table names the parked caller by generational thread handle; the caller's own
parked TCB is the reply object.

- **`CapType::CAP_REPLY`** (`kernel/include/kickos/cap.h`) -- one arm beside `CAP_SEM` /
  `CAP_MUTEX` / `CAP_ENDPOINT` / `CAP_IRQ`.
- **`CapEntry.obj` packing** (fits the frozen 8-byte `CapEntry`): `obj` holds
  `ThreadPool::handle_for(caller)` WHOLE and UNSHIFTED, all 32 bits of it
  (`gen:16 | index:16`, since `ThreadPool::INDEX_BITS` is 16) -- read it back with
  `cap_reply_handle`. The caller's 8-bit `call_seq` low byte is NOT in `obj`: it is split
  across the two spare bitfields beside the type and the rights -- `seq_lo`
  (`KCAP_REPLY_SEQ_LO_BITS` = 5, beside `CapType`'s 3) and `seq_hi`
  (`KCAP_REPLY_SEQ_HI_BITS` = 3, beside `CapRights`' 3) -- seated by `cap_reply_seq_seat`
  and read by `cap_reply_seq`. Giving `obj` to the whole handle is what makes it impossible
  for any width of the thread index to truncate the thread generation.
- **Decode the handle with UNSIGNED shifts** (`cap_reply_caller`): a fully aged thread
  generation sets bit 31, so `obj` is routinely negative and an arithmetic shift would
  corrupt the generation. The high bits are compared in FULL, not truncated to the
  generation's storage width, so a handle carrying anything above the field fails to
  resolve rather than aliasing a live slot.
- **`CapEntry.rights = 0`** -- no `WAIT` / `SIGNAL` / `TRANSFER`. A reply cap is not
  delegable, not dupable, and usable only by `KOS_SYS_REPLY` and `KOS_SYS_HANDLE_CLOSE`.
- **No `ThreadPool` refcount.** The cap does NOT pin the caller's thread slot; staleness
  is generation-guarded (mirrors how thread handles already behave). `obj_ref_inc` /
  `obj_ref_drop` are no-ops for `CAP_REPLY`.
- **One-shot.** The cap is consumed exactly once (entry emptied + slot cap-gen bumped) on
  EVERY exit from the in-flight state (see the death matrix). A second `kos_reply` on the
  same handle fails resolve.

**Stale-resolve (`cap_reply_caller`, `cap.cc`).** Decoding the obj word to a live caller
requires, under one `IrqLock`: index in range, thread-slot gen match, `state == BLOCKED`,
`call_state == CALL_REPLY_WAIT`, and `call_seq & 0xFF == seq8`. ANY mismatch resolves to
`nullptr` (a stale caller). This is the late-reply ABA guard (see limits).

## TCB call state

`Thread` carries, valid only while parked in a call:

- `call_rx_cap` -- reply capacity (the in-place buffer size).
- `call_seq` (`uint16_t`) -- bumped per call BEFORE the reply cap is packed; its low byte
  rides the entry's `seq_lo` / `seq_hi` spare bits.
- `call_state` -- `CALL_NONE` / `CALL_SEND_WAIT` (still parked on the endpoint's
  `send_waiters`) / `CALL_REPLY_WAIT` (queue-less, bound to the reply cap).

`call_state` is single-writer-clean at every park/unpark: `endpoint_send` sets
`CALL_NONE` before parking a plain sender; `endpoint_call` sets `CALL_NONE` after
`wq_confirm_resume` on EVERY return path. Without this an EPIPE-drained call would leave
`CALL_SEND_WAIT` set and a later plain `kos_send` would be misread as a call.

## `KOS_SYS_CALL = 34`

    kos_call(int ep, void* buf, size_t send_len, size_t recv_cap) -> long

One buffer carries the request out and receives the reply back (**in-place**): the
request is fully copied at rendezvous before the caller parks, so overwriting `buf` with
the reply is safe. A client wanting split tx/rx copies locally.

- Requires `CAP_SIGNAL` on `ep` (a `CAP_ENDPOINT` cap) -- same right as `kos_send`.
- `send_len > KOS_EP_MSG_MAX` (256) -> `-KOS_EINVAL` (never clamped). `recv_cap` above the
  bound is clamped (harmless). Reply truncation into `recv_cap` follows datagram semantics
  (not an error).
- The caller MUST be a spawned pool thread (`threads.index_of(c) >= 0`); the reply cap
  names it by pool-slot handle. The root/init TCB spawns and parks, it does not call ->
  `-KOS_EPERM`.
- Returns reply bytes (`>= 0`), or a negative error:

| Return | Meaning |
|---|---|
| `>= 0` | reply byte count (post-truncation into `recv_cap`) |
| `-KOS_EINVAL` | `send_len` exceeds `KOS_EP_MSG_MAX` |
| `-KOS_EFAULT` | `buf` not readable (`send_len`) or not writable (`recv_cap`) by the caller |
| `-KOS_EBADF` | bad endpoint cap |
| `-KOS_EPERM` | missing `CAP_SIGNAL`, no caller context, or a non-pool caller |
| `-KOS_EPIPE` | dead endpoint (`recv_holders == 0`), or the server died mid-transaction |
| `-KOS_EMFILE` | the server's handle table is full (no free slot to mint the reply cap) |
| `-KOS_ENOSYS` | the receiver took an info-less recv and cannot host a call |

Both buffer bound-checks run up front, in caller context, once. Two paths:

- **Fastpath** (a receiver is already parked in recv): under one `IrqLock`, PROBE before
  popping -- reject an info-less receiver (`ipc.badge_out == 0` -> `ENOSYS`) or a full
  receiver table (`EMFILE`) with NO side effects, THEN pop, copy the request into the
  receiver's buffer, mint the reply cap into the receiver's table, deliver its
  `kos_recv_info`, repurpose the caller's `ipc` to the reply target, park the caller
  queue-less in `CALL_REPLY_WAIT`, donate (D1), and wake the server (switches to it now).
- **Slowpath** (no receiver parked): park on `send_waiters` in `CALL_SEND_WAIT`; the mint
  + transfer + donation happen later in server context inside `endpoint_recv`. Boost the
  conventional server now (D2) if this caller outranks it.

## `KOS_SYS_REPLY = 35`

    kos_reply(kos_cap_t reply_cap, void const* buf, size_t len) -> int

Completes a call: copy the reply into the parked caller's buffer and wake it. The cap is
consumed on EVERY exit (one-shot). `len > KOS_EP_MSG_MAX` is clamped (the caller's
`call_rx_cap` clamps it anyway).

| Return | Meaning |
|---|---|
| `0` | reply delivered, caller woken |
| `-KOS_EBADF` | the handle is not a live `CAP_REPLY` cap |
| `-KOS_EFAULT` | `buf` not readable by the server for `len` |
| `-KOS_ESRCH` | the caller is gone/aborted/reused (stale resolve); the cap is still consumed |

`ESRCH` is a cheap no-op today (a parked caller cannot fault or exit and there is no
`thread_kill`, so it is unreachable); it becomes reachable when timed-call / kill land,
and the full stale-resolve above is what makes it safe.

## `KOS_SYS_RECV = 28` -- widened out-pointer

The recv out-pointer is now a `struct kos_recv_info` (was a bare `uint32_t` badge):

    struct kos_recv_info { uint32_t badge; kos_cap_t reply_cap; };   // 8 bytes, 4-aligned

- A plain `kos_send` arrival delivers `reply_cap == KOS_CAP_NONE`.
- A `kos_call` arrival delivers a real one-shot `CAP_REPLY` handle in the receiver's
  table; the receiver must eventually `kos_reply` it or `kos_handle_close` it. Test it
  against `KOS_CAP_NONE`: a handle fills all 32 bits, so no sign test works.
- **Info-less recv** (`out == NULL`, i.e. `badge_out == 0`): the receiver is NOT minted a
  reply cap and REJECTS calls -- the caller's `kos_call` fails `-KOS_ENOSYS`. Plain sends
  behave exactly as before. `endpoint_recv` validates 8 writable bytes at a 4-aligned
  out-ptr (misalignment `-KOS_EINVAL`, unowned `-KOS_EFAULT`); alignment is load-bearing
  for the privileged store.

This is a deliberate DoS closure: a service that must not have its handle table filled by
untrusted callers (the console, which every task holds a `SIGNAL` cap on) uses a plain
info-less recv, so hostile `kos_call`s bounce with `ENOSYS` instead of burning cap slots
and pinning the server's priority.

## Priority donation

Without donation a high-priority client is served at the driver's static priority and any
medium thread preempts the transaction indefinitely (the classic inversion; the KickCAT
cyclic exchange is the named victim). The contract, all through `sched::set_prio` (the
sole effective-priority writer):

- **D1 -- donate on handoff.** At the fastpath handoff and the slowpath pop, the server's
  effective priority is raised to `max(server->prio, caller->prio)`. With `sched::wake`'s
  trailing `reschedule()` the CPU goes straight to the server.
- **D2 -- boost on enqueue.** A caller parking on `send_waiters` while the server is busy
  boosts `Endpoint::server` NOW (not at the next recv), so the driver does not serve a
  low-priority transaction at low priority while a high-priority caller queues.
- **D3 -- revert by recompute, through ONE funnel.** On reply (and on reply-cap
  close/teardown) the server's priority is recomputed by `thread_effective_prio`, never
  restored-to-base. `mutex_unlock`'s revert is rebased onto the SAME funnel -- two live
  recomputes would let a driver that unlocks a mutex mid-transaction deflate below its call
  donation.
- **D4 -- the return handoff.** `kos_reply` deflates (D3) then wakes the caller; the caller
  is `>=` the server's recomputed priority whenever it donated, so the switch back is
  immediate. Symmetric with D1.

**The single funnel (`thread_effective_prio(t)`).**

    effective(t) = max( t->base_prio,
                        highest waiter across t's held mutexes,               // PI mutex term
                        prio of the caller behind each live CAP_REPLY in t's table,
                        highest parked CALL_SEND_WAIT caller on each endpoint
                          where ep->server == t )

Neither term scans the handle table. The `CAP_REPLY` donors come from `Thread::reply_waiters`,
one entry per live reply capability `t` holds, and the endpoint-server term from the chain
through `Endpoint::next_served` -- both O(donors) rather than O(table), and
`kernel/include/kickos/thread.h` forbids a table walk here in as many words. A capacity-bounded
scan would in any case be wrong now that width is per task: `KICKOS_MAX_HANDLES` is ROOT's width,
and a scan of `t`'s table is bounded by `thread_cap_capacity(t)`. Nothing here scans an object
pool either: the
same cheap-scan philosophy as the mutex held-list walk. `Endpoint::server` is a raw
`Thread*` set at every recv and CLEARED in the endpoint close/teardown arm when the server
drops its `WAIT` cap (waker-cleared discipline, mirrors `blocked_on`).

## Lifecycle / death matrix

The cap is consumed exactly once per unpark:

| Event | Mechanism | Caller outcome |
|---|---|---|
| `kos_reply` success | consume, copy reply, wake | woken, reply bytes |
| `kos_reply` to a stale caller | consume anyway, `-KOS_ESRCH` to server | n/a (already gone) |
| second `kos_reply` / bad handle | resolve fails at lookup | unaffected |
| server `handle_close`s the reply cap | `CAP_REPLY` close arm: EPIPE-wake caller, consume | woken, `-KOS_EPIPE` |
| server dies mid-transaction (fault -> exit) | `cap_teardown` hits the same close arm | woken, `-KOS_EPIPE` |
| server dies while caller still in `CALL_SEND_WAIT` | `recv_holders` -> 0 drains `send_waiters` | woken, `-KOS_EPIPE` |
| endpoint destroyed while a reply is outstanding | nothing -- the cap names the CALLER, not the endpoint | server can still reply; woken normally |
| mint fails, fastpath | fail the call `-KOS_EMFILE` BEFORE any side effect | error return, no state change |
| mint fails, slowpath (pop at recv) | wake the popped caller `-KOS_EMFILE`, recv retries | woken, `-KOS_EMFILE` |
| info-less receiver hit at recv (slowpath) | wake the popped caller `-KOS_ENOSYS`, recv keeps scanning | woken, `-KOS_ENOSYS` |
| server closes / loses its `WAIT` cap while `ep->server == it` | close arm clears `ep->server` + recomputes | any lingering D2 donation dropped |

The `CAP_REPLY` close arm runs the SAME full stale-resolve as `kos_reply` before waking
(defense in depth, load-bearing once timed-call lands). A teardown wake runs with the
closer `EXITED`; `sched::wake` already defers the switch in that case, so it is safe.

## Documented limits

- **Single-level donation only.** A nested call (service A `kos_call`s service B while
  serving C) does NOT propagate C's boost through A to B. The `blocked_on` edge is not yet
  generalized to a thread-or-mutex tag. No current consumer nests calls (SPI/I2C services
  call no one).
- **D2 through an already-held mutex is not re-boosted.** If the server is parked on a
  mutex when a high caller enqueues, D2 writes `ep->server`'s field but the mutex owner is
  not re-boosted (the chain walk runs only at `mutex_lock`). No current consumer takes a
  mutex mid-transaction. A ~10-line fix (re-run `mutex_lock`'s pass-2 chain walk after the
  D2 raise) is banked for when one does.
- **No timed / abortable call.** A parked caller waits indefinitely. `call_seq` already
  closes the late-reply ABA (an 8-bit, 256-deep window per thread) so a timeout path bolts
  on without an ABI change.
- **Callers must be spawned pool threads** (the root/init TCB cannot `kos_call`:
  `-KOS_EPERM`).
- **No cross-call state hold.** A reply cap lives across exactly one transaction; there is
  no bus-claim/session that spans multiple calls (a coherent multi-phase transaction is
  expressed as one call with multiple segments -- see `bus-service.md`).
- **Call-cycle (A<->B) and self-call** (a caller holding the only `WAIT` cap) park forever
  -- there is no cycle detection and no timeout.

## Cross-references

- The endpoint rendezvous this layers on: `../book/endpoints-synchronous-ipc-by-rendezvous.md`,
  `architecture.md` ("Object model, capabilities & IPC").
- The wait/wake substrate + the `wq_confirm_resume` barrier a parked call depends on:
  `../book/the-blocking-substrate-one-wait-wake-primitive.md`.
- The PI-mutex donation vocabulary the funnel unifies:
  `../book/priority-inheritance-lending-urgency.md`.
- The first consumer -- the SPI/I2C bus service: `bus-service.md`.
