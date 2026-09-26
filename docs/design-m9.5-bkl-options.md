<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# M9.5 BKL alternatives on the 12-core x86 bench

> **Status: DECIDED.** Keep one BKL for shared-kernel transactions. x86_64 uses
> CLH arbitration for that lock; the other shared-kernel backends retain
> ticket locks. The owner-local IPC experiment was removed after its mixed
> workload gate. A measured gain does not change an exclusion or lifetime rule.

## The workload and the constraint

The pinned x86 baseline has twelve guest cores on twelve distinct physical host
cores. Independent IPC pairs contend on the BKL. In the one-fast-core 8-byte
transaction, call + receive + reply spend about 79% of their locked cycles in
handoff/parking, 1% in copying, and 20% elsewhere
([phase record](archive/M9.5_x86_ipc_baseline.md)). A read-side optimization
cannot speed the mutating handoff merely by making a capability lookup cheaper.
The owner-local experiment showed that bypassing global exclusion can help a
purely local workload greatly, but its second protocol lost on an all-cross-core
workload and was removed ([mixed record](archive/M9.5_x86_ipc_mixed.md)).

The evaluation rules are: one understandable exclusion and lifetime story;
fixed storage; a stated maximum number of predecessors, retries and objects
visited on deadline paths; no blocking while a lock is held on a peer that may
need that lock; and no unbounded retry or reclamation grace period hidden in an
otherwise bounded call. Current BKL **time** is not proven bounded merely by
its FIFO arbitration: its holder maximum remains an M9 inventory item.

## Candidate map

| Approach | What it could improve here | Current disposition |
| --- | --- | --- |
| Change the single BKL from ticket to CLH queueing | Waiters read a predecessor's cache line rather than one shared serving line. No new lifetime domain. | **Accepted on x86_64 only.** It improves contended local IPC and lock-only yield, with small changes at low width and for cross-core IPC. It remains one global exclusion domain. |
| Ticket-lock backoff or parking | Reduce repeated loads of the serving line on other backends. | Not adopted for M9.5: no evidence that it would change the BKL decision, while delaying a newly served core raises handoff latency. Sleeping cannot replace the required doorbell poll while local interrupts are masked. |
| Shorten BKL hold without changing ownership | Copy and validation can move only with safe snapshots/revalidation. | Copy alone is below 2% of measured locked cycles. The large part is park/wake/switch and cannot simply move past the current frame-publication point. |
| Trap-level IPC fastpath under the BKL | Shorten the critical section while retaining one exclusion domain, as seL4 does. | The existing KickOS register fastpath is deliberately single-core and x86 has no backend; SMP is rejected at configure time. A new x86 path would duplicate the IPC transaction and switch logic. It is a separate maintenance decision, not an unlock of the current fastpath. |
| Lockless reads with sequence counters | A small, pointer-free read-mostly snapshot could avoid the BKL. | **No IPC win established.** The IPC transaction mutates endpoint, TCB, cap and scheduler state. A retrying reader has no finite retry bound; a cap lookup follows reclaimable pointers. Keep available for a separately measured read-heavy snapshot. |
| RCU or epoch-protected lookups | Make readers cheap and defer object reuse. | **Lifetime project, not a small BKL edit.** Removal must be followed by a grace period before reuse; parked IPC and teardown need a complete reference policy. No bounded grace/reclaim storage is designed for this kernel. |
| Fair reader/writer lock | Concurrent genuinely read-only operations. | The measured IPC call/reply path is a writer. The BKL-protected scheduler probe is selftest-only; clock and frequency reads already avoid it. A writer-fair implementation still needs a separate reader lifetime contract. No relevant read-heavy workload is present yet. |
| Per-object locks and local scheduler ownership | Independent endpoints may run in parallel, including cross-core calls. | **Architecturally plausible, larger redesign.** A call touches at least a cap table, endpoint wait queue, two TCBs, reply cap, donation/deadline state and scheduler placement. A lock per endpoint alone leaves the global scheduler and object teardown serialized; a complete version needs lock order, handoff and lifetime proofs. Evaluate only with a whole transaction design and a workload that beats the single-lock options. |
| Single-reader/single-writer IPC rings | Asynchronous transfer between a fixed pair. | Does not preserve synchronous `CALL`/`REPLY_RECV` semantics by itself: multiple callers, one-shot reply caps, cancellation, timeout, priority donation and user-buffer lifetime remain. The existing per-core scheduler work rings already cover a narrower fixed-producer/fixed-consumer boundary under the BKL. |
| Generation-only revocation or lock-free object pools | Reject stale handles in constant-time lookup. | **Insufficient for in-flight use.** A generation mismatch detects a later lookup, but a resolved `Endpoint*` or `Thread*` can outlive the check. Current close/teardown also edits refcounts, queues and free lists. Reuse still needs exclusion or quiescence. |
| Hardware transaction, speculative lock elision, unbounded CAS retry | Potentially fast uncontended execution. | Excluded from the deterministic common path: finite retry and fallback behavior would require a second exclusion protocol, and hardware availability varies across the fleet. |

The [seL4 CLH lock](https://github.com/seL4/seL4/blob/master/include/smp/lock.h)
shows that a queue lock can remain the **one** BKL. Linux's
[sequence-counter documentation](https://docs.kernel.org/locking/seqlock.html)
explicitly requires retrying readers and excludes data containing pointers a
writer can invalidate. Its [RCU documentation](https://docs.kernel.org/RCU/whatisRCU.html)
requires delaying reclamation until pre-existing readers have finished. The
existing [reference-kernel survey](design-m9-reference-kernels.md) compares the
other lock-domain choices against KickOS's scheduler and capability model.

## One BKL with CLH arbitration on x86_64

The ignored workspace directory `build/m95/clh-study/` holds the prototype
sources, matched EFI images and raw captures. The accepted x86 change replaces
only `arch_kernel_lock` and `arch_kernel_unlock`: one
cache-line-aligned request per core plus a sentinel, a cache-line-aligned node
per core, one atomic tail exchange per acquisition, predecessor polling with the
existing doorbell service, and a release store that grants the next turn. It
keeps FIFO order and the same outer `IrqLock` protocol. The released holder
reuses its predecessor's request on its next acquisition; its successor can
keep polling the released request until its own turn. The interrupt mask allows
only one outstanding request per core. No call, reply, scheduler, or capability
lifetime path changes. CLH has no bounded number of tail-exchange attempts above
the architecture's atomic primitive, and, as with the ticket lock, FIFO order
does not bound the wall time of a predecessor's critical section.

The final benchmark records a zero draw-retry sample per acquisition because
the exchange has no retry loop. It leaves `draw-queue` unsampled (`n=0`), since
the predecessor chain exposes no queue length. This is distinct from the
prototype's misleading zero-valued queue samples.

Each rate below is the median of three ticket and three CLH KVM runs, 50,000
calls per active pair, with each guest vCPU pinned to a distinct physical host
CPU. The first matrix alternated images; the last three rows repeat the final
instrumented CLH build against the same ticket images in the same measurement
window. Host CPUs 0-3 are the faster class on this Ryzen AI 9 HX PRO 370;
CPUs 4-11 are the slower class, so comparisons across widths must account for
which class is active. Both 8-byte and 256-byte requests and replies were
checked. `remote pairs` counts clients on a different guest core from their
server. The full matrix and evidence are in
[the CLH measurement record](archive/M9.5_x86_clh.md).

| Guest cores and placement | Remote pairs | Payload | Ticket calls/s | CLH calls/s | Change |
| --- | ---: | ---: | ---: | ---: | ---: |
| 2 fast, 2 local pairs | 0 | 8 B | 624,525 | 639,060 | +2.3% |
| 2 fast, 2 local pairs | 0 | 256 B | 615,600 | 627,264 | +1.9% |
| 4 fast, 4 remote pairs | 4 | 8 B | 70,571 | 72,138 | +2.2% |
| 4 fast, 4 remote pairs | 4 | 256 B | 70,076 | 71,476 | +2.0% |
| 12 mixed, 12 local pairs | 0 | 8 B | 258,891 | 320,453 | +23.8% |
| 12 mixed, 12 local pairs | 0 | 256 B | 250,364 | 317,326 | +26.7% |

The earlier complete matrix found +12% at eight slow cores with local pairs,
about +1-2% for mixed or all-remote IPC at twelve cores, and +47.5% for the
lock-only yield probe at twelve cores. One local-pair CLH run timed out with
only OVMF output, before the kernel's first entry marker. The next run and
subsequent twenty CLH and twenty ticket boot checks completed. The first
kernel marker precedes lock execution, so that timeout cannot establish a CLH
runtime deadlock; its firmware/pre-entry cause was not localized. Five final
12-core selftests and a four-core selftest reported all tests passed and clean
exits. The x86 trap-redzone gate and static gates passed. The final 12-core
lock benchmark reached `bench: done` and a clean exit; its `draw-queue` row is
truthfully unsampled. The registered `qemu_x86_64_bench_lock` and selftest
gates pass at every x86 SMP width under TCG.

### ARM64 and LX6 portability check

The unchanged four-core `qemu-arm64-benchsmp` ticket-lock image built and
passed `qemu_arm64_bench_lock`, `qemu_arm64_smp_doorbell`, and
`qemu_arm64_selftest`. A temporary CLH version using an inline `LDAXR` /
`STLXR` exchange also passed those three QEMU tests and was then removed.
A first version using GCC's generic atomic exchange instead faulted on its
first lock call: the compiler emitted an out-of-line
`__aarch64_swp8_acq_rel` helper linked at the app address, and the kernel
attempted to execute it there. The inline exchange avoided that cross-address
runtime call. ARM64 QEMU on this x86 host checks function, not physical-core
throughput, so the ticket lock remains shipped on ARM64.

A twelve-core GICv3 QEMU posture then exercised the same architecture-specific
CLH exchange. Both ticket and CLH passed twelve-core arrival, doorbell, lock
and selftest gates, and all twelve workers and IPC pairs completed. Three
matched multithreaded TCG runs found +1.4% yield throughput but -2.9% and
-1.5% local IPC throughput at 8 and 256 bytes. The rates drifted between
runs; TCG does not reproduce the physical ARM64 cache-coherence cost that
motivated CLH on x86. The tested exchange is reviewable as an archival patch,
with the full measurements in [the ARM64 record](archive/M9.5_arm64_clh_tcg.md).
The shipped ARM64 BKL remains ticket until physical A-class data supports a
change.

The two-core LX6 `esp32-wroom-benchsmp` ticket images linked, and
`lx6_atomctl`, `lx6_park_mask`, `lx6_irq_cells`, the trap-redzone gate, and
the lock-benchmark parser controls passed. A temporary CLH version using an
`S32C1I` tail exchange with an `L32AI` reload on *every* failed store also
linked as bootable bench and selftest images; its trap-redzone and three LX6
static gates passed. The disassembly confirms that the failed exchange loops
back to the load without a call or poll inside that interval. The sources and
matched ticket/CLH images are retained under the ignored `build/m95/` scratch
tree; the shipped LX6 source was restored to ticket.

After the remote bench transfer and flash were approved, both ticket and CLH
selftest images passed on the ESP32-D0WD-V3 at 240 MHz: 145 planned arms, 27
skipped, five partial and zero failed for each. Complete ordinary `bench`
reports also passed with both locks. The 8-byte call/reply rate was 15,816/s
with ticket and 15,964/s with CLH (+0.9%); the 256-byte rate was 14,623/s and
14,749/s (+0.9%). That bench predominantly ran on core 0, so it does not
establish a contended-lock benefit.

The shared `bench_smp` yield workload was extended to ARM64 and LX6 with
200,000 yields per core. It pins one worker to each core and reports the
individual completions. On four-core ARM64 QEMU with the shipped ticket lock,
all four workers completed 200,000 yields. On LX6 silicon, two alternating
captures of each lock completed 200,000 yields on *each* of its two cores:

| LX6 lock | Both runs, aggregate yields/s | Wall time, 400,000 yields |
| --- | ---: | ---: |
| Ticket | 242,352 | 1,650 ms |
| CLH | 245,161 | 1,631 ms |

CLH gained 1.2% on this deliberately contended two-core workload. The extra
tail-exchange code and per-core request state do not earn their maintenance
cost for this small gain, so LX6 retains ticket arbitration. The captures
behind every LX6 figure here are in
[archive/M9.5_lx6_clh.md](archive/M9.5_lx6_clh.md). The silicon
result does not predict ARM64 physical throughput, which remains unmeasured.

The tail exchange is not the same primitive on all three backends. On LX6,
GCC's generic exchange emits an `S32C1I` retry loop that carries a failed
return value into its next attempt; the ticket code deliberately reloads
memory because the Xtensa ISA permits a different failed return value. Both
ARM64 and LX6 therefore need backend-specific exchange code even when the
CLH request algorithm is common. The x86 gain alone is not evidence for
changing either backend. All backends still provide the same one-BKL ownership
and doorbell contract through `arch_kernel_lock` and `arch_kernel_unlock`.

## Corrections to the root exploration sketch

The root exploration sketch is useful as a candidate list. Its per-core ready
queues are already in M9.2; M9.4 added the cross-core work rings. The sketch's
atomic-flag object lock is not FIFO, so an unlucky deadline caller can be
overtaken without bound. A waiter also must service doorbells, and release must
restore the caller's *prior* interrupt state rather than unconditionally enable
interrupts. A per-object design would need a fair primitive and an explicit
order for operations touching several objects. AMP and one-core images already
compile out the BKL via `KICKOS_KERNEL_CORES == 1`; they need no new lock
configuration to obtain that property. The proposed `CONFIG_SMP` and
`CONFIG_BKL` switches are not current KickOS configuration names. The
generation and IPC-ring proposals
need the lifetime and synchronous-call contracts described in the table above.

## M9.5 decision and reopen criteria

CLH improves arbitration but does not break the BKL. The
[whole-transaction feasibility audit](design-m9.5-ipc-lock-feasibility.md)
traced CALL through reply-cap mint, deadline expiry, close/teardown and frame
publication. It found that an endpoint-only or per-core-only lock is unsound,
and that a safe single protocol requires a new pin, lock order, timed-park and
switch handoff design across several kernel subsystems. There is no small
full-transaction candidate to benchmark yet. A read-side scheme should be
tested only against a real read-heavy kernel workload; the measured IPC and
existing syscall inventory do not supply one. M9.5 therefore keeps the single
BKL and the existing per-core ready-queue and work-ring ownership. The
[Book chapter](book/one-lock-many-ready-queues.md) explains why these are
compatible designs.

Reopen a BKL break for a measured workload and a complete, bounded exclusion
and lifetime protocol that does not require a second hot path. Reconsider CLH
on a non-x86 backend only with physical multicore measurements from that
backend; the ARM64 QEMU run proves function, not physical cache-coherence cost.
