<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# M9.5 whole-transaction IPC lock feasibility

> **Result: no small per-object prototype is ready to benchmark.** This is a
> source-level feasibility gate, not a performance measurement or a claim that
> fine-grained locking is impossible. The existing owner-local prototype and
> its measured mixed-workload cost remain the performance evidence. A safe
> per-object version requires a new lifetime and publication protocol across
> IPC, capabilities, deadlines, scheduler and context switching.

## Scope of the proposed cut

The proposed gain is simultaneous CALL/REPLY transactions on independent
endpoints. To preserve the current ABI, the cut must admit the slow path,
cross-core peers, deadlines, cancellation, last-WAIT-cap close, server death,
reply-cap close, priority donation and thread migration. Restricting it to a
fast local pair recreates the owner-local second path that was already measured
and removed ([mixed result](archive/M9.5_x86_ipc_mixed.md)). An endpoint lock
alone cannot be the ownership rule for all of these paths.

| Operation | State read or changed in one current `IrqLock` span |
| --- | --- |
| Fast CALL | Caller cap run and endpoint pool; endpoint receive queue; receiver TCB and cap run for reply mint; both IPC buffers and mapping; donor link and effective priority; caller deadline; ready placement and parked frame. |
| Slow CALL and RECV | Endpoint send/receive queue and server link; caller or receiver TCB; deadline list; donation to the endpoint's server; ready placement and parked frame. |
| REPLY/REPLY_RECV | Server cap run and reply-cap generation; caller handle, wait edge and donor link; caller deadline and buffer; server priority; caller wake; optional notification and next receive/park. |
| Timeout/cancel | Global deadline list or target control state; endpoint queue or server donor link; call generation and priority; ready placement. |
| Last WAIT-cap close/teardown | Owner cap run; endpoint holder/ref counts and pool slot; served-endpoint chain; every queued sender; each sender's wake and deadline; possible console-driver death. Teardown releases its BKL between chunks. |

These are actual dependencies in `syscall_ipc.cc`, `cap.cc`, `park.cc`,
`time.cc`, `sync.cc` and `sched.cc`; they are not a list of proposed locks.

## Lock-order witnesses

1. **Endpoint to cap run, cap run to endpoint.** Fast CALL must pop a receiver
   from the endpoint before it can mint a reply into *that receiver's* cap run.
   Close and teardown start with a cap entry, then run the endpoint close
   protocol, possibly clearing the server and waking all senders. Locking in
   existing operation order gives `endpoint -> receiver cap run` and
   `owner cap run -> endpoint`. A fixed order requires staging one operation:
   pin the discovered receiver and endpoint, release/reacquire in a canonical
   order, then revalidate queue membership and cap capacity. A failed
   revalidation needs a defined bounded retry or refusal. The current
   generation check does not pin an already resolved pointer.

2. **Endpoint to timer, timer to endpoint.** A timed CALL links its wait edge
   and arms the deadline before another core may wake it. Timer expiry removes
   the deadline, then `endpoint_wait_abort` unlinks the endpoint wait or donor
   edge and calls the scheduler. Directly locking the existing order gives
   `endpoint -> timer` in CALL and `timer -> endpoint` in expiry. Reversing one
   order creates a gap in which expiry can run before the park is published;
   closing that gap needs a new pending/committed wait state and an owner for
   expiration during the transition. REPLY's wake also cancels the timer.

3. **One endpoint does not bound the priority funnel.** A reply, timeout or
   close can recompute a server's effective priority by walking held mutexes,
   reply donors and *all* endpoints in its served chain. Another core can
   change one of those lists through an unrelated endpoint. Protecting only
   the current endpoint therefore does not protect the recompute. A server
   lock could own those links, but then endpoint-server reassignment, mutex
   operations and every donor path must follow its order too.

4. **A parked frame still needs an owner.** `sched::wake` can publish a thread
   to another core or switch immediately. The current `klock_detach` and
   `kickos_switch_unlock` retain the *single* BKL until the outgoing frame is
   saved. Per-endpoint locks cannot simply be released at syscall return:
   another core could wake or reclaim the parked TCB before its frame exists.
   A per-core scheduler lock would need its own detach/attach handoff and a
   rule for operations that wake a thread while holding endpoint, timer or cap
   locks. The existing work rings solve placement after publication, not this
   frame-lifetime boundary.

5. **The BKL paths must join the new protocol.** Last-WAIT close, reply-cap
   close, teardown, timer expiry, cancellation, affinity changes and mapping
   changes currently exclude IPC by taking `IrqLock`. Once IPC stops taking
   that lock, those paths are concurrent with it. Leaving them as-is is a
   race; making the BKL wait for object-local spans is the second exclusion
   protocol of the removed owner-local experiment. A genuine single protocol
   must convert all of these mutators and the endpoint/thread pool reclaim
   rules together.

## Decision for this experiment

The first plausible single protocol needs at least: stable pins for resolved
endpoints and TCBs; an ordered cap-run/endpoint/server/deadline/scheduler lock
hierarchy; a staged timed-park state; a switch-frame handoff; and conversion of
close, teardown, timer and cancellation to those same rules. It also needs a
bounded answer for each revalidation and for the number of objects visited by
priority recomputation. That is a kernel-wide concurrency redesign, not a
small IPC lock substitution. Implementing just its fast CALL arm would provide
no valid benchmark candidate under the stated full-transaction criterion.

The owner-local experiment already demonstrates the potential gain and the
cost of a narrow second path: at 12 pinned x86 cores it gained 2.06x with one
cross-core pair and 1.20x with six, but lost 6.35% when all twelve pairs were
cross-core; its four-core global-only yield control lost 4.42%. The shipped
single CLH BKL remains the comparison point. No new throughput claim is made
here, and no runtime lock change follows from this audit.

Reopen a whole-transaction prototype only if a concrete design specifies the
above lifetime, lock order and frame handoff without an unbounded retry or a
second exclusion path. Then measure local, mixed and all-remote IPC at 1-4,
6 and 12 pinned physical cores before considering it for the shared kernel.
