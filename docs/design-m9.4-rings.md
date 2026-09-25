<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# The per-pair rings, then the local scheduler leaves the lock: the M9.4 design

> **Status: STAGE 1 LANDED, STAGE 2 REFUSED.** M9.4, recorded 2026-09-24, revised the same day
> against an adversarial review and two maintainer rulings. Stage 1 (the rings under the one lock)
> has landed, inside the regression budget section G sets. Stage 2 (the switch half of a pass
> without the lock) was refused on 2026-09-24 by the numeric stop condition in section G, and a
> refusal is a successful outcome of this milestone. The contract this rests on is `roadmap.md`
> `### M9`, `docs/design-multicore.md` N2, N3, N4, N6f, N9 and section 8, `docs/design-m9-lock-bound.md`
> and `docs/design-m9-entry-envelope.md`; nothing here reopens a ruling in them, and
> `ready-never-waits-while-a-core-below-idles` (`docs/reference/invariants.md:596`) stands as
> recorded in both stages.

## Verdict (2026-09-25)

**Stage 1 landed.** The rings, `HANDOFF`, `RESEAT`, the drop-ask and level cells and the switch to
per-core ready structures are all in the tree, under the one lock, as section E describes.

**Stage 2 was REFUSED by R0 on 2026-09-24**, on `esp32-wroom-benchsmp` at two cores (source:
`2026-09-24-m94-silicon.md`):

- ceiling(W3) 0.010% and ceiling(W2) 0.006%, both far under the 5% gain R0 requires;
- non-parking switches: W3 0.030% (3 of 10005) and W2 0.015% (3 of 20009) of all switches;
- the two four-core emulators read about 0.1% as counts only (R4), which does not gate by itself.

This is the milestone's stop condition firing, a successful outcome per `roadmap.md`'s M9 section.

**D6, stage 1 against master, on silicon.** Numbers below are taken from the report files; where a
number in this brief's own phrasing differs from the report, the report's number is used and the
difference is noted.

- *At S1.4 (stage 1 without the S1.5 instrument), against master* (source: `2026-09-24-m94-s1-bisect.md`,
  "Clean stage-1 verdict: S1.4 against A"): call/reply 8 B +4.30%, call/reply 256 B +3.98% (not a
  single "+4.30%" figure: the two message sizes move by different amounts), ping-pong ns/sw +4.30%,
  switch p50/p99 +3 buckets, e2e-local p50 +1 bucket, and all five BREACH the 1%/one-bucket limit.
  lock-hold p50/p99 do not move: PASS.
- *After the perf commits* (source: `2026-09-24-m94-perf.md`, the addendum table, which supersedes
  the pre-addendum "Final" table for the e2e row): call/reply 8 B -1.02%, call/reply 256 B -0.95%,
  ping-pong ns/sw -3.10%, switch p50/p99 and lock-hold p50/p99 each one bucket better, lock-wait
  unmoved, e2e-local p50 and p99 flat at n=1000 (the sweep now settles before its first raise and
  sizes the local row for a p99): all PASS. The n=100 e2e-local max is not a D6 row (the addendum's
  own ruling); a max is a sample, not a bucketed percentile.

**The budget's reading rule** (section G): 1 percent on the exact means (call/reply, ping-pong
ns/sw), one bucket on every percentile and on the longest masked window, and a max is not a row.

**Superseded where the code diverged from this record**, cited by commit subject:

- *Section D's publish rule.* "Publish a core's level only when its top moves, and from its start":
  `publish_top` (`kernel/sched/policy_fifo_rr.cc`) stores the level cell only on a bitmap edit
  that changes the TOP bit, not on every bitmap change, and it stores nothing until the core has
  seated its first thread (`current[c] != nullptr`). Section D below is design prose, not the
  landed shape.
- *The drop-answer site.* "Answer a drop ask in the holder's dispatch rather than in every pass":
  `push_take` runs once, from `kickos_kernel_core_resched` (the doorbell dispatch), not from every
  ordinary pass; the comment there states it plainly: "the dispatch that raise enters is where it
  is answered, and no ordinary pass reads the drop cells."
- *Section B storage.* `SchedOut` gained a `uint32_t raise` bitmap (which cores this producer owes
  a flush to) and `SchedIn` a `uint8_t cursor` (the drain's round-robin start), neither of which
  section B's struct sketch below lists. `sched_flush_owed` reads and clears `raise` and publishes
  `head` only for the cores named in it: a span that staged or drop-asked nothing publishes
  nothing.
- *The exact read.* "Pass over a placement candidate whose published level is already too high":
  `policy_place` (`kernel/sched/policy_fifo_rr.cc`) prunes a candidate core by its level CELL before
  it reads any ring index (`cell < 0 or cell >= best_lvl` continues first), where section D below
  reads the cell and the ring together as one `effective()` expression.
- *The RESEAT "no forward is needed" note (below, and `sched.cc`'s comment at the same site).* This
  is being fixed to a forward: a `RESEAT` entry whose thread has moved to a peer (`HANDED` elsewhere
  or linked on another core) is re-staged toward that thread's current `queue_core` rather than
  dropped, so a second concurrent re-seat request is not lost. Written here as the forward, since
  the code fix lands in this same milestone.

**`SCHED_DRAIN_BUDGET` (8), measured.** The `KICKOS_BENCH_SCHED` capture with the drain counters on
(`.session/logs/m94perf-C5on-esp32-wroom-bench.log`) prints `sched-drain` and `sched-drain-n` for
every workload. The entries-per-dispatch maximum (`sched-drain-n`, the `max` field) never exceeds 2
in that capture, and `drain-over=0` on every `sched-rings` line: the budget of 8 was never exceeded
by any workload measured, so its retrigger path is unwitnessed by these captures, not merely
unexercised.

**Stage-2-only note, for the record.** On lx6 the lock-free head edge stage 2 would need rests on
the compiler's `memw` lowering, which `klock_lx6.cc` declines to use for the lock itself (it uses
`S32C1I` instead, per section C's ruling). Since stage 2 is refused, this is recorded and not
resolved.

**The rv64 map-unwind re-read** (`TODO.md`'s tracked item): closed through stage 1. Per-core ready
queues serialise the map edit and the install under the one lock exactly as before, so the window
stays closed; the three windows section F opens for stage 2 open only if the lock ever leaves,
which R0's refusal means it has not.

## What exists at the M9.2 merge

Under one ticket lock every core has its own ready structure, and a thread's holder or waker
writes a PEER's structure directly:

| cross-core write today | site |
| --- | --- |
| a wake or admission published to a home that is not the waker's core | `kernel/sched/sched.cc:42-47` (`publish_ready`), called from `:439`, `:620` |
| the push: a READY thread moved off its holder onto a peer | `sched.cc:79-83` (`place`), `:143-144` (`push_take`) |
| a peer-homed READY thread re-seated by a re-mask or a priority change | `sched.cc:479` via `place`, `:675-677` (`set_prio`) |
| a peer's seated priority raised | `sched.cc:699-702` |
| the drop ask, a bit OR-ed into the holder's word | `sched.cc:117` into `Kernel::push_asked` (`kernel/include/kickos/instance.h:61`) |

and reads peer state that only the lock makes sound: `policy_fifo_rr.cc:222-225` (`started`, a
peer's `current`), `:229-241` and `:260-272` (a peer's bitmap and list heads), `:280-308`
(`push_candidate` over the HOLDER's structure, called by the asker at `sched.cc:112`),
`sched.cc:489-506` and `kernel/thread/park.cc:181-192` (a scan of every core's `current`).

Two cross-core cells are already the right shape and are kept: the reschedule ask
(`kernel/sync/klock.cc:45-48`, one sequence per ordered pair, one writer each, coalescing, no full
case) and the doorbell's request/answer rows (`arch/include/kickos/arch/doorbell_protocol.h:41-44`).

## A. The kinds

Two ring kinds and three cells. An entry is a `uint16_t`: a thread slot index and a kind bit. An
idle TCB is outside the pool, pinned to its own core (`sched.cc:455`), and never enters a ring.

| kind | producer | consumer | payload | refuses on full | depth rule |
| --- | --- | --- | --- | --- | --- |
| `HANDOFF` ring kind | the thread's waker (a PARKED thread readied to a home that is not the waker's core), its holder (every push: displaced, declined, answered drop), its creator (admission to a home that is not the creator's core) | the new home | slot index | never | one entry per thread in the machine, below |
| `RESEAT` ring kind | a core that changed a READY, RUNNING or HANDED thread's effective priority, mask or slay claim without being the core it is linked on (the PI recompute, `set_prio`, `set_affinity`, a slay) | `queue_core` as the producer read it | slot index | never | one entry per slot in the machine, by `reseat_owed` |
| drop ask CELL | the core whose level fell | the holder asked | none: a sequence per ordered pair | no full case | none: coalescing, like `klock.cc:45-48` |
| reschedule ask CELL | unchanged, `klock_resched_ask` | unchanged | none | no full case | none |
| level CELL | the core itself, at every change of its bitmap and at every pass end | any placer, under the lock | level + 1, 0 = not started | no full case | none: one byte, one writer |

**ONE RING PER ORDERED PAIR CARRIES BOTH KINDS**, because both are consumed by the same dispatch in
the same order, and a second ring per pair would double the index lines for nothing.

**THE DEPTH CANNOT FILL, and the argument is per thread, not per pair.**

- `HANDOFF`: a thread is `HANDED` from the moment a producer stages it to the moment its consumer
  links it, and no path hands a `HANDED` thread: a wake refuses anything but `BLOCKED`
  (`sched.cc:609`), and a push takes only a thread linked on the pusher's own structure. So a thread
  is named by at most one `HANDOFF` entry in the machine.
- `RESEAT`: a producer publishes one only after reading `reseat_owed` as 0 and setting it, and the
  consumer RETIRES THE ENTRY (stores its `tail` past it) BEFORE it clears the byte, so a second
  entry cannot be staged while the first is outstanding. The byte belongs to the pool SLOT and not
  to the thread: `thread_create`'s `kmemset` (`kernel/thread/thread.cc:144`) preserves it, so an
  entry still in flight for a slot's previous occupant keeps the new occupant from staging a second
  one. That entry is then applied to or dropped for the new occupant, which is harmless: it only
  says "re-read".

So at most `2 * KICKOS_THREAD_SLOTS` entries are outstanding on a pair, staged ones included, and

    SCHED_RING_DEPTH = the least power of two >= 2 * KICKOS_THREAD_SLOTS

makes a full ring unreachable by construction, which is what "a wake is never refused on full"
requires (`roadmap.md` M9, the inbox paragraph). `KICKOS_THREAD_SLOTS` is `KICKOS_MAX_THREADS + 1`
(`kernel/include/kickos/config/system.h:42`). The producer never reads the consumer's tail on the
release path; a `KICKOS_DEBUG` build asserts `staged - tail < SCHED_RING_DEPTH` at every stage.

**`RESEAT` CARRIES NO VALUE.** Two requesters on two cores publish on two rings, and the consumer
drains rings in its own order, so a value in the entry lets an older request win. The requested
values live in the TCB (`prio`, `affinity`, `cancel_kind`), written under the global lock by
whoever changes them; the entry says only "re-read T", and any drain order converges on the last
write. **`reseat_owed` IS NOT A SECOND TRUTH**: it answers "is an entry naming this slot in some
ring", which nothing else records, and it is what makes the depth argument a proof.

**WHICH OF TODAY'S CELLS BECOME WHAT.**

- `Kernel::push_asked` (`instance.h:61`) becomes the drop ask cell. Today it is a word per holder
  OR-ed by every asker, a cross-core read-modify-write the one lock alone makes sound; as a sequence
  per ordered pair it has one writer per cell and needs no RMW (N9). It stays a cell because a drop
  ask names no thread (the holder re-reads the asker's level, `sched.cc:124-149`) and coalescing is
  its semantics.
- `klock_resched_ask` stays as it is: it already is the per-pair single-writer idempotent cell N4
  wants, and it becomes the raise that carries every ring publication and drop ask of a span.
- `Kernel::seated_prio` (`instance.h:62`) stays a per-core field and becomes CORE-LOCAL: its only
  cross-core writer, `set_prio`'s raise of a peer's running thread (`sched.cc:699-702`), becomes a
  `RESEAT` applied by that peer.
- `Kernel::current` stays per core and becomes core-local: every peer read of it is replaced by the
  level cell (not started = 0) or by a `RESEAT` sent to the thread's `queue_core`.
- The level cell is new: the "published per-core level cell (single writer)" `STATE.md` M9.2 names.

**CROSS-CORE PRIORITY INHERITANCE LANDS IN M9.4, IN STAGE 1, AS `RESEAT`.** `set_prio` re-seats a
peer's ready structure (`sched.cc:675-677`), a cross-home write, so stage 1 cannot claim "no core
writes another core's scheduler state" while it exists, and the roadmap's reason for landing the
rings under the lock first applies to it exactly as to the push. The recompute is unchanged and runs
under the global lock; its result is a published request; the delivery delay is one flush, one raise
and one dispatch, priced in section G. Helping by migration stays refused. It does not cap the
recompute over an unbounded donor set, which stays on the roadmap's list of unbounded paths.

## B. Storage

**SUPERSEDED: the landed `SchedOut` also carries a `uint32_t raise` bitmap, and the landed
`SchedIn` a `uint8_t cursor`; the flush publishes `head` only for the cores named in `raise` and
clears it, so a span that staged or drop-asked nothing publishes nothing.** See the Verdict.

    struct alignas(64) SchedOut           // core p's row, written by p alone
    {
        Atomic<uint32_t> head[N];         // entries p has PUBLISHED toward t
        uint32_t staged[N];               // entries p has written toward t, read by placers under the lock
        Atomic<uint32_t> drop_asked[N];   // drops p has asked of holder t
        Atomic<uint8_t> level;            // p's level + 1; 0 = not started
    };
    struct alignas(64) SchedIn            // core c's row, written by c alone
    {
        Atomic<uint32_t> tail[N];         // how far c has drained p's ring toward c
        Atomic<uint32_t> drop_took[N];    // how far c has answered p's drop asks
    };
    SchedOut sched_out[N];
    SchedIn sched_in[N];
    uint16_t sched_slot[N][N][SCHED_RING_DEPTH];   // [producer][consumer][i], written by producer

The row shape is `klock.cc:37-48`'s: one row per writer, a line each, so no two cores write one line.
The line is 64 as `klock.cc:19` uses; on lx6, which has no cache over kernel SRAM
(`arch/xtensa/lx6/include/kickos/arch/doorbell_part.h:18-20`), that alignment buys nothing and costs
the padding below, stated rather than folded into a per-arch constant nothing else needs.

Cost at the default `KICKOS_MAX_THREADS` 16 (17 slots, depth 64, 2-byte entries, `[N][N]` slots with
the unused diagonal kept for a plain index):

| cores | slots | rows | total | used slot bytes |
| --- | --- | --- | --- | --- |
| 2 | 512 B | 256 B | 768 B | 256 B |
| 4 | 2048 B | 512 B | 2560 B | 1536 B |
| 8 (scenario) | 8192 B | 1536 B | 9728 B | 7168 B |

`esp32-wroom-benchsmp` states `KICKOS_MAX_THREADS=8` (`boards/esp32-wroom/configs/benchsmp/defconfig`),
so 9 slots, depth 32 and 512 B in all. At the Kconfig ceiling of 64 threads the depth is 256 and eight
cores spend 32 KiB on slots, the quadratic cost the roadmap names. It removes `push_asked`, `4 * N`
bytes.

**WHERE IT LIVES.** In `Kernel`, at the end of the struct, under `#if KICKOS_KERNEL_CORES > 1`, since
it is scheduler state and `instance-scoped-state-no-arch-crossing` puts every such cell in the
instance. EVERY MEMBER IS ZERO-INITIALISED, which is why `level` is biased by one: a non-zero
initialiser anywhere in `Kernel` moves the whole constinit instance out of `.bss`
(`docs/design-multicore.md` section 8). Two TCB bytes are added, `rq_prio` (the list key the policy
seated, section E, S1.3) and `reseat_owed`, in the three padding bytes after `Thread::queue_core`
(`kernel/include/kickos/thread.h:182`, followed by the 4-aligned `quantum_ns`): the prediction is zero
TCB growth on every 32-bit target, and the `thread_scalar_bytes` assert is the witness.

## C. Publication, drain and ordering

**PUBLICATION IS DEFERRED TO THE SWAP WHENEVER A SWITCH HAS BEEN DECIDED, AND OTHERWISE HAPPENS AT
THE END OF THE SPAN.** A producer writes the slot and advances `staged`; `head` is stored by
`sched_flush`, and the flush bumps the reschedule ask cell of every target it published to and every
holder it drop-asked, raising each once. The flush is keyed on a per-core flag,
`flush_owed_to_swap`, set by the pass that decides to switch, before anything that could release a
lock runs, and cleared by `kickos_switch_unlock`:

- `kickos_switch_unlock` (`klock.cc:138-149`) ALWAYS flushes, after the swap has parked the outgoing
  frame, which is the one point that knows it (N2; `arch/include/kickos/arch/arch.h:811-815`);
- every other span end (`klock_leave`'s depth-zero arm, `klock.cc:79-89`; `klock_drop` from
  `sched::start`, `sched.cc:545`; and in stage 2 the end of a `CoreLock`) flushes ONLY when the flag
  is clear.

The flag is what makes the rule independent of which lock is held. On a deferred-switch backend a
switch booked in a handler (`arch/arm64/armv8a/arch_armv8a.cc:408-418`,
`arch/riscv/rv64imac/arch_rv64imac.cc:529-535`) parks its frame only at the exception exit
(`arch/arm64/armv8a/switch.S:278-291`, `arch/riscv/rv64imac/switch.S:449-454`), so any flush before it
would let a target seat a thread whose registers are still live. Under one lock in stage 1 the lock's
own `owed` already has this shape (`klock_detach` sets it, `klock_leave` does not release while it is
set); the flag states it for a span that holds no global lock. Nothing else needs an on-CPU record.

**THE RAISE STOPS GOING THROUGH `arch_ipi_send`.** `klock_resched_ask` (`klock.cc:153-159`) calls
`arch_ipi_send`, which bumps the rendezvous request row and reaches `kickos_doorbell_poll`
(`arch/common/doorbell_protocol.cc:235-258`). A reschedule owes no rendezvous answer, and the vector
already tells the two apart by the cell (`arch/arm64/common/arch_arm64_gicv3.cc:785-794`,
`arch_rv64imac.cc:876-890`, `arch/xtensa/lx6/arch_xtensa.cc:614-636`). One seam member replaces
`arch_ipi_resched_self`: `arch_ipi_raise(uint32_t cores)`, the part's bare lowering
(`kickos_doorbell_raise`, `klock_armv8a.cc:201-204`, `klock_rv64imac.cc:218`, `klock_lx6.cc:213`), self
bit included. A poll that finds no request owed returns without clearing the part's pending state
(`klock_armv8a.cc:182-185`, `klock_lx6.cc:187-190`), so a raise it cannot see stays pending and is
taken at the unmask. The flush raises every target with new entries unconditionally, an unstarted
one included (its park loop answers the raise and its start drains), so no raise decision reads a
peer cell and no store-then-load pair arises on this path.

**WHY: THE lx6 RED ZONE, PRICED.** Measured on a freshly wiped scratch tree for `esp32-wroom-smp` at
this merge: PREEMPT 576 against `KICKOS_LX6_TRAP_DEPTH` 608. The winner is `kickos_lx6_dispatch_l1`
through `irq_event_isr -> notify_raise -> wake -> resched_after_wake -> pick_and_seat` (304 bytes to
there), then `klock_attach -> arch_kernel_lock -> kickos_doorbell_poll -> kickos_lx6_doorbell_service
-> kickos_irq_route_service -> arch_irq_mask -> kickos_lx6_hw_mask -> phys_int_disable` (272 more). The
header still describes a 608 measured through `klock_resched_ask -> arch_ipi_send`
(`arch/xtensa/lx6/include/kickos/arch/lx6_trap_stack.h:31-38`) and calls 608 the measurement (`:47`);
both are stale on master. The flush sits under `arch_switch -> xtensa_switch -> kickos_switch_unlock`,
whose descent the gate takes from a hand record (`tests/static/trap_redzone_roots.txt:488`,
`xtensa_switch` 128). With the raise through `arch_ipi_send` that route is 304 + 32 (`arch_switch`,
off the printed chain, taken at the windowed minimum) + 32 (`xtensa_switch`) + 32
(`kickos_switch_unlock`) + 32 (flush) + 32 (`klock_resched_ask`) + 32 (`arch_ipi_send`) + the 208-byte
`kickos_doorbell_poll`-to-`phys_int_disable` tail, about 704, which overruns 608 by about 96. With
`arch_ipi_raise` the hand record becomes 192 (flush called from `kickos_switch_unlock`) to 224 (flush
called from inside `klock_drop`), and the route 528 to 560, under the 576 winner. **The exit taken is
the first one the header names, shortening the chain; the fleet-wide `KICKOS_MIN_STACK_SIZE` is not
moved.** The record is re-derived by hand from the linked image in S1.2, since no gate re-derives it.

**THE DRAIN LIVES IN THE DISPATCH THAT ENTERS THE SCHEDULER, NOT IN THE SERVICE BODY, and that is
N3 applied, not reopened.** N3 puts the route drain in the service body because an initiator HOLDING
THE LOCK WAITS for it, and only the acquire poll answers a core spinning for that lock. No ring kind
has a waiter: a producer publishes and returns ("a core never waits on a peer's scheduler lock while
holding local state; it publishes"). N3 names the other home itself: `klock_resched_ask`'s consumer
"is drained by the dispatch ENTERING THE SCHEDULER". The service body is wrong here twice over: it
runs from the poll inside `arch_kernel_lock` and `arch_ipi_send`, which a pass reaches mid-walk
(`policy_declined` may move nothing but its answer, `kernel/include/kickos/sched.h:124-131`), so a
drain there would mutate the structure under a walk; and it is reachable from every chain that can
spin on the lock, so it would charge the red zone on every one. So `sched_drain(me)` runs, always
under the global lock:

- in `kickos_kernel_core_resched` (`sched.cc:929-938`), inside its `IrqLock`, before the pass, reached
  only after `kickos_kernel_core_resched_take` consumed an ask;
- in `sched::start` (`sched.cc:513-548`), before its pick, draining everything: bounded by
  `(N - 1) * SCHED_RING_DEPTH` once per core per boot, which is not a deadline path.

An ordinary pass does not drain, so a pinned same-core handoff reads no ring; every publication
carries its own raise, so nothing waits past that raise for a drain.

**BUDGET AND RETRIGGER.** One dispatch applies at most `SCHED_DRAIN_BUDGET` = 8 entries across all
producers, round-robin from a per-core cursor, FIFO within a ring. A remainder stays published and the
dispatch calls `klock_resched_self` (`klock.cc:165-168`), whose raise the span's release carries. Each
entry costs one link and one `place`, so the dispatch's masked work is bounded by a build constant
and sits inside `C` of `D + (N-1)*(C+H+S)`. The value 8 is set, not measured; `BD_SCHED_DRAIN`
(section E, S1.5) says whether it is right.

**APPLYING AN ENTRY** (at consumer `me`, entry naming `T`). Every entry is RETIRED FIRST: its `tail`
is stored past it before any byte of `T` is written.

- `HANDOFF`: `T` is `HANDED` with `queue_core == me` (asserted). If `me` is no longer in `T`'s mask
  (a re-mask drained ahead of it), `T` is not linked: it is handed on to `sched_home_for(T, me)`.
  Otherwise store `READY` and `rq_prio = prio`, link, and `place(T, me)`, since a thread linked below
  what this core runs may belong on a lower peer and no other pass will name it.
- `RESEAT`, `T` linked here (READY or RUNNING with `queue_core == me`): clear `reseat_owed`, re-read
  `prio`, `affinity` and `cancel_kind`, re-seat at the new priority, apply `seated_prio`'s raise rule
  (`sched.cc:697-702`, moved here), place `T` if READY, and let the pass that follows switch a running
  `T` away if its mask or claim now refuses it (`available_to`, `policy_fifo_rr.cc:92-107`).
- `RESEAT`, `T` `BLOCKED`, `EXITED` or `INACTIVE`: clear `reseat_owed` and drop; the request is
  stale, and whatever wakes or reuses `T` next re-reads the fields.
- `RESEAT`, `T` `HANDED` toward a peer or linked on one: the entry is re-staged toward that peer's
  `queue_core`, and `reseat_owed` is kept set rather than cleared. This is the forward: a drop here
  would lose the request outright if a second one lands before the first is read (see the Verdict).

**ORDERING.** Every cell is a load and a store by its one writer; nothing above the seam is a
read-modify-write (N9, `tests/static/check_atomic_rmw.sh`). In stage 1 everything is ordered by the
ticket (`klock_armv8a.cc:298-307` STLR, the waiter's acquire; the rv64 and lx6 counterparts), and the
cells use acquire/release loads and stores anyway so that stage 2 changes no access kind. Stage 2
keeps every drain, every level read and write and every placement decision under the lock (section
F), so its only lock-free cross-core edges are the flush's `head` release store in
`kickos_switch_unlock`, paired with the consumer's acquire load under the lock, and the residency
record of S2.1, the one store-then-load pair (section F).

**lx6'S FULL BARRIER, RULED.** Xtensa ISA summary 4.3.13.5 (p.122): the `S32C1I` atomic pair orders
every ordinary load and store on both sides, as specified, on any word, a core-private one included.
So lx6's `arch_ipi_fence` is `S32C1I` on a per-core word, below the seam where an RMW is permitted,
and lx6 runs stage 2 like the other two backends. It is missing today (declared at
`arch/include/kickos/arch/arch.h:127`, called only by armv8a and the AMP window); it is added by the
first commit that needs it, and in the form of stage 2 below no lx6 path does, the residency pair
being MMU-only.

## D. The level cell, and the exact placement read

**SUPERSEDED: the landed publish rule is narrower than the rule below.** `publish_top`
(`kernel/sched/policy_fifo_rr.cc`) stores the cell only on a bitmap edit that moves the TOP bit, not
on every bitmap edit, and only once the core has seated its first thread. The design argument below
(a bitmap change with no pass leaves the cell stale) is what motivates publishing on every
TOP-moving edit rather than only at the pass end; it does not require publishing on every edit. See
the Verdict.

**THE CELL IS PUBLISHED AT EVERY CHANGE OF THE BITMAP, and at every pass end.** `level` is
`top_prio(ready_bitmap[me]) + 1`, stored by `rq_push_back`, `rq_remove`
(`policy_fifo_rr.cc:41-57`) and a re-seat, still by the one core that owns the bitmap, and again at
`seat_level` (`sched.cc:173-183`) and at start. A pass reads peers' levels in `place` and
`place_declined` (`policy_fifo_rr.cc:266`, via `sched.cc:167`, `:250`, `:378`) BEFORE its pass-end store
(`sched.cc:252`, `:386`), and a bitmap can change in a span that ends with no pass
(`wake_no_resched` followed by `place_ready`), so publishing at the pass end alone leaves the cell
stale for exactly the reads that matter.

**THE PLACEMENT READ IS EXACT UNDER THE LOCK, RULED BY THE MAINTAINER.** A core's level as the
invariant means it ("the priority its next pass seats if nothing arrives") is its bitmap AND what is
already on its way to it. So a placer reads, for each candidate core `c`:

**SUPERSEDED, cheaply: the landed `policy_place` prunes a candidate by its level cell alone before
reading `effective(c)`'s ring half at all** ("Pass over a placement candidate whose published level
is already too high"): a cell already at or above the best candidate so far cannot beat it (what is
on its way only raises a core), so the ring-index walk below runs only for a candidate whose cell
clears that bar. The formula is still exact; the landed code just orders the cheap test first.

    effective(c) = max(level(c),
                       prio of T for every entry toward c in [tail, staged) of every producer's
                       ring, the placer's own staged entries included, where T->queue_core == c
                       and T is READY, RUNNING or HANDED)

`N - 1` index comparisons per candidate core, plus one TCB byte per outstanding entry, which is
zero in the common case. The TCB and not the entry is read because a later request can move `prio`
while its entry is in flight, and under the lock the field is exact. **A single "highest staged
level" byte per producer is not exact, which is why the read is over entries**: a consumer that
drains part of a ring cannot lower a byte in the producer's row, and a byte left high makes a placer
skip a core that sits below, which is a violation, not a delay. With this read two placements toward
one idle core from one span, or from two cores' spans, never pile up: the second sees the first. The
invariant holds at every decision exactly as on master, `two_threads_declined_in_one_pass_do_not_pile_onto_one_idle_core`
(`tests/unit/placement/placement.cc:317`) stands, and the drop and decline halves keep the meaning
section 8 of the multicore contract gives them.

| today's read | site | after |
| --- | --- | --- |
| `started(c)`: a peer's `current` | `policy_fifo_rr.cc:222-225`, `sched.cc:108` | `level != 0` |
| `level(c, t)` for `c` not `t`'s holder | `policy_fifo_rr.cc:266` | `effective(c)` |
| `level(home, t)`: the holder's own bitmap with `t` excluded | `:252`, `:299` | unchanged; `place` is called only by `t`'s holder (asserted) |
| `push_candidate(h, me)` over a peer's structure, by the asker | `sched.cc:112` | the asker drop-asks every started `h` whose `effective(h)` exceeds its new level; `h` runs `push_candidate(h, asker)` over its own structure (`sched.cc:138`) |
| `level(target)` inside `push_candidate` | `policy_fifo_rr.cc:282` | `effective(target)` |
| `seated_prio[me]` | `sched.cc:160`, `:177-181`, `:380` | unchanged, own core |
| `seated_prio[core]`, `current[core]` of a peer | `sched.cc:692-706` | `RESEAT` to `queue_core` |
| every core's `current`, to find a running thread | `sched.cc:489-506`, `park.cc:183-190` | `RESEAT` to `queue_core`: a running thread's home is the core running it, because a core picks only from its own structure |
| `idle[c]` of every core | `sched.cc:868-878` | unchanged: written once before that core starts |

After this table no pass reads a peer's `ready`, `ready_bitmap`, `current`, `seated_prio` or
`idle`-after-start. The witness is a host arm that poisons every peer's structure and bitmap in the
fixture and asserts the pass's decisions unchanged. The drop filter "`effective(h)` exceeds my new
level" is necessary and not sufficient, `push_candidate`'s mask test being the holder's, so a holder
with nothing placeable on the asker gets a spurious ask; it is counted (S1.5).

**WHAT STAGE 2 MUST DO INSTEAD, STATED PLAINLY: THERE IS NO CHEAP EXACT LOCK-FREE ANSWER.** Without
the lock, two cores can each decide a placement toward one core, and a third can change its level,
concurrently. With single-writer cells and no RMW (N9), a full barrier on both sides guarantees only
that ONE of two concurrent deciders sees the other: enough for liveness (the drop and decline
re-establish the invariant a raise later) and not for exactness, which is what `invariants.md:596`
records. The candidates that would make it exact were worked and fail: per-producer reservations
must be retracted when two placers tie, and a third placer that acted on a reservation later
retracted has then left a thread waiting beside a core that is below, which needs a re-notification
protocol, that is, consensus among the placers; a shared reservation word is an RMW above the seam.
**So stage 2 keeps every level write, every level read and every placement decision under the global
lock, and only the switch half of a pass leaves it (section F).** That is the cost stage 2 pays, it is
what section G measures, and the invariant is not weakened in either stage.

## E. Stage 1: the rings under the lock

Every commit keeps the one lock, is bisectable, and compiles to nothing at one kernel core. The
common witness set: `sim` with the GTest layer (the `kickos_kseam_smp` suites: `placement`,
`migrateask`, `reschedowed`, `slaypeer`, `priodonor`, `schedwake`, `fusedwake`, `exitquiesce`); the
selftest on `qemu-arm64-smp` and `qemu-riscv64-smp` (four cores), `qemu-arm64-smpiso`,
`qemu-arm64-gicv3`, and the two-core `qemu-arm64-benchsmp2` and `qemu-riscv64-benchsmp2`;
`esp32-wroom-smp` and `esp32-wroom-benchsmp` on silicon through `tools/bench/` only; the trap red-zone
gate on `esp32-wroom-smp`, `qemu-arm64-smp` and `qemu-riscv64-smp` from a freshly wiped per-agent
scratch tree; `check_smp_doorbell.sh`; and byte identity of the one-core controls `qemu-arm64`,
`qemu-riscv64` and `esp32-wroom` against the parent commit, built from one path with the build stamps
handled.

**S1.1: the reschedule raise loses its rendezvous.** `arch.h` replaces `arch_ipi_resched_self` with
`arch_ipi_raise(uint32_t cores)`; the three backends define it as their existing lowering;
`klock_resched_ask` owes the cells and calls it; `klock_leave`, `kickos_switch_unlock` and the three
polls call it for their own bit. `docs/reference/porting.md` "The cross-core doorbell" gains the
member. `lx6_trap_stack.h:31-38` and `:47` are rewritten to the chain the gate measures, and
`KICKOS_LX6_TRAP_DEPTH` is set to the measurement, the header's own rule. Witness: the doorbell
selfcheck at boot, `check_smp_doorbell.sh`, `reschedowed`, `t_resched_reaches_pinned_caller`
(`user/apps/common/selftest/selftest_smp.cc:1398`), and the lx6 figure, whose ask route falls and
whose `klock_attach` winner holds. Lands first because S1.2 puts the flush under
`kickos_switch_unlock`.

**S1.2: the ring, `HANDOFF`, the flush and the drain.** `instance.h` gains `SchedOut`, `SchedIn` and
`sched_slot`; `thread.h` gains `ThreadState::HANDED`; `sched.cc` gains `ring_stage`, `sched_flush` and
`sched_drain`; `publish_ready` (`sched.cc:42-47`) links when `home == me` and otherwise stores
`queue_core` and `HANDED` and stages; `klock.cc` calls `sched_flush` from its release arms under
section C's rule. Every arm that assumed a published thread is linked tests `HANDED` first and, on
it, does nothing but what the drain will read: `pick_and_seat` places `woken` only if it is not
`HANDED` (`sched.cc:376-379`); `place_and_ask` and `sched::place_ready` return on `HANDED`
(`sched.cc:95-98`, `:646-650`), which covers `resched_after_wake`'s decline (`:638`) and the IPC
deferred-wake collector (`kernel/syscall/syscall_ipc.cc:68`) that every IPC wake reaches; `sched::add`
hands off without placing when its home is a peer (`:439-443`); `place` asserts its thread is linked.
The `HANDOFF` apply includes the mask check of section C. `place` returns only this core's bit; the
flush raises every target. The `xtensa_switch` hand record (`trap_redzone_roots.txt:488`) is
re-derived from the linked image. Witness, new host arms:
`a_handoff_is_linked_by_the_target_dispatch_and_by_no_other`,
`a_handoff_to_an_unstarted_core_is_linked_at_its_start`,
`a_handoff_whose_mask_no_longer_names_its_target_is_handed_on`,
`every_thread_in_flight_at_once_does_not_fill_one_ring` (all `KICKOS_THREAD_SLOTS` pool threads handed
and reseated toward one core in one span), `a_staged_entry_is_invisible_until_the_span_ends`,
`a_drain_past_its_budget_owes_itself_the_next_dispatch`,
`a_wake_placed_by_the_collector_while_handed_is_left_to_its_target`; the existing `placement` arms
rewritten to run the target's dispatch before asserting where a thread sits (the fixture gains that
step, `tests/unit/kfixture/kseam_test.h`); selftest `t_threads_reach_every_core`, `t_migrate_running`,
`t_slice_preempts_every_core` (`selftest_smp.cc:1243`, `:334`, `:1026`).

**S1.3: `RESEAT`, and the policy keys its lists on `rq_prio`.** `set_prio`, `set_affinity` and
`thread_cancel_kind`'s running-target arm apply directly ONLY when the thread is linked on this core
(READY or RUNNING with `queue_core == me`); a `HANDED` thread, wherever it is headed, gets the field
write alone, plus a `RESEAT` when it is headed elsewhere (`set_prio`'s READY arm, `sched.cc:675-683`,
and `set_affinity`'s, `:477-479`, both gain the `HANDED` test); every other thread gets the field
write and a `RESEAT` under `reseat_owed`. The `current` scans at `sched.cc:489-506` and
`park.cc:181-192` go; `seated_prio` has no peer writer left; `policy_fifo_rr.cc` keys `rq_push_back`,
`rq_remove` and every level comparison on `rq_prio`, so `prio` is the effective priority the
recompute owns and `rq_prio` the one this core seated; `thread_create` preserves `reseat_owed` across
its `kmemset` (`thread.cc:144`). `thread.h:89-92`'s "executes no further unprivileged instruction" is
restated to what master already does (section F, the slay ruling). Witness, new host arms:
`a_boost_of_a_thread_ready_on_a_peer_is_seated_by_that_peers_dispatch`,
`a_reseat_of_a_handed_thread_is_read_at_its_link`,
`a_reseat_reaching_a_core_that_moved_the_thread_is_dropped_and_the_link_reads_the_field`,
`a_reseat_of_a_parked_or_exited_thread_is_dropped`,
`a_second_request_before_the_first_is_retired_publishes_nothing`,
`a_reseat_in_flight_across_a_slot_reuse_does_not_admit_a_second`,
`a_local_request_on_a_thread_handed_to_this_core_writes_only_the_field`; the existing
`lowering_a_peers_running_thread_asks_that_peer_for_the_pass_that_sees_the_drop`
(`placement.cc:421`), `slaypeer`, `priodonor`, `migrateask`; selftest `t_affinity_*`, the PI arms in
`user/apps/common/selftest/main.cc`, the `slaypeer` app.

**S1.4: the level cell, the exact read, and the drop cells.** `push_asked` goes; the cell is
published at every bitmap change (section D); `started`, `level` for a peer, the drop filter and
`push_candidate`'s floor read `effective`; `push_take` answers the drop cells. Witness: every
`placement` arm, `:317`, `:480` and `:531` kept verbatim; new:
`two_placements_from_two_cores_toward_one_idle_core_do_not_pile_up`,
`a_level_changed_in_a_span_with_no_pass_is_seen_by_the_next_placer`,
`a_drop_ask_to_a_holder_with_nothing_to_push_moves_nothing`,
`no_pass_reads_a_peers_ready_structure` (the poisoning arm of section D). This commit carries the
peer-ask re-price (section G).

**S1.5: the instrument**, under its own `KICKOS_BENCH_SCHED` knob beside `KICKOS_BENCH`, off by default so that no other bench figure carries it, before any stage-2 code, so the stage-2 decision has
its inputs from the tree it is judged against: acquisitions per switch by entry reason (syscall entry,
resume re-acquire in `klock_attach`, reschedule dispatch, timer, other); pass cycles by entry reason
and by whether the pass parks, the switch half bracketed apart (what section G's ceiling needs);
`BD_CALL_RT`, a round-trip distribution stamped on the caller's core; `BD_PUSH_E2E` and
`BD_RESEAT_E2E` in nanoseconds, from the drop or the request to the moved or re-seated thread's first
instruction on its core; `BD_SCHED_DRAIN`, cycles and entries per dispatch; counters for handoffs,
reseats, handoffs handed on, spurious drop asks and budget exhaustions. Witness: the phase-table gates
(`tests/integration/check_bench_phase_table.sh`, which reads each row's `n`), byte identity of the
non-bench SMP images against S1.4.

**THE STAGE-1 GATE, BEFORE STAGE 2 STARTS.** Stage 1 lands in every outcome, and the regression budget
guards the latency posture against M9 as a whole (`roadmap.md` M9, the stop-condition paragraph), so
stage 1's own cost is judged against master under section G's R1 and R2 limits on silicon. A breach is
a defect of stage 1, fixed before stage 2 is written, not a refusal of the rings.

**S1.6 and S1.7: the P6 doorbell copy, AMP only, orthogonal to everything above.** Section H
designs the lifetime; S1.6 is the CALL side, which needs no lifetime change, and S1.7 the REPLY side,
which lands (section H, ruled). Witness: `tests/unit/ampwindow`, `qemu-arm64-amp*`, `pizero2350-amp*` on
silicon, M8.11's masked-span sweep for the before and after, and an arm per clause of section H.

## F. Stage 2: the switch half of a pass leaves the lock

**THE SHAPE: DECIDE UNDER THE LOCK, PERFORM WITHOUT IT.** Section D rules out exact lock-free
placement, so the lock keeps the whole decision: the drain, every level write and read, every
placement and push decision and its staging, the pick (`available_to` reads the mask and the claim
there, `policy_fifo_rr.cc:92-124`), the slay redirect of the incoming thread (`sched.cc:260-264`), and
the timer's next deadline (the sleep queue head and the incoming slice, `kernel/time/time.cc:98-108`).
The global lock is then released and the PERFORM half runs under `CoreLock`, the interrupt mask alone:
`switch_book`'s stores (`current[me]`, the two states, `switch_count`, `on_switch_in`), programming the
comparator from the deadline already computed (`time.cc:122-133`), the address-space and MPU install
(`sched.cc:228-229`), `reent_seat`, and `arch_switch` up to `kickos_switch_unlock`, which flushes
(section C).

**WHICH PASSES.** A PARK and an EXIT keep the lock through their switch, exactly as today: a waker can
see `BLOCKED` only by taking the lock, and the lock is released inside the swap (N2), so no waker can
seat a thread whose frame is still live; `EXITED` makes the slot reclaimable, `sched.cc:797-798`. Every
other pass is split: the reschedule dispatch (the drain is its decision half), the slice expiry and
`sched::yield`, and the tail pass of a syscall that readied a thread and does not park. **What this
cannot do is remove an acquisition**: every split pass still takes the lock for its decision, so the
`N + 1` acquisitions per switch M9.1 measured are not what stage 2 buys; what it buys is the switch
half's cycles off every non-parking pass's hold.

**THE LOCAL EXCLUSION IS THE CORE'S OWN INTERRUPT MASK.** After stage 1 no core writes another core's
scheduler state, and after the split no other core reads what the perform half writes, so the only
concurrency the perform half has is this core's own handlers, which the mask excludes. `CoreLock` is
`IrqLock` without `klock_enter`; its end flushes and raises explicitly, only when
`flush_owed_to_swap` is clear. The lock order: the mask is always outermost
(`kernel/include/kickos/irqlock.h:34-37` already takes it before the global lock, as `klock.h`
requires); the global lock is taken only under a mask; a perform half never takes the global lock; no
core waits on a peer's mask or pass. **TWO CORE-LOCAL EXCLUSIONS CANNOT BE HELD TOGETHER BECAUSE NO
PRIMITIVE ACQUIRES ANOTHER CORE'S MASK**: the exclusion is a property of the core executing, not a
word, so there is nothing to order. `klock_exclusion_held` (`klock.cc:93-97`) gains a `CoreLock`-aware
form, a per-core `CoreLock` depth in the klock row under `KICKOS_DEBUG`, because the perform half's
callees assert an exclusion and the global form would fail there.

| state | owner | who else touches it | protection in stage 2 |
| --- | --- | --- | --- |
| `ready[c][*]`, `ready_bitmap[c]`, level cell | core `c` | placers read the cell | `c` writes them only in the decision half, under the lock; the perform half changes no bitmap |
| `current[c]` | `c` | nobody after S1.4 | the perform half, `c`'s mask |
| `idle[c]` | boot core, once, before `c` starts (`sched.cc:448-460`) | read by `c` and `is_idle` | the bring-up seat's acquire (`arch.h:817-819`) |
| `seated_prio[c]` | `c` | nobody after S1.3 | decision half |
| ring slots, `head`, `staged` | producer | placers read `staged` and entries, the consumer drains | under the lock, except `head`'s release store in `kickos_switch_unlock`, which the consumer acquires under the lock |
| `tail`, drop and reschedule cells | one writer each | the other side reads | under the lock; acquire/release as `klock.cc:170-219` |
| READY, RUNNING TCB: `link`, `rq_prio`, `queue_core`, `slice_deadline_ns` | the home | read by requesters under the lock | written in the decision half; `slice_deadline_ns` in the perform half, which only the home reads |
| TCB `state` | the home for READY/RUNNING, the waker for PARKED | a requester under the lock tests it | the perform half flips READY and RUNNING only, never to or from `BLOCKED` or `HANDED`, so no decision a requester makes depends on which of the two it reads; the field becomes an atomic byte so the concurrent read is not a data race |
| HANDED TCB | nobody | the target at the drain | producer's release, consumer's acquire |
| PARKED TCB | the waker, under the lock | as today | the lock, then the ring |
| `prio`, `affinity`, `cancel_kind` | the global-lock holder that changes them | the home reads them in its decision half | the lock |
| `reseat_owed` | set by a requester, cleared by the applier | none | the lock |
| sleep queue, `timer_armed_ns[c]` | the lock; `c` | none | deadline computed under the lock, comparator and cell written in the perform half |
| `Kernel::live`, pools, `next_tid` | the lock | none | unchanged |
| telemetry counters | global | none | not selectable on a shared-kernel preset (`TELEMETRY_RTT` depends on an RTT console); S2.2 adds the configure refusal so that is stated |
| bench accumulators | per core | none | unchanged; the bracket depth rides the frame (`sched.cc:351-355`) |
| deferred-switch cells (`switch_to`, `ctx_current`) | per core | none | unchanged |
| klock row: `owed`, `CoreLock` depth, `flush_owed_to_swap` | `c` | none | `klock_detach` sets `owed` only when the global lock is held; `klock_attach` acquires only for a nonzero depth; `kickos_switch_unlock` releases only when `owed` is set and always flushes |
| `aspace` install: `g_current[cpu]`, `g_installed_root[core]`, the residency record | per core, except the residency record | the residency record is written by every install and by map, unmap and destroy | S2.1, below |
| MPU, the libc seat word | per core hardware; a word per space | none | unchanged (the seat word is handled outside this design, section H) |

**HAZARDS, EACH WITH ITS ANSWER.**

- *A global-lock holder waking a thread homed elsewhere.* The waker owns the PARKED block by the wait
  edge, publishes `HANDOFF`, and from the staging on writes nothing to that block. The target reads it
  only in its drain, under the lock, so a later write by the waker's span is ordered and not a race;
  the audit S2.3 owes is narrower: a write after `wake_no_resched` to a field the TARGET'S PERFORM HALF
  reads (the switch half reads `ctx`, `mpu`, `task`, `reent`, `call_frame_parked`), found by reading
  forward from every caller of `wake_no_resched`, `wake` and `place_ready` to the end of its span.
- *The deferred switch.* `flush_owed_to_swap` (section C). A handler that books two switches stages
  both displaced threads, each `HANDED`, all published at the exception exit.
- *The slay claim, RULED: NOT A NEW WINDOW.* The claim is read and the redirect applied in the decision
  half, under the lock, so stage 2 changes nothing about when a claim takes effect. What master already
  does is looser than `thread.h:89-92` says: a victim RUNNING on a peer loses that core only at that
  core's next pass (`invariants.md:416`; `park.cc:181-191` posts the raise and returns). S1.3 restates
  the comment to that: no further unprivileged instruction from the victim's next switch-in, and a
  running victim is taken off its core by that core's next pass. `kos_thread_slay`'s answer is not
  affected: it parks the caller on the victim and answers 0 only once the victim has exited and been
  swept (`kernel/syscall/syscall_thread.cc:766-826`).
- *A re-mask racing the switch half.* `affinity` is read by `available_to` (`policy_fifo_rr.cc:106`,
  `:123`) in the pick, which is in the decision half, so a `set_affinity` is either ordered before the
  pick or its `RESEAT` makes the next pass switch the thread away; the window is the same as master's.
- *IRQ-context wakes.* A claimed line's waiter is pinned to the claim core and the line routes there
  (M9.2), so the wake is local and uses no ring. A timer sweep's wake may hand off; its entries are
  staged in the handler and published at the span's end or the exception exit.
- *Aspace install against map, unmap and destroy.* S2.1, below.

**THE rv64 MAP UNWIND, RE-READ (`TODO.md`, the peer-mask item): CLOSED THROUGH STAGE 1, OPEN AT STAGE
2, AND WIDER THAN RECORDED.** Stage 1 keeps every install and every edit under the one lock. Stage 2
moves the install into the perform half, lock-free, and opens:

1. `arch_aspace_map` samples `peers` before the edits (`arch/riscv/rv64imac/aspace_rv64imac.cc:850-851`)
   and spends that sample in the unwind before and after `prune_empty` (`:871-878`) and on the success
   path in `translation_rendezvous` (`:880-884`); `arch_aspace_unmap` does the same (`:901-902`, spent at
   `:912`). A hart that first installs the space after the sample is missing from every one of them:
   on the unwind it can cache non-leaf entries into tables `prune_empty` frees, and on unmap it can keep
   a leaf to a frame being released. The file's own comment at `:871-874` names part of the repair:
   "Separate clear and free passes are required if that lock no longer covers the operation."
2. `Residency::note_and_was_resident` (`arch/include/kickos/arch/aspace_residency.h:70-81`) is a
   read-modify-write of one row's `cores` word from every installing core, serialised today only by the
   lock; concurrent, it loses a bit, and a lost bit is a missed shootdown. armv8a notes the same record
   (`arch/arm64/armv8a/aspace_armv8a.cc:353`).
3. `Residency::close` compacts by moving the last row into the freed one (`aspace_residency.h:118-119`)
   while a lock-free install scans `index_of`, so the install can miss its row, losing the note or, on
   armv8a, reading the wrong ASID through `identifier` (`aspace_armv8a.cc:816`).

**THE FIX, S2.1, AHEAD OF EVERY OTHER STAGE-2 COMMIT.**

- The residency record keeps its rows index-stable (a freed row is marked, not refilled by a move), and
  `cores` becomes one byte per core written by that core alone, read as a mask by map, unmap and destroy.
- The installer, on the 0 -> 1 transition of its byte only, stores the byte, then orders it against
  the walks it is about to make: on rv64 a `fence rw, rw` then the `sfence.vma` the first-use arm of
  `write_satp` already issues (`aspace_rv64imac.cc:216-231`), since implicit translation reads are
  ordered by `sfence.vma` and not by `fence` (`:211-212`, citing Privileged ISA 11.1.1.11 and 12.2.1);
  on armv8a a `dsb ish` then the `TTBR0` write and its `isb`, because `write_ttbr0` issues only an `isb`
  unless it leaves ASID 0 (`aspace_armv8a.cc:341-349`). An already-resident core is in every sample and
  pays nothing.
- The editor clears, `fence rw, rw` / `dmb ish`, and RE-SAMPLES the peers after `publish_edits` on
  every path that spends them: map's success rendezvous and its unwind (sampled again before each
  invalidate, the clear and the free split into two passes), and unmap's rendezvous.
- armv8a's TLB side is broadcast above one core (`invalidate_all` issues `tlbi_all_is`,
  `aspace_armv8a.cc:304-310`; `invalidate_page` the `is` form, `:193`), so the frees in its map unwind
  (`:764`) do not depend on the peer sample; the instruction-side rendezvous after it (`:771`) does,
  and is re-sampled the same way.

Either the editor's re-sample sees the new core, or that core's walks see the cleared entries.
`tests/unit/mapfence` gains an arm that installs between the sample and each spend and asserts the
installing core is in the invalidated set.

**STAGE 2 COMMITS.** S2.1 the residency fix. S2.2 the klock row (`owed` only when held,
`klock_attach` by depth, `flush_owed_to_swap`, `CoreLock` and its exclusion assert), and the telemetry
refusal. S2.3 `pick_and_seat` split into its decision and perform halves for the reschedule dispatch,
the slice expiry and `yield`, with the wake-write audit. S2.4 the non-parking syscall tails, a separate
commit so its share is measured apart. Witness beyond stage 1's set: a selftest stress arm at four
cores racing kills, boosts and re-masks against pushes and slice expiries, run N times; the S2.1
`mapfence` arm; and, where herd7 or an equivalent is on the box, litmus tests for the residency pair on
the AArch64 and RISC-V models, which no run can witness.

## G. The stop-condition measurement

**WHERE A NUMBER CAN COME FROM.** `esp32-wroom-benchsmp` is the only multi-core configuration with a
live cycle counter (240 MHz, two LX6 cores); five of the six emulator presets print 0 Hz and the
sixth is single-core (the envelope, metric 1). Every TIME criterion is read on that board; the
emulators give COUNTS (acquisitions per switch by reason, draws, asks, handoffs, instructions) and
shape. Emulator wall clock is host-load contaminated: five runs per tree per preset, the trees
alternated, on an idle box, spread published, and no emulator time figure refuses or approves
anything. Silicon: ten captures per tree, alternated, last report window each, median and range
published; earlier captures on this board were bit-identical across flashes, so a difference outside
the range is real. **A refusal read on this board is a refusal at two cores, which is the only width
silicon has here**; four cores is emulation and may only confirm shape.

**WORKLOADS.**

| | workload | side |
| --- | --- | --- |
| W1 | the single-flow rows: pinned ping-pong `ns/sw`, `call/reply` 8 B and 256 B, and `BD_CALL_RT` | overhead: the latency posture |
| W2 | one pinned ping-pong pair per core, all cores at once: aggregate switches/s, `lock-wait`, acquisitions per switch | contention |
| W3 | an unpinned equal-priority ping-pong (3.02 acquisitions per switch at two cores, `STATE.md` M9.2) plus round-robin threads on wide masks | the wide-mask workload stage 2 is judged on |
| W4 | the push probe and the reseat probe (`BD_PUSH_E2E`, `BD_RESEAT_E2E`) | the two delivery delays owed |
| W5 | `e2e-local` and the longest interrupt-masked window | tails |

**THE TWO OWED MEASUREMENTS, ON STAGE 1.**

- *The push's latency*, two doorbell hops (the drop ask to the holder, the handoff back), at master and
  at S1.4, `BD_PUSH_E2E` on silicon and on both four-core emulators; the reseat delay beside it is the
  PI delivery delay the roadmap says must be priced.
- *The peer ask against M8's +61.* The method of `TODO.md`'s M8.8 item: object code of `pick_and_seat`
  (into which `switch_book`, `place` and `seat_level` inline) on `qemu-arm64-smp` at four kernel cores,
  MinSizeRel as the presets build, counting the instructions retired on the path "a displaced wide-mask
  thread, no peer strictly below, nobody asked" from the `RUNNING -> READY` store to the end of
  `seat_level`, loops expanded at `N = 4`. At master first, since M9.2 moved it and it has not been
  re-priced, then at S1.4, where each candidate costs `effective` instead of a peer bitmap walk.

**THE THRESHOLDS, RULED BY THE MAINTAINER.** 1 percent on the single-flow call's central figure; one
bucket on every p99 and on the longest masked window; a 5 percent minimum gain on W3. **The 1 percent
is read from the exact means** (`call/reply` and ping-pong `ns/sw`, each a count over an elapsed time):
`BD_CALL_RT`'s p50 is a bucket floor at an eighth of an octave (`docs/reference/bench.md:663`), about 9
percent wide, so on that row the limit is "does not move a bucket". The same limits gate stage 1
against master before stage 2 starts (section E).

**THE BENEFIT CEILING, BEFORE STAGE 2 IS WRITTEN.** Stage 2 can take off the lock only the perform half
of non-parking passes. The phase table aggregates each `PH_*` row over every pass, so the ceiling is
taken from S1.5's per-reason brackets directly and, as a cross-check, reconstructed as

    ceiling(W) = sum over entry reasons r of
                 (non-parking passes of reason r per switch) * (perform-half cycles per pass of r)
                 / (cycles per switch of W)

where the per-pass perform-half cycles, if the per-reason bracket is not trusted, is the aggregate
fraction of `PH_SWITCH_BOOK`, `PH_MPU_APPLY`, `PH_REENT_SEAT`, the comparator write and `PH_ARCH_SWITCH`
over all passes, which assumes a pass's perform half costs the same whatever entered it, stated as the
assumption it is. The call/reply fraction the envelope says is derivable is computed beside it by the
envelope's method.

**STAGE 2 IS REFUSED, AND RECORDED AS REFUSED, IF ANY OF THESE HOLDS**, stated before any run:

- R0, before it is built: `ceiling(W3)` is under 5 percent on silicon. The gain the thresholds require
  could not exist.
- R1: the single-flow call is more than 1 percent worse on silicon (the means), or `BD_CALL_RT` p50
  moves a bucket, S2 tip against S1 tip, outside the published range.
- R2: `BD_CALL_RT` p99, `e2e-local` p99 or the longest masked window is one bucket or more worse on
  silicon.
- R3: W3 cycles per switch is not at least 5 percent better on silicon, or W2 aggregate throughput is
  worse outside the range.
- R4: on either four-core emulator, W3 or W2 acquisitions per switch rise. A count, so the emulators
  may speak to it.

The non-parking syscall tails (S2.4) are judged separately: kept only if they improve on S2.3 by R3's
measure and breach none of R1 or R2. A refusal removes the stage-2 commits and keeps stage 1.

## H. Scope, rulings, and the P6 lifetime

**THE P6 LIFETIME, AND WHY THE AMP RING AND THE SCHEDULER RING ARE TWO STRUCTURES.** `take_reply`
copies a reply slot onto the handler's stack (`kernel/amp/ampwindow.cc:604`), advances the tail at once
(`:613`), and `dispatch_reply` copies again into the caller (`:253-261`,
`kernel/syscall/syscall_ipc.cc:1064`), both inside the masked service; M8.11 priced one 256-byte copy at
about 15 us on `esp32c6-wroom` and 34 us on `f411disco`. The lifetime that moves both copies out, which
`TODO.md`'s P6 item and N6f's ordered reclamation with a released mask already decide:

- the service validates the header snapshot as today and resolves the tag. Where it resolves to a
  parked caller, the service records on that caller the held slot and its bounded length, marks the
  slot TAKEN and not RELEASED, and wakes it, copying nothing; the tail does not advance. Where it does
  not resolve (`dispatch_reply` false), the service marks the slot RELEASED itself, or the tail would
  never move past it;
- the caller, resuming in its own call in its own space, copies once from the slot into its buffer, then
  under its lock releases the slot. The reply ring gains the CALL ring's reclamation: a released mask per
  pair and the tail advanced over the leading released run, and the credit-return raise moves from the
  take to that advance, keeping its liveness role;
- the release is total over the caller's exits: the resume, a timeout or cancel that finds a held reply,
  and the slay exit and `exit_current`, which never run the continuation;
- the CALL side needs no lifetime change (its slot is already held until the reply), so the receiver
  copies from the held call slot in its own context. Both 256-byte stack buffers in `node_service`
  (`ampwindow.cc:796`, `:834`) leave the handler's frame.

**The copy needs a kernel continuation, and two existing properties give it one.** The AMP presets on
`pizero2350` are armv7m (`boards/pizero2350/board.cmake:13`), which has the register fastpath
(`arch/arm/armv7m/ipc_fastpath.cmake:9`), so the constraint is not the absence of the fastpath: it is
that a far endpoint falls through the fastpath to the slow path (`kernel/syscall/syscall_ipc_fast.cc:103-106`),
which parks a far caller with a continuation, and that the fastpath parks only a LOCAL caller
(`:170-178`). Both become load-bearing for P6 and each gets an arm in S1.6/S1.7.

A far peer can rewrite a held slot during the copy, as it can today; the length was bounded from the
snapshot and the validation defends against a malformed peer, never a hostile one (N6f). Reclamation
stays ordered, so a starved caller holding the oldest reply slot delays the tail, and the serving node's
admission (`used + owed <= RING_SLOTS`) answers RESERVE to further calls from this node until it runs;
that is the ordered reclamation N6f already chose, bounded by `RING_SLOTS` replies.

**TWO STRUCTURES, ONE DISCIPLINE.** They share the ordered-pair rule, one writer per index on its own
line, publication before the raise, and the raise as a hint. They share nothing else, for four reasons:
the AMP ring crosses a trust boundary and validates every far index and length, which inside one kernel
would validate the kernel against itself; its slot is a 256-byte message and the scheduler's a two-byte
index, so one type would multiply the quadratic storage by more than a hundred; its depth is four with
FULL and RESERVE as answers, while the scheduler ring's depth is derived from this kernel's own thread
count so that it can never fill, a bound no node can state about a peer's threads; and the AMP ring lives
in the window both images link, the scheduler ring in `Kernel`.

**RULINGS THIS DESIGN RECORDS**, none of them open:

- Slay exactness is not a fork (section F): master is already looser than `thread.h:89-92`, stage 2 adds
  no case, and the comment is restated in S1.3.
- P6's REPLY side lands (S1.7), on the ordered reclamation N6f and `TODO.md`'s P6 item decided.
- The thresholds are 1 percent, one bucket, 5 percent, and they gate stage 1 against master too (section
  G).
- The placement invariant is exact in stage 1 by the read of section D, and stage 2 keeps every placement
  decision under the lock because no cheap exact lock-free read exists; the invariant is not weakened.
- lx6's full barrier is `S32C1I` on a per-core word, and lx6 runs stage 2 like the others (section C).

**WHAT THIS DESIGN DOES NOT DECIDE.** Capability lifetime and resolve-to-use (M9.5's precondition, held
under the global lock here); same-owner IPC leaving the lock (M9.5); the console contract (M9.6); a
per-core sleep queue; any balancing or pull (refused); helping by migration (refused); the ceiling as an
admission rule (the sealed profile's); a cap on the effective-priority recompute over an unbounded donor
set, or on the linear pick and pop; the lx6 draw bound; anything at eight cores but storage; the RK3588
re-check at width; the routed mask touch, which stays as it is (`kernel/irq/irq_route.cc:81-100`) and
which this design does not copy, adding no cross-core wait; the register fastpath above one core; shared
kernel stacks. The libc seat word shared by two cores running threads of one space
(`kernel/thread/reent.cc:115-137`) is handled outside this design.
