<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# Synchronous call/reply IPC -- the reply capability

The exact contract for the L4-style call/reply fastpath layered on the endpoint
rendezvous (`../book/endpoints-synchronous-ipc-by-rendezvous.md` narrates the endpoint;
`../book/synchronous-call-and-reply.md` narrates the why of this layer). Code source of
truth: `kernel/syscall/syscall_ipc.cc` (`endpoint_call` / `endpoint_reply` /
`endpoint_reply_recv` / `endpoint_recv_locked`),
`kernel/syscall/cap.cc` (the `CAP_REPLY` arm + `cap_reply_thread` / `cap_reply_caller`),
`kernel/sync/sync.cc` (`thread_effective_prio`), `kernel/thread/park.cc`
(`endpoint_wait_abort`), `kernel/amp/ampwindow.cc` (the far reply's route),
`kernel/irq/irq.cc` (the notification word, its bind and its post),
`user/include/kickos/sys/abi.h` (numbers + `kos_recv_info` + `kos_reply_recv_opts`),
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

**Stale-resolve (`cap_reply_thread`, `cap.cc`).** Decoding the obj word to a live caller
requires, under one `IrqLock`: index in range, thread-slot gen match, `state == BLOCKED`,
`call_state == CALL_REPLY_WAIT`, and `((call_seq ^ seq) & seq_mask) == 0`. ANY mismatch resolves
to `nullptr` (a stale caller). This is the late-reply ABA guard (see limits).
**`seq_mask` is the width the CALLING ARM's own storage carries and not a policy knob**: a
`CAP_REPLY` entry holds `KCAP_REPLY_SEQ_BITS` of the sequence in its spare bitfields and asks for
those 8 bits, a far node's reply tag carries `call_seq` whole and asks for all 16
(`amp::REPLY_SEQ_MASK`). An arm comparing more bits than its storage carries would refuse every
live call, so the width travels with the arm and the body does not branch on it. A mask of zero
drops the clause, which is what an arm asserting that an EARLIER clause refused wants.
`cap_reply_caller` is that body over what a `CAP_REPLY` entry carries; the far-call section
below runs the SAME body over a peer node's reply token, which is why the clauses have one
home and not two.

## TCB call state

`Thread` carries, valid only while parked in a call:

- `call_rx_cap` -- reply capacity (the in-place buffer size).
- `call_seq` (`uint16_t`) -- bumped per call BEFORE the reply cap is packed; its low byte
  rides the entry's `seq_lo` / `seq_hi` spare bits.
- `call_state` -- `CALL_NONE` / `CALL_SEND_WAIT` (still parked on the endpoint's
  `send_waiters`) / `CALL_REPLY_WAIT` (queue-less, bound to the reply cap, or to a far
  node's reply token where the park is `WAIT_EP_FAR_REPLY`).

`call_state` is single-writer-clean at every park/unpark: `endpoint_send` sets
`CALL_NONE` before parking a plain sender; `endpoint_call` sets `CALL_NONE` after
`wq_confirm_resume` on EVERY return path. Without this an EPIPE-drained call would leave
`CALL_SEND_WAIT` set and a later plain `kos_send` would be misread as a call.

## `KOS_SYS_CALL = 34`

    kos_call(kos_cap_t ep, void* buf, size_t send_len, size_t recv_cap) -> int32_t

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
| `-KOS_EFAULT` | `buf` not readable (`send_len`) or not writable (`recv_cap`) by the caller, or the rendezvous copy was refused (see **A copy the boundary check cannot promise** below) |
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
  + transfer + donation happen later in server context inside `endpoint_recv_locked`. Boost the
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

## `KOS_SYS_REPLY = 35`

    kos_reply(kos_cap_t reply_cap, void const* buf, size_t len) -> int

Completes a call: copy the reply into the parked caller's buffer and wake it. The cap is
consumed on EVERY exit (one-shot). `len > KOS_EP_MSG_MAX` is clamped (the caller's
`call_rx_cap` clamps it anyway).

| Return | Meaning |
|---|---|
| `0` | reply delivered, caller woken |
| `-KOS_EBADF` | the handle is not a live `CAP_REPLY` cap |
| `-KOS_EFAULT` | `buf` not readable by the server for `len`, or the reply copy was refused (see below). The cap is consumed either way |
| `-KOS_ESRCH` | the caller is gone/aborted/reused (stale resolve); the cap is still consumed |

`ESRCH` is REACHABLE, by exactly one route: `kos_call_timed`. A caller whose deadline
expires is unwound out of `CALL_REPLY_WAIT` and left `CALL_NONE`, while the server keeps
the reply cap it was minted; the server's eventual `kos_reply` then resolves the cap, finds
no caller in `CALL_REPLY_WAIT`, consumes the cap and answers `-KOS_ESRCH`. The full
stale-resolve in `cap_reply_caller` is what makes that safe: it rejects on four independent
grounds (index out of pool range, thread-slot generation mismatch, the `CALL_REPLY_WAIT`
test, and a rolled `call_seq`) before anything is consumed.

## `KOS_SYS_REPLY_RECV = 68`

    kos_reply_recv(kos_cap_t reply_cap, void* buf, uintptr_t lens,
                   struct kos_reply_recv_opts* opts)
        -> int32_t

A `kos_reply` and the receive that follows it, under ONE kernel entry. Nine server loops in
the tree open-code that adjacency; the fused form pays one trap, one `IrqLock`, one
`sched::current()` and one buffer validate where the pair pays two of each.

- `lens` is `kos_call_lens_pack(reply_len, recv_cap)`, the nine-bits-each pack
  `KOS_SYS_CALL_TIMED` established. Both halves CLAMP to `KOS_EP_MSG_MAX` rather than being
  refused, which is what the two syscalls this one replaced did: an over-length reply was
  clamped by the parked caller's own capacity anyway, and an over-length receive capacity was
  harmless. The pack SATURATES at 511 rather than masking, so a wild argument arrives above
  the bound and clamps there instead of wrapping to a silent zero.
- `buf` is ONE buffer used IN PLACE: the reply goes out of it and the next request comes back
  into it. Safe for the reason `kos_call`'s in-place buffer is: the reply is fully copied into
  the caller before this body parks, so nothing in flight reads `buf` afterwards.
- `reply_cap == KOS_CAP_NONE` is the first pass of a loop, and the notify-only pass: nothing to
  answer, receive only.

**`struct kos_reply_recv_opts`**, 24 bytes with the nested info at offset 16:

    struct kos_reply_recv_opts {
        kos_cap_t ep;              // IN:  endpoint to receive on (CAP_WAIT)
        uint32_t  flags;           // IN:  KOS_RECV_NO_INFO, or 0
        uint32_t  timeout_us;      // IN:  relative us, or KOS_TIMEOUT_NONE
        uint32_t  notify;          // IN:  the notification bits this wait accepts
                                   // OUT: the bits it consumed
        struct kos_recv_info info; // OUT: the arrival, whole-struct
    };

It is IN-OUT, so it is checked readable AND writable, and it NESTS `kos_recv_info` rather than
widening it: a caller then has no input field to leave uninitialised, and the kernel's
write-back stays a whole-struct copy at
`opts + offsetof(..., info)`, so `write_recv_info` is unchanged. `notify` sits OUTSIDE that
nesting because only this syscall reads or writes it.

**`notify` is IN-OUT, and the input half is the WHOLE opt-in**: there is no flag beside it, and
a zero mask accepts nothing. The call takes an endpoint and no IRQ capability, so with an
OUT-only field nothing at the call site would say the wait can be ended by a line at all, the
link being the per-thread bind and nothing else. The input mask puts the link where the call is
written, and it lets a thread bound to several lines wait on one of them. **A bit the caller did
not accept is LEFT STANDING** for a later wait, never consumed and dropped.

**The endpoint has to be passed, and that is not redundant.** A `CAP_REPLY` names the CALLER
and not the endpoint (see the death matrix: an endpoint destroyed under an outstanding reply
changes nothing), so the receive half has no way to derive where to listen from the reply
capability. `RECV_RESOLVE` is not elided by the fusion.

**THE REPLY HALF'S FAILURES ARE NOT SYMMETRIC, and this is the one place the fused form is not
the concatenation of its two halves.**

| reply-half return | the receive half |
|---|---|
| `-KOS_EBADF` | does NOT run: the server's own argument is wrong, and receiving on top of it would hide the fault behind a park |
| `-KOS_EFAULT` | does NOT run, but for a DIFFERENT reason: the code has to reach the server, and a receive on top of it would overwrite the code with its own. The cap is consumed either way |
| `-KOS_ESRCH` | DOES run: the transaction the cap named is already over, which says nothing about this server's ability to receive and nothing a caller has to be told |

**A REPLY-HALF `-KOS_EFAULT` NAMES THE CLIENT AND NOT THE SERVER**, so it ends a transaction
and not a service. `buf` was proved readable and writable in the server's own context before
anything was copied, which leaves the caller's side of the reply copy as the only one that can
still be gone. A loop that leaves on any negative result therefore lets one client take a
shared service away from every other client, and that is what
`<kickos/sys/serve.h>`'s `serve_transaction_failed` exists to refuse: `-KOS_EFAULT` drops the
transaction and receives again, and every other code ends the loop. `-KOS_ETIMEDOUT` is
deliberately not on that list, a deadline being the server's own policy rather than anything a
client did.

Receive-half returns, the whole set a receive can answer:

| Return | Meaning |
|---|---|
| `>= 0` | received byte count |
| `-KOS_ENOTIFY` | no message; an accepted notification ended the wait and `opts->notify` carries its bits. NOT spelled EAGAIN: a code reading as "try again" invites a loop that never services the device |
| `-KOS_EINVAL` | `opts` null or misaligned, or an undefined `flags` bit. NOT a length: both halves of `lens` clamp |
| `-KOS_EFAULT` | `opts` or `buf` not accessible, or a copy was refused |
| `-KOS_EBADF` / `-KOS_EPERM` | bad endpoint cap, missing `CAP_WAIT`, or no caller context |
| `-KOS_ETIMEDOUT` | `opts->timeout_us` passed |
| `-KOS_ECANCELED` | the caller was cancelled before or during the park |

**`opts->notify` IS WRITTEN BACK ON EVERY EXIT THAT PROVED THE FIELD WRITABLE**, the error
exits included, and a reply refusal that ends the call before the receive half runs writes
zero. That is the whole of the in-out rule: IN is the mask the caller ACCEPTS and OUT is the
mask this call CONSUMED, so an exit that wrote nothing would report every accepted line as
consumed and the caller would service lines that never fired. Zero is an answer, not an
absence. Only the exits that refuse `opts` itself, the null or misaligned pointer and the
inaccessible struct, leave the field alone: they never proved it writable, and they are out of
reach of a caller passing the address of a real one, so the loop below may read the field on
any code it can actually receive.

The write is skipped where the caller ACCEPTED no line, and the input half is what makes that
sound: such a caller wrote 0 in, so leaving the field alone leaves 0, which is the true count
of what the wait consumed. The observable contract is the same either way, the field holding
the consumed bits after the call, and a caller must re-seat the accepted mask before each call
because the return overwrites it. THE SKIP DIES WITH THE INPUT HALF: make `notify` OUT-only
again and it starts leaving a stale value behind. Where a line WAS accepted, a refused write
answers `-KOS_EFAULT` even though a message may have arrived, the bits otherwise being lost
with nothing saying so.

**THE RULE IS STRUCTURAL AND NOT A CONVENTION EACH EXIT KEEPS.** Everything past the point
where the options snapshot succeeds is one callable, and the write-back is the statement after
its single call, so an exit added anywhere inside that region lands on the write-back rather
than stepping over it. A destructor would not serve: the write-back can be refused in its own
right and the contract owes the caller `-KOS_EFAULT` for that, which a destructor running after
the return value is already fixed cannot answer.

**CONSUMING A NOTIFICATION REARMS ITS LINE, and that is not bookkeeping.** The first-level ISR
masks the line, and only a `needs_rearm` flag set in thread context lets a later `kos_irq_ack`,
or the next accepting wait's own entry, lift that mask. So this call does both of the things a
`kos_irq_wait` does around a line: on ENTRY it rearms each accepted line a previous pass
consumed, which is what keeps `kos_irq_ack` OPTIONAL exactly as it is for a per-line wait, and
on consuming a bit it flags that line so a later ack can lift the mask early. Without the
second, a driver takes exactly ONE interrupt per line and then goes silent with no error on any
path.

**THE WAKE IS DEFERRED, and a green suite does not say whether it was.** `sched::wake` reaches
`resched_after_wake` and then `pick_and_seat`, and on arm64, rv64, x86_64, the LX6 and the sim
`arch_switch` swaps INLINE. A fused body that woke the answered caller eagerly would therefore
switch away BEFORE reaching its own park on those backends, the caller would find no receiver
parked, and its next call would take the slowpath. The reply half uses `wake_no_resched` and
hands the woken thread to the receive half's own deferred-wake bookkeeping, so the one
reschedule is the park's. **The park's reschedule has to be TOLD which thread**, because the
server goes BLOCKED and the switch that follows stores nothing and announces nothing: a caller
placeable only on a peer core is owed an ask that no later pass re-derives.
**The witness is a COUNT and not a time**: the bench's slowpath share is entirely the donating
arm, so `CALL_SLOW_TOTAL`'s n falls toward zero once a server loop adopts this and
`CALL_TOTAL`'s n rises by the same amount. On a backend where the eager wake
was left in, neither moves.

## The IRQ notification, and which waits admit one

An interrupt is delivered as ONE BIT PER LINE in the SERVING thread's own
`Thread::notify_pending`, not as a per-line notification object.

- **The bind is explicit and it is the SERVER's.** `kos_irq_attach(irq_cap, &mask)` says "this
  thread serves this line" and answers the single-bit mask the line will arrive in. It cannot
  be done at the claim: a driver's lines are claimed by the SPAWNER, which needs `KOS_AUTH_IRQ`,
  delegated into the threads it spawns, and then closed, so the claiming thread is neither the
  waiter nor a holder afterwards. One binding is also routinely delegated twice, `CAP_WAIT` to
  the thread that parks on the line and `CAP_SIGNAL` to the peer that rings it as a doorbell.
- **A second thread binding the same line is refused `-KOS_EBUSY`**, as a second claim of the
  line is: one server per line, as one owner per line.
- **A raise arriving before any bind LATCHES on the binding** and the bind delivers it. That is
  what keeps a `kos_irq_notify` doorbell rung before the server's first wait from being
  dropped, which is the startup race the doorbell exists to win.
- **A post onto a bit already set answers `-KOS_EALREADY`.** The post is absorbed, as it must
  be, one bit per line carrying no count; the code says the doorbell is working and the
  consumer has not drained it yet, never that the ring was lost. A poster that treats it as a
  failure and retries is reading it backwards.
- **A raise the server never consumed goes BACK to that latch when its bind ends**, which is
  the same startup race read from the other end: another capability can keep the binding alive
  across a server's death, and the ISR has already masked the line. Dropped instead of handed
  back, the event is nowhere and the line is masked with no wait return left to flag it for
  rearm, so the replacement server is silent for good.
- **`kos_irq_wait` refuses `-KOS_EPERM` from a thread that does not hold the bind**, there
  being no word for the line to arrive in. A pending cancel still answers `-KOS_ECANCELED`
  first: a killed thread is owed that code from every later wait.
- **Which waits ACCEPT a notification**: `KOS_SYS_IRQ_WAIT` and its timed form, for the one
  line they name, and `KOS_SYS_REPLY_RECV` for the lines its `notify` mask names. Nothing else.
  A bit set on a thread parked in a mutex, a plain `sem_wait` or a call's reply wait is NOT a
  wake: those parks have no result channel for it, and the bits are collected by the thread's
  next accepting wait.
- **A mask naming a line the caller does not serve is DROPPED, not honoured.** The bit is a
  binding's pool index, which is not a capability, so the bind is what authorises the wait to
  touch that line and the mask is narrowed to it.
- **A message and a notification are additive, not alternative.** `opts->notify` is non-zero
  iff at least one accepted bit fired and was consumed by this wait, and the return value says
  separately whether a message arrived. A notification-only return is `-KOS_ENOTIFY`, which
  keeps a zero-length arrival meaning a zero-length arrival.

The resulting server loop:

    while (true)
    {
        opts.notify = my_lines; // IN, and overwritten by the OUT on return
        long n = kos_reply_recv(reply_cap, buf, lens, &opts);
        // BEFORE the return code is classified. Every exit that could have consumed a bit
        // writes the bits it really took, zero included, so a negative code carries no stale
        // accepted mask. The two exits that leave the field alone refuse `opts` itself, which
        // the address of a real struct cannot provoke.
        if (opts.notify != 0)
        {
            service_lines(opts.notify);
        }
        if (n == -KOS_ENOTIFY)
        {
            reply_cap = KOS_CAP_NONE;
            continue;
        }
        if (n < 0)
        {
            if (serve_transaction_failed(n)) // <kickos/sys/serve.h>
            {
                reply_cap = KOS_CAP_NONE;
                continue;
            }
            break;
        }
        reply_cap = serve(buf, n, opts.info.reply_cap);
    }

**WITNESSED AT ONE KERNEL CORE ONLY, and that is a coverage statement and not a caveat about
the mechanism.** Both arms that exercise the notification, the one whose bits are already
standing and the one that wakes a server parked in the wait, rest on one thread's progress
against another's and so carry `TAP_SKIP_ONE_CORE_ORDER()`. A multicore image therefore ships
this path with no multicore run behind it. What IS witnessed above one core is that the word
costs nothing there: the binding, the bind and the post all compile and link on every preset,
and the rest of the IPC suite is green on four cores.

**The notification bound and the reply bound do not interact.** `cap_can_take_reply` gates on a
free dynamic slot and on `cap_reply_live(c) < KICKOS_CAP_REPLY_MAX`. A notification is not a
capability and consumes no slot, so a wait that returns one alone mints nothing and leaves the
table as it found it.

## The receive out-struct

**There is no plain receive syscall.** A receive is `KOS_SYS_REPLY_RECV` with nothing to reply
to. The split forms, plain and timed, are gone rather than kept beside it: one entry serves a
server loop, a first pass and a pure listener alike, and keeping the split pair would have left
them a call frame slower than the fused form for no reason but that they existed. The
out-struct they carried survives, nested in `kos_reply_recv_opts`:

    struct kos_recv_info { uint32_t badge; kos_cap_t reply_cap; };   // 8 bytes, 4-aligned

- A plain `kos_send` arrival delivers `reply_cap == KOS_CAP_NONE`.
- A `kos_call` arrival delivers a real one-shot `CAP_REPLY` handle in the receiver's
  table; the receiver must eventually `kos_reply` it or `kos_handle_close` it. Test it
  against `KOS_CAP_NONE`: a handle fills all 32 bits, so no sign test works.
- **Info-less receive** (`KOS_RECV_NO_INFO`): the receiver is NOT minted a reply cap and
  REJECTS calls, so the caller's `kos_call` fails `-KOS_ENOSYS`. Plain sends are unaffected.
  It is a FLAG and not a null out-pointer because an opts struct always has an address, so
  without one the info-less posture and every other thing that struct carries would be
  mutually exclusive.

This is a deliberate DoS closure: a service that must not have its handle table filled by
untrusted callers (the console, which every task holds a `SIGNAL` cap on) receives info-less,
so hostile `kos_call`s bounce with `ENOSYS` instead of burning cap slots and pinning the
server's priority.

**`kos_recv_info` never grew an input field, and the nesting is what keeps it that way.** A
timeout member on it would have put an *input* inside the struct every receive loop declares,
so a stack-garbage deadline would have been one line away and the rule against it would have
been a comment anyone could violate. Nested, the kernel's write-back stays a WHOLE-struct copy
at `opts + offsetof(struct kos_reply_recv_opts, info)`, with no uninitialised tail to leak and
no input word to preserve, and the input words above it survive every call.

## A copy the boundary check cannot promise

Both buffer bound-checks run at the syscall boundary, once. The COPY happens later -- for a
receiver, arbitrarily later, because it parked in between -- and two things can refuse it
then. `access_copy` reaches every granule through its own `arch_aspace_acquire`, which
answers null once that page is unmapped, so a sibling holding `AUTH_MEMORY` + `CAP_FRAME` +
`CAP_ASPACE` can `KOS_SYS_FRAME_UNMAP` the page under a thread parked in a receive. And
`ep_copy` refuses a copy whose two ends are the same memory under one owner, the primitive
under it being the ascending-only `kmemcpy`; two threads of ONE task share a space, so one
static array is one address for both ends of a rendezvous, and that route needs no
capability at all.

**Both ends are answered `-KOS_EFAULT`, and neither is a panic.** The rendezvous is the unit:
a sender whose copy into a parked receiver is refused answers `-KOS_EFAULT`, and that
receiver is WOKEN with `-KOS_EFAULT` rather than a byte count, because it is already off
`recv_waiters` and nothing else would ever wake it. The same holds in the other direction
(a receiver's scan over parked senders), for the fastpath call, and for `kos_reply`, whose
cap is consumed regardless. `endpoint_recv_locked`'s scan STOPS at the refusal instead of
continuing to the next sender, unlike the `ENOSYS`/`EMFILE` bounces: a bool does not say
which end went away, so a scan that continued could pop and fault every queued sender on the
receiver's own lost buffer.

**A refused `kos_recv_info` write RETRACTS the reply capability it was about to disclose.** A call
arm mints the one-shot `CAP_REPLY` into the receiver's table and only then writes the handle out,
so a refused `write_recv_info` would leave a capability installed whose holder was never told the
number of it: nothing can ever spend it, and the caller it names parks to its deadline or, untimed,
forever. `endpoint_recv_locked`'s `CALL_SEND_WAIT` arm and `endpoint_call`'s fastpath each undo the
mint
with `cap_uninstall_reply` and then answer both ends `-KOS_EFAULT`. That retraction is SILENT,
unlike `handle_close`'s `CAP_REPLY` arm, which answers the parked caller as a
close-instead-of-reply: here the minter is itself the one answering that caller, and a second
answer would be a second wake. The scan stops here as it stops at a refused copy, and with less
ambiguity than there: the out-pointer is the RECEIVER's own, so every sender queued behind this one
would meet the same fault.

An info-less receive has nothing to retract. `write_recv_info` answers true for `out == 0` and
moves no byte, and `endpoint_recv_locked` bounces a `CALL_SEND_WAIT` sender `-KOS_ENOSYS` ahead of
any
mint, so the retraction above is reached only where a real out-pointer was named.

**A refusal is not a no-op.** The copy stops at the first granule refused, so a prefix has
already landed: a buffer the kernel answers `EFAULT` about holds a head of the new bytes over
a tail of what it held, bounded by `KOS_EP_MSG_MAX`. That is why `0` is the wrong answer for
a receiver -- `n == 0` is a valid zero-length signal, and it would describe an indeterminate
buffer as an empty arrival. The residue is documented rather than engineered away, on
`KOS_ETIMEDOUT`'s precedent (`invariants.md`, `syscall-return-abi`): the promise is that no
party is told a transfer succeeded over a buffer in that state, not that the transaction was
undone.

The receive's own input words are outside this: the IN prefix of `kos_reply_recv_opts` is read
in one copy of 4-aligned words before anything is committed, so a refusal there is a plain
`-KOS_EFAULT` with nothing moved and nothing to unwind.

On a board that describes regions instead of translating, `access_copy` is a bare `kmemcpy`
and cannot fail -- the unmap route does not exist there, the frame syscalls being
`KICKOS_HAVE_ASPACE`-only. The overlap refusal is reachable on every board.

**Which is why `kernel/syscall/syscall_ipc_fast.cc` discards its `write_recv_info` answer, and is
not a third undisclosed mint.** A reader counting the sites that mint a reply cap and then disclose
its handle finds three, and only two carry the retraction above. That file compiles under
`KICKOS_ARCH_HAS_IPC_FASTPATH` alone, which is set by the armv6m, armv7m, rxv3 and rv32imac
`ipc_fastpath.cmake` ladders and by no translating arch, the two capability families being
exclusive. So `write_recv_info` there reduces to that `kmemcpy` and cannot refuse, and a retraction arm
beside it would be code no board can enter.

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
any arm that only reads results: dropping the `sched::set_prio` in `endpoint_wait_abort`
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

## A FAR call -- the receiver is in another kernel

Present only where `KICKOS_AMP_NODE`, and reached through the SAME `kos_call` /
`kos_call_timed` / `kos_send`: locality never reaches the API
(`../design-multicore.md` N7). What decides it is the endpoint object, whose
`far_node` (biased by one, so 0 is local) and `far_port` are the whole answer and
have no flag beside them to disagree with. `endpoint_is_far` folds to a constant
`false` below the AMP posture, so every arm below compiles out there.

**Where a far endpoint comes from.** The PARTITION states it: `CONFIG_KICKOS_AMP_PORTS` names
every crossing once for the whole partition, and the kernel seats this node's derived set into
root before root's first instruction (`../design-multicore.md` N6g). An entry's position in that
list is its capability handle, `KOS_CAP_FIRST_DYNAMIC + i`, and `<kickos/amp.h>` is how an app
spells it: `kos_amp_port(node, port)` answers the handle, or `KOS_CAP_NONE` for a crossing the
partition does not name. No syscall is involved and nothing is minted at run time.
`KOS_SYS_AMP_ENDPOINT_CREATE` remains the privileged mint of N8 and has no caller in the tree.

**A NODE THAT SERVES MUST NOT RETURN FROM `main`.** Root returning ends the system
(`<kickos/sys/init.h>`), and the nodes of an AMP partition share one machine, so a serving
node's main taking the exit halts its callers too. It is the same rule any persisting init
already lives by and not a new burden the partition invents; what the partition adds is that
somebody else is affected by breaking it. A node that only CALLS may return as any app does.

**Which capability a node gets is its own reading of the same entry.** An entry naming this node
is a LOCAL endpoint carrying `CAP_WAIT | CAP_SIGNAL`, with the port bound to it, so a call
arriving on that port reaches a thread parked in an ordinary receive. An entry naming another
node is a FAR endpoint carrying `CAP_SIGNAL` alone, which is what refuses it at that receive's
resolve with no branch of `endpoint_recv_locked` having to learn about locality.

**A pooled slot keeps its last occupant's fields**, so a far endpoint that is closed would
leave its route standing for whatever local endpoint lands on that slot next. Both fields
are reset through the ONE slot claim in `kernel/syscall/syscall_ipc.cc` that every endpoint
allocation goes through, which resets the whole aggregate; each caller then applies only the
mode fields it owns. There is deliberately no per-caller list of fields to forget, a list
being incomplete the moment a field is added.

**Placement is load-bearing.** In both `endpoint_send` and `endpoint_call` the
locality decision is taken immediately after the capability resolve and AHEAD of the
`recv_holders == 0` test. A far endpoint carries no local receiver -- its capability
is minted with `CAP_SIGNAL` alone and `recv_holders = 0` -- so a dead-endpoint refusal
taken first would answer `-KOS_EPIPE` for every far send and every far call.

**The reply token.** `amp::ReplyTag` is two words carried across the shared window and
handed straight back to the taker; nothing in `kernel/amp/ampwindow.cc` spends either,
which is the whole of why an unvalidated one may cross.

- `tag.thread` -- the caller's generational thread handle, `ThreadPool::handle_for`,
  whole and unshifted, exactly as a `CAP_REPLY` entry's `obj` carries it.
- `tag.seq` -- the caller's `call_seq` WHOLE, all 16 bits of it, through `amp::reply_seq`, the
  ONE conversion to the wire width (`amp::REPLY_SEQ_MASK`). The same late-reply ABA guard as the
  local arm's, over the whole field rather than a byte of it, so no retry count aliases a live
  call: the sequence comes round only once the caller has wrapped its own field.
- `amp::REPLY_TAG_NONE` -- the route of a sender that does not park. It is NOT the zero
  tag: zero is index 0 at generation 0, which a live slot can be, so it carries
  `KOS_THREAD_NONE`, whose all-ones index the thread pool reserves and never seats.

`endpoint_send` publishes `REPLY_TAG_NONE` and never parks. `endpoint_call` decides
`call_seq + 1`, builds the tag from it, publishes, and COMMITS the sequence only once the
publication is taken, so a refused call leaves the caller exactly as it found it.

**Delivering it (`endpoint_far_reply_deliver`).** The window's `PORT_REPLY` route runs it
from inside the doorbell service body, so this core's interrupts are masked and that mask
is the exclusion; no `IrqLock` is taken. FIVE clauses, and any failure counts
`amp::Counts::reply_drop` and drops:

1-4. `cap_reply_thread(tag.thread, amp::reply_seq(tag.seq), amp::REPLY_SEQ_MASK)`, which is the
   same body `cap_reply_caller` runs over a `CAP_REPLY` entry: index in range, thread-slot gen
   match, `state == BLOCKED` with `call_state == CALL_REPLY_WAIT`, and the sequence match under
   the mask this arm's storage carries -- all 16 bits here, the 8 a `CAP_REPLY` entry can hold
   there. One body and not a copy -- a second set of these clauses would be a second answer to
   whether a reply may land. A node whose `ThreadPool` is zeroed has `next == 0` and refuses every index at
   the first clause, which is what makes the same call inert on a peer that runs no kernel
   of its own.
5. **The cross-node clause**, which the four above cannot carry: the resolved caller must
   be parked on a far endpoint whose node equals the RING the reply arrived on
   (`caller->wait_far_endpoint()`, `WAIT_EP_FAR_REPLY`). The four above are satisfied by any
   caller parked in a call, a LOCAL one included, so without this node 2 can complete a call
   node 0 made to node 1.

On success the length is clamped into `call_rx_cap` and copied with `kaccess_to_user`,
whose result is NOT asserted -- `cap_console_deliver` is the standing precedent for copying
into a parked thread from a masked handler, and a refused copy tells the caller nothing
arrived rather than panicking inside one. Then `wait_result`, `call_state = CALL_NONE`,
`clear_wait_edge()` and `sched::wake`.

**The park.** `WAIT_EP_FAR_REPLY`, `wait_obj` the far `Endpoint`, queue-less on no list at
all. `endpoint_wait_abort` unwinds it with no donor list to unlink and no server to deflate,
and it MUST bump `call_seq` for the same reason the local `WAIT_EP_REPLY` arm does: a reply
still in flight names the call by its sequence alone, so a sequence left standing resolves to
this thread again once the caller's `call_seq` has come round -- after exactly 65536 further
calls on this arm, which carries the field whole, and after exactly 256 on the local
`CAP_REPLY`, whose spare bitfields carry 8 bits of it.

**What DIVERGES from a local call is back-pressure, and it acts in BOTH directions.** A local
caller parks on `send_waiters` until a receiver takes its request; a far caller cannot, the ring
being finite and the far scheduler not this kernel's. So a full peer ring answers
`-KOS_EBUSY` immediately, with nothing mutated. Per `../design-multicore.md` N6e that
refusal IS the contract: named, counted, spending no time, with no queue and no retry
behind it, because how stale a dropped record may be is the workload's property and not the
kernel's. The rest of the window's refusals map to `-KOS_EPIPE` (a far index this node
cannot believe) and `-KOS_EINVAL`.

**Back-pressure also exists on the RECEIVING side, and it is not an errno and not a loss.**  A
serving node admits a call only where the reply ring toward that sender has room for one more
reply than it already owes that sender (`../design-multicore.md` N6f). A peer that has not
drained its replies therefore has its calls left UNREAD rather than taken and answered into a
ring with no slot: nothing is published, the consumer's cursor does not move, and the call is
taken by the service pass THE PEER'S OWN DRAIN RAISES. That raise is what makes the refusal a
delay rather than a strand: a take that advances a reply ring's tail rings the node whose answers
that slot belonged to, because a credit return is not a publication anybody rescans
(`../design-multicore.md` N6f). The caller sees only the latency; the refusal is
the serving node's own and reaches userspace as the probe verdict `KOS_AMP_V_RESERVE`, counted in
`amp::Counts::reply_reserve` apart from the producer-side `send_refused` so that a take declined
and a publication refused are never read as one number.

**AND WHAT IT OWES IS NOT DERIVED FROM THE HELD RUN ALONE.** A CALL-ring resynchronisation sets
`taken = tail = head` with the released mask clear, so a run of length zero can still owe a reply
slot: the record the clause left pending (`amp::Inbound::run_gone`) is counted beside the run
rather than out of it. The masked slot cannot say which is which -- an abandoned record and a
later wrap's call share one -- so the record carries the fact. Counting the run alone admits one
call too many per such record, and the answer to each of those is a publication the reply ring
then refuses, which is how a GENERIC malformed peer reaches the full-ring state above rather than
only a targeted one. `run_gone` also decides that such a record's slot is never spent as a
release again: `release_call` masks its index against the run that stands NOW, so releasing it
would hand the peer back a slot this node is still being served on.

**AN ANSWER THE REPLY RING REFUSES ANYWAY LOSES ITS BYTES AND NOT AT ONCE ITS CALLER**, and the two
halves of that are separate mechanisms. It is reachable only where a peer regressed a tail under a
reservation it had already granted. The BYTES are gone: the reply capability was spent to reach the
publication, so nothing holds what would remake them, and `amp::Counts::reply_unsent` counts one,
readable from userspace through the `KOS_AMP_OP_REPLY_UNSENT` probe op. That op is the only signal
outside the kernel that an answer was lost, which is why the counter and it land together: the
exceptional loss would otherwise be invisible exactly where it would be diagnosed.
The OBLIGATION is not: the record is left pending with its call slot still held, and a later
service pass publishes an empty `PORT_REPLY` carrying its tag once that ring is believable again
(`amp::inbound_reply` defers, `amp::node_service` discharges, between the reply drain and the call
drain). **THE SLOT IS HELD BACK ON PURPOSE, AND THAT COSTS A BOUNDED DELAY OF THE RUN BEHIND IT
RATHER THAN A DEAD CALL RING**: what it buys is that no later call lands on a slot whose caller is
still owed an answer, which releasing it at the refusal gave up.

**AND THE OBLIGATION CARRIES A BOUND OF ITS OWN, `amp::DEFER_PASSES` SERVICE PASSES, BECAUSE THE
PRODUCER'S STRIKE BOUND CANNOT REACH THE STATE THAT NEEDS ONE.** An EXACTLY FULL reply ring is an
outstanding count of `KOS_AMP_RING_SLOTS`, which is what a well-formed consumer presents while it
has read nothing, so `tail_believed` believes it and no strike accrues -- deliberately, since
resynchronising a legitimately full ring would destroy a slow peer's unread answers, which is data
loss where there was none. A node holding an answer for such a ring owns neither index and can
publish nothing, so an unbounded obligation there is the whole held run behind it pinned for the
life of the image, every later call meeting a masked slot the standing record refuses, and
`send_refused` climbing once per doorbell any other peer raises. At the bound the call slot goes
back and the record dies UNANSWERED: no far index is written, the peer's own unread answers stand,
and what is given up is this answer's wire refusal alone -- which is the state that held before
the slot was ever held back. **The bound is PER RECORD**, so a peer that drains at the last pass
keeps every obligation behind the one that expired, and the run is back within
`RING_SLOTS * DEFER_PASSES` passes at worst. A pass is any doorbell this node takes: on a
partition wider than two nodes an unrelated peer's traffic spends the budget too, which is the
price of measuring in passes rather than in a time this path has no clock for.

**AND THE PAYLOAD IS DELIBERATELY NOT RETAINED.** A staging buffer per record would make the kernel
the keeper of an answer's staleness, which is the workload's property and nobody else's
(`../design-multicore.md` N6e); an empty reply carries no such policy, saying "no answer", which is
as true a tick later as it was at the refusal. And in the case that motivates the retry at all, a
peer that regressed or restarted that tail, the caller the payload would be delivered to may no
longer exist: the tag then resolves to nothing at the far end, or past a 65536 wrap to a stranger.
The obligation-only shape is correct in both sub-cases and costs one byte per record.

**THE PRODUCER HAS THE SAME STRIKE BOUND THE CONSUMER HAS, AND IT IS THE REPLY RING'S ALONE.** A
far tail whose outstanding count exceeds the ring's depth cannot come from a well-formed consumer
at all: that tail only advances, and this node publishes nothing past `KOS_AMP_RING_SLOTS` beyond
the tail it last read. After `amp::DEPTH_STRIKES` consecutive such readings on one reply ring the
index THIS NODE OWNS is resynchronised to the far tail, the publication that reached the bound is
taken rather than refused once more, and `amp::Counts::tail_reset` counts one
(`KOS_AMP_OP_TAIL_RESET`, the producer's counterpart of `KOS_AMP_OP_DEPTH_RESET`). What it abandons is
this node's own answers the peer's regression had already declared it would not read, and the far
index it adopts is still spent modulo the ring's depth alone. Both readers of that tail run it: the
publication in `amp::send` and the admission test in `take_call`, the second being what makes the
recovery reachable on a node whose every take is refused and which therefore has nothing of its own
to publish. It introduces nothing on the wire: the strike count is this node's private state, so no
epoch and no second index sits beside the ring's own release point.

**THE CALL RING HAS NO SUCH BOUND, AND THAT ASYMMETRY IS THE ARGUMENT AND NOT AN OMISSION.** A
refused CALL is answered to an application that can act on it, so a call ring this node has stopped
believing costs no caller its answer, only its own sends. Discarding an outstanding call would
strand a caller already parked on that publication, and nothing on this node could then answer it.
The recovery a regressed call-ring tail needs is a coordinated restart, which is a mechanism this
layer does not have.

**A ZERO-LENGTH `PORT_REPLY` CARRIES SEVERAL MEANINGS AND CARRIES NO REASON, AND THAT IS
RULED RATHER THAN INHERITED.** On the wire it is a genuine empty answer, a call refused after its
slot was taken, a caller whose route the resynchronisation stopped believing, and a deferred answer
whose bytes were lost. A receiver cannot tell them apart, which the contract already said of the
first two, and the two added here do not change what a caller may do about any of them.

*Why no reason code rides it.* Not for want of a bit: `ReplyTag::seq` is a 32-bit field carrying a
16-bit `Thread::call_seq`, so half of it is structurally spare. It is refused because writing it
would make the SERVING node an author of the CALLING node's own route word, and the whole of why an
unvalidated tag may cross is that the serving node spends neither field and hands both back exactly
as they arrived (`../design-multicore.md` N6f). A reason composed there is a reason a malformed peer
can compose too, so the calling node would owe it validation, on the reply path, for a value no
caller can act on differently: none of these outcomes is retryable from inside the kernel, and
whether to retry, drop or escalate is the workload's (N6e). And a partial distinction would be worse
than none, since a genuine zero-length answer would stay folded in with the refusals whatever code
were added, so a receiver told "refused" would over-trust a signal silent on the one case it most
needs to see.

*Where the distinction does live.* At the node that caused it, as a count, which is what makes it
diagnosable at all: the resynchronisation's answers and the deferred ones each count
`amp::Counts::reply_unsent` (`KOS_AMP_OP_REPLY_UNSENT`), where a call refused past its take counts
nothing of the kind, and the producer's recovery counts `amp::Counts::tail_reset`. So the added
meanings are one wire shape with a per-node name, not an extra unnamed one.

*And a payload copy this kernel refuses is NOT one of them, because it does not need the wire.*
`endpoint_far_reply_deliver` answers `-KOS_EFAULT` on a refused `kaccess_to_user`, handing it
through `Thread::wait_result` to its own parked thread. That is the calling node writing to its own
TCB, not a message crossing a window, so the code reaches that caller by exactly the channel the
local rendezvous uses, with nothing on the wire changing and nothing in `kernel/amp/` involved.
`endpoint_far_call_deliver` answers the same way and on the same channel -- for its receiver's
payload copy and for a refused `write_recv_info` alike -- that receiver being a parked local thread
already off `recv_waiters`. So no far arm is an end of the wire that describes a lost buffer
differently. A masked doorbell body having no syscall return is not what stands in the way
of a code: neither arm needs one, each having a parked TCB to write instead.

*The count is held apart from `reply_unsent`, and holding it apart is the point of both fields.*
`amp::Counts::deliver_fault` (`KOS_AMP_OP_DELIVER_FAULT`) counts an arrival this node could not put
into a local thread's buffer, once per arrival however many of its copies were refused. Folding it
into `reply_unsent` was refused: a non-zero row there must keep naming a malformed or regressed
PEER, where a refused copy is this node's own buffer fault on a message that arrived intact.
`reply_drop` is not its home either, that field naming a reply the tag validation turned away,
which is a reply that reached no caller at all -- where this one resolved a live parked caller, woke
it, and could not fill its buffer.

**AND A RESYNCHRONISATION ANSWERS THE CALLERS IT ABANDONS.** The depth clause on a CALL ring
abandons a sender's whole held run at once, which is what makes it memory-safe (`amp::Inbound`'s
generation), and every live record in that run names a far caller parked on an answer. Each is
published an empty `PORT_REPLY` carrying its own tag before its record dies, read out of the record
while it can still be named; each counts `amp::Counts::reply_unsent`, the service holding that
capability being about to find its token dead. A publication refused there defers exactly as
`amp::inbound_reply`'s does.

**Donation does not cross.** There is no thread on the far side to raise and no seam by
which this node's priority reaches a peer's scheduler, so D1 and D2 have no far arm and D3
has nothing to recompute. This is stated and not solved (N6e), and it is deliberately NOT an
errno: refusing a far call because it cannot donate would be locality reaching the API,
which is exactly what N7 forbids. The sharper half is that the two priority scales are
unrelated number lines, so meeting a deadline across nodes is a system-integration act
across two configurations and neither kernel can check the result.

**The CALLING node holds no reply capability.** Its whole route is the tag, so there is no
one-shot cap to consume and no `reply_waiters` membership on this side; what makes the delivery
one-shot is `call_state`, the wake setting `CALL_NONE` so that clause 3 refuses every later reply
carrying the same tag. **The SERVING node's receiver holds an ordinary `CAP_REPLY`.** What N6d
forbids is naming a far kernel's own handle table, and nothing here does: the record lives in the
SERVING node's table (`amp::inbound_seat`), and the capability `cap_install_far_reply` seats into
the receiver's own run carries a handle in the thread pool's RESERVED BAND
(`ThreadPool::far_reply_handle`) rather than a thread handle. That band is how a far reply
capability exists without a far table being named, and `cap_reply_thread`'s first clause is what
keeps a band handle from resolving to a local thread -- the pool's `next` never reaches
`FAR_REPLY_BASE`, asserted at compile time. So `kos_reply` IS on this path, on the serving side.

**The serving side (`endpoint_far_call_deliver`, `kernel/syscall/syscall_ipc.cc`).** The doorbell
service takes one CALL and HOLDS its ring slot: that slot IS this node's record of the far caller
until the reply is published, which is what bounds outstanding inbound calls from one peer at
`KOS_AMP_RING_SLOTS` (`../design-multicore.md` N6f). **The reply slot is reserved UP FRONT**: the
take is admitted only where the reply ring toward that sender has room for the answer, and the
answer consumes that reserve whether the receiver SERVED the call or the take was refused past it
-- an empty reply is a reply. So `-KOS_EBUSY` at the caller and `RESERVE` at the taker are the two
ends of ONE bound and not two unrelated refusals: the caller meets a full call ring toward the
serving node, and the taker declines because the reply ring back toward that caller is full. Then,
in order:

1. the port's bound endpoint is resolved BY INDEX (`amp::port_endpoint`), the bind itself holding
   a second endpoint reference so the slot cannot be freed under a `g_port_ep1` entry that still
   names it. A port bound to nothing, or an endpoint with no `recv_holders`, refuses here.
2. `wq_pop_highest(e->recv_waiters)` takes the receiver, and a call finding nothing parked is
   refused ON THE SPOT rather than held for a service that may arrive. Once popped, that receiver
   is COMPLETED whatever follows: it is off its queue, so an early return would park it on nothing.
3. a record is seated for the held slot (`amp::inbound_seat`) and a `CAP_REPLY` installed for it
   (`cap_install_far_reply`), gated on the receiver having asked for info -- an
   info-less recv has nowhere to be handed a capability, so the seat is never taken rather than
   taken and undone. A seat or an install that fails FORGETS the record (`amp::inbound_forget`)
   and the slot stays the taker's to release.
4. the payload is copied with `kaccess_to_user`, truncated to the receiver's `ipc.len` as any
   datagram is, and `kos_recv_info` carries `KOS_BADGE_NONE` -- a far sender holds no badge this
   kernel minted -- beside the reply handle. **A refused copy and a refused info write are ONE
   fault with ONE answer**: the receiver is woken `-KOS_EFAULT` with `reply_cap` reading
   `KOS_CAP_NONE`, the mint is undone (`cap_uninstall_far_reply` + `amp::inbound_forget`), and
   the far caller is answered by the ONE publish site every refusal past the take funnels
   through (`dispatch_call`, below). A capability is
   disclosed only BESIDE THE BYTES IT ANSWERS FOR, which is the rule the local rendezvous keeps
   by minting only after its own copy: a receiver that never saw the request is handed no
   obligation, and one told nothing of a handle could never spend it.

The receiver then answers with an ordinary `kos_reply`. The record routes it to
`amp::inbound_reply`, which publishes it on the sender's `amp::PORT_REPLY` with the tag verbatim,
releases the held call slot, and frees the record. Reclamation of the call ring is by a released
mask, so replies may complete OUT OF ORDER while the tail still advances in order.

**EVERY REFUSAL PAST THE TAKE PUBLISHES AN EMPTY `PORT_REPLY` CARRYING THE TAG, and that is the
wire's only refusal shape.** No refusal message and no errno crosses the window: a far caller
under `KOS_TIMEOUT_NONE` has no deadline, so a refusal that answered nothing would leave that
thread parked for the life of the image. Every refusing arm above therefore funnels through ONE
bool whose single caller publishes the answer and releases the slot (`dispatch_call`,
`kernel/amp/ampwindow.cc`); an answer written per arm would be a new leak for each arm added. A
caller cannot distinguish a refusal from an empty answer, and `amp::PORT_ECHO` is the one port the
window layer answers with the payload rather than with nothing.

**The register fastpath refuses a far endpoint** as a fall-through returning null, never an
errno, so `endpoint_call` produces the answer. It folds to nothing on every part that links
the fastpath today.

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
which is load-bearing because a timed call can leave a stale cap behind. A teardown wake runs with
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
- **`cap_console_deliver` is the one route left answering a lost buffer with `0`.** Every IPC
  path answers `-KOS_EFAULT`, the far arms included: see "A copy the boundary check cannot
  promise" for the local rendezvous and the far-window section above for
  `endpoint_far_reply_deliver` and `endpoint_far_call_deliver`. The console route keeps `n = 0`
  for both of its refusals. Half of that is structural: its producer is the fault reporter
  descending through `kconsole_write`, so there is no caller to answer and the byte count is
  the whole of what it returns. The other half is not -- its parked receiver is woken through
  the same `Thread::wait_result` the far arms carry a code on, so nothing stops a code
  reaching it. What is missing is a ruling on what a console consumer should be told about a
  record it holds a prefix of, which is that route's contract and not this one's. Until then
  `0` cannot be told from a zero-length line on that route, and on no other.
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
