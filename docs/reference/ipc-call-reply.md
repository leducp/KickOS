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
- The caller is named by its thread-pool slot handle in the reply cap, so any thread the
  pool seats may call, root included. Idle is the one TCB outside the pool and it issues no
  syscall at all.
- Returns reply bytes (`>= 0`), or a negative error:

| Return | Meaning |
|---|---|
| `>= 0` | reply byte count (post-truncation into `recv_cap`) |
| `-KOS_EINVAL` | `send_len` exceeds `KOS_EP_MSG_MAX` |
| `-KOS_EFAULT` | `buf` not readable (`send_len`) or not writable (`recv_cap`) by the caller |
| `-KOS_EBADF` | bad endpoint cap |
| `-KOS_EPERM` | missing `CAP_SIGNAL`, or no caller context |
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

## `KOS_SYS_CALL_TIMED = 46`

    kos_call_timed(kos_cap_t ep, void* buf, size_t send_len, size_t recv_cap,
                   uint32_t timeout_us) -> int32_t

`kos_call` with a deadline of `timeout_us` RELATIVE microseconds (`KOS_TIMEOUT_NONE` = no
deadline, which is exactly `kos_call`). Same returns as `KOS_SYS_CALL`, plus:

| Return | Meaning |
|---|---|
| `-KOS_ETIMEDOUT` | the deadline passed; no reply was received and none can arrive later |

- **Its own syscall number, not a flag.** The trap frame carries four argument slots and
  `kos_call` spends all four, so a deadline needs one freed. The stub packs `send_len` and
  `recv_cap` into a single slot (`kos_call_lens_pack`, nine bits each, both bounded by
  `KOS_EP_MSG_MAX`) and the dispatch arm unpacks. `kos_call` keeps its own number with the
  lengths UNPACKED, so the untimed path pays no packing.
- The pack SATURATES each field at 511 rather than masking it. The kernel remains the sole
  validator, and a masked `512` would arrive as `0` and become a silent zero-length call; a
  saturated `511` is still above `KOS_EP_MSG_MAX`, so the `-KOS_EINVAL` still fires.
- **ONE deadline spans BOTH phases**, the wait on `send_waiters` and the wait for the reply.
  It is armed once, at the call, and survives the handoff between the two: that transition
  moves the caller through `link` while the timer delta list uses `tnext`, and the cancel
  lives in `sched::wake`, which a park-to-park migration never reaches.
- **A timeout is not an abort.** If a server already took the request, it keeps its reply
  cap; its later `kos_reply` gets `-KOS_ESRCH` and consumes the cap. Reclaiming that entry
  would reach across a containment boundary, so it is left alone; the residue is bounded by
  `KICKOS_CAP_REPLY_MAX` against `Thread::cap_reply_live`.

## `KOS_SYS_RECV_TIMED = 47`

    kos_recv_timed(kos_cap_t ep, void* buf, size_t cap_len,
                   struct kos_recv_timed_opts* opts)
        -> int32_t

`kos_recv` with a deadline. It travels in a struct because `kos_recv` also spends all four
argument slots and a 9-bit `cap_len` has no packing partner. Same returns as
`KOS_SYS_RECV`, plus `-KOS_ETIMEDOUT`, and `-KOS_EINVAL` when `opts == NULL` (there is
nowhere else to state a deadline). `opts` is IN-OUT, so it is checked readable as well as
writable; plain `kos_recv` keeps its writable-only check.

**`kos_recv_info` did NOT grow a timeout field; a separate type appeared that NESTS it**,
and that separation is the load-bearing part:

    struct kos_recv_timed_opts {
        uint32_t timeout_us;        // IN
        struct kos_recv_info info;  // OUT, written exactly as a plain recv writes it
    };                              // 12 bytes, info at offset 4

A third member on `kos_recv_info` would have put an *input* field inside the struct every
plain recv loop declares uninitialised (`struct kos_recv_info info;` in `uart_service.h`
and `usb_cdc_service.h`), so a stack-garbage deadline would have been one line away, and
the rule against it would have been a comment anyone could violate. Nesting makes it
unrepresentable: a `kos_recv` caller has no timeout field to reach. It also keeps the
kernel's write-back a WHOLE-struct copy of `kos_recv_info` -- the dispatch passes
`opts + offsetof(struct kos_recv_timed_opts, info)` as the ordinary out-pointer, so
`write_recv_info` is unchanged, has no uninitialised tail to leak into user memory, and has
no input word to preserve. `opts->timeout_us` therefore survives every call, and a recv
loop may reuse one struct.

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

`ESRCH` is REACHABLE, by exactly one route: `kos_call_timed`. A caller whose deadline
expires is unwound out of `CALL_REPLY_WAIT` and left `CALL_NONE`, while the server keeps
the reply cap it was minted; the server's eventual `kos_reply` then resolves the cap, finds
no caller in `CALL_REPLY_WAIT`, consumes the cap and answers `-KOS_ESRCH`. The full
stale-resolve in `cap_reply_caller` is what makes that safe: it rejects on four independent
grounds (index out of pool range, thread-slot generation mismatch, the `CALL_REPLY_WAIT`
test, and a rolled `call_seq`) before anything is consumed.

## `KOS_SYS_RECV = 28` -- widened out-pointer

The recv out-pointer is now a `struct kos_recv_info` (was a bare `uint32_t` badge):

    struct kos_recv_info { uint32_t badge; kos_cap_t reply_cap; };   // 8 bytes, 4-aligned

It is PURELY an out-struct, and stays that way: the timed recv carries its deadline in its
own `kos_recv_timed_opts`, which nests this one (see `KOS_SYS_RECV_TIMED` above).

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

A donation that is never reverted changes no return code, so the revert is invisible to
any arm that only reads results: dropping the `sched::set_prio` in `endpoint_wait_timeout`
leaves the whole timed-call family green. What holds it is a scheduling ORDER --
`call_timeout_revert` in `user/apps/common/selftest/main.cc` runs a medium-priority
spoiler against a boosted server and requires it to run between the expiry and the
server's next log entry. It is the only arm that fails when that line goes.

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
drops its `WAIT` cap (waker-cleared discipline, like every wait edge).

## Lifecycle / death matrix

The cap is consumed exactly once per unpark:

| Event | Mechanism | Caller outcome |
|---|---|---|
| `kos_reply` success | consume, copy reply, wake | woken, reply bytes |
| `kos_reply` to a stale caller | consume anyway, `-KOS_ESRCH` to server | n/a (already timed out) |
| `kos_reply` on an abandoned cap whose caller is mid-call elsewhere | consume; the reply-waiter unlink misses, so complete as if gone | untouched: its live call is another server's |
| `kos_call_timed` deadline expires in `CALL_SEND_WAIT` | timer unwinds `send_waiters`, reverts D2 | woken, `-KOS_ETIMEDOUT`, never taken |
| `kos_call_timed` deadline expires in `CALL_REPLY_WAIT` | timer unwinds the donor list, reverts D3 | woken, `-KOS_ETIMEDOUT`; server keeps the cap |
| second `kos_reply` / bad handle | resolve fails at lookup | unaffected |
| server `handle_close`s the reply cap | `CAP_REPLY` close arm: EPIPE-wake caller, consume | woken, `-KOS_EPIPE` |
| server dies mid-transaction (fault -> exit) | `cap_teardown` hits the same close arm | woken, `-KOS_EPIPE` |
| server dies while caller still in `CALL_SEND_WAIT` | `recv_holders` -> 0 drains `send_waiters` | woken, `-KOS_EPIPE` |
| endpoint destroyed while a reply is outstanding | nothing -- the cap names the CALLER, not the endpoint | server can still reply; woken normally |
| mint fails, fastpath | fail the call `-KOS_EMFILE` BEFORE any side effect | error return, no state change |
| mint fails, slowpath (pop at recv) | wake the popped caller `-KOS_EMFILE`, recv retries | woken, `-KOS_EMFILE` |
| info-less receiver hit at recv (slowpath) | wake the popped caller `-KOS_ENOSYS`, recv keeps scanning | woken, `-KOS_ENOSYS` |
| server closes / loses its `WAIT` cap while `ep->server == it` | close arm clears `ep->server` + recomputes | any lingering D2 donation dropped |

The `CAP_REPLY` close arm runs the SAME full stale-resolve as `kos_reply` before waking,
which is load-bearing now that a timed call can leave a stale cap behind. A teardown wake runs with
the closer `dying` and still `RUNNING`: `EXITED` is set only after `cap_teardown` returns.
`sched::wake` defers the switch for a woken caller that does not outrank the closer and ADMITS one
that does, so what makes this safe is that the sweep is RESUMABLE, not that it is uninterrupted. The
caller is off `reply_waiters` with its wait edge cleared before the wake, so a peer that runs there
walks a shorter chain and never a torn one.

## Documented limits

- **Single-level donation only.** A nested call (service A `kos_call`s service B while
  serving C) does NOT propagate C's boost through A to B. The chain walk follows the
  `WAIT_MUTEX` edge only; a `WAIT_EP_REPLY` park names its server but no walk consumes that
  edge yet. No current consumer nests calls (SPI/I2C services call no one).
- **D2 through an already-held mutex is not re-boosted.** If the server is parked on a
  mutex when a high caller enqueues, D2 writes `ep->server`'s field but the mutex owner is
  not re-boosted (the chain walk runs only at `mutex_lock`). No current consumer takes a
  mutex mid-transaction. A ~10-line fix (re-run `mutex_lock`'s pass-2 chain walk after the
  D2 raise) is banked for when one does.
- **An UNTIMED call still parks indefinitely.** `kos_call` has no deadline; `kos_call_timed`
  is the bounded form, and its deadline covers both phases (the wait on `send_waiters` and
  the wait for the reply). A timeout does not abort the transaction: a request a server has
  already taken stays taken, and that server still holds the reply cap.
- **A caller may be root, and an UNTIMED root park is terminal for the whole system.** A pool
  caller holding a valid `CAP_SIGNAL` with no receiver ever parking is parked forever, and
  root is a pool thread like any other. Nothing else ends that park: root leaves
  `spawner_tag` at `KILL_TAG_NONE` so `kos_thread_kill` refuses it `-KOS_EPERM`
  (`kernel/syscall/syscall_thread.cc`), a cancel would only wake an `irq_wait` park in any
  case, and `kernel().live` never reaches 0 while root is parked, so shutdown never fires.
  Where a worker's park costs one thread, root's costs the image. `kos_call_timed` is the
  answer, and root is exactly where it is worth paying for.
- **Root holding a mutex the service takes is an undetected deadlock.** Root locks a mutex,
  calls a service, and the service locks the same mutex: `mutex_lock`'s cycle-detection pass
  walks the `WAIT_MUTEX` wait edge (`Thread::wait_mutex()`), which only `mutex_lock` ever
  seats, and a call park tags `WAIT_EP_*` instead, so the accessor answers `nullptr` there.
  The walk therefore ends at root, reports no `-KOS_EDEADLK`, and both threads wedge. LATENT:
  no in-tree service takes a mutex mid-transaction (same premise as the D2 limit above). A
  timed call would bound that wedge, not detect it: the cycle stays invisible, the caller
  learns only that time ran out, and the service stays blocked until the caller releases the
  mutex of its own accord.
- **No cross-call state hold.** A reply cap lives across exactly one transaction; there is
  no bus-claim/session that spans multiple calls (a coherent multi-phase transaction is
  expressed as one call with multiple segments -- see `bus-service.md`).
- **Call-cycle (A<->B) and self-call** (a caller holding the only `WAIT` cap) park forever
  under `kos_call`: there is no cycle detection. `kos_call_timed` bounds the park without
  detecting the cycle.

## Cross-references

- The endpoint rendezvous this layers on: `../book/endpoints-synchronous-ipc-by-rendezvous.md`,
  `architecture.md` ("Object model, capabilities & IPC").
- The wait/wake substrate + the `wq_confirm_resume` barrier a parked call depends on:
  `../book/the-blocking-substrate-one-wait-wake-primitive.md`.
- The PI-mutex donation vocabulary the funnel unifies:
  `../book/priority-inheritance-lending-urgency.md`.
- The first consumer -- the SPI/I2C bus service: `bus-service.md`.
