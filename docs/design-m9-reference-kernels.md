<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# The reference-kernel survey -- lock domains, remote wake, and where KickOS sits

> **Status: EXPLORATORY.** M9.0's first deliverable, recorded 2026-09-22. It enumerates a
> design space and commits to nothing. Nothing here licenses a change, and no row is a
> verdict about the project it describes.

## How to read this

**It does not rank.** No row is better or worse than another, and a majority among these
projects is not an argument for a KickOS design choice. A single-core kernel is not an SMP
precedent, and the one row that is single-core says which questions it therefore does not
answer rather than answering them anyway.

**Every row carries a LICENCE, because the clean-room rule is only checkable against one.**
KickOS is CECILL-C. Several of these trees are GPL or LGPL, and the rows with the most SMP
design content are among them, so lifting recognisable expression from them is a legal
problem and not a matter of style. **Nothing in this document is copied from any surveyed
tree.** Every finding is described in this project's own words and cited by path into the
tree it came from.

**A path outside this repository is relative to that project's checkout**, written
`<project>/<path>`. Where those checkouts live, and at which revision, is recorded in
`CONTEXT.local.md`, which is gitignored so that no personal path ships here. The revisions
printed in each row were re-derived with `git -C <checkout> rev-parse --short=8 HEAD` when
the row was written; **re-derive before citing one again**, because that file records and
this one reports.

**And no gate can check any of it.** The doc-name gate resolves in-repo paths and strips a `:N`
without verifying it, and a path whose first component is not tracked here is dropped -- which is
every citation into a surveyed tree, the large majority of the citations in this document. They
were checked by hand when written and they will drift on any upstream pull. **A claim here is
evidence that somebody opened that file at that revision, and nothing automatic will tell you when
that stops being true.** Re-derive the revision before leaning on a row, and treat a line number
that no longer says what the row claims as the row being stale rather than the source being wrong.

**Two claims had to be checked before either could be cited**, both about the most-cited
precedent in the set. They are answered inside the seL4 row, under *Claim (a)* and
*Claim (b)*: whether its IPC fastpath takes a queue lock on the same acquisition as the rest
of the kernel entry, and what the project's own documents say its verification envelope
excludes.

**Each row answers nine points** where they apply: licence, revision, lock domains,
acquisition order, remote wake, migration, whether the kernel lock is released inside the
context switch, the kernel stack model, and what benchmark evidence the project publishes
for its own claims. Rows for which a point does not apply say so rather than inventing an
answer.

## The nineteen rows at a glance

| row | licence | lock domains | across the switch | own published numbers |
| --- | --- | --- | --- | --- |
| seL4 | GPL-2.0-only kernel, BSD-2 userland | one CLH big kernel lock, nothing else | held | none in tree |
| Fiasco.OC | MIT kernel, third-party subtrees vary | partitioned, no global lock; the ready queue has none | no global lock to hold | none; asks for review before publication |
| NuttX | Apache-2.0 | one thread-owned critical section plus leaf spinlocks | re-evaluated at the switch | none for SMP or IPC |
| Zephyr | Apache-2.0 | one scheduler lock plus per-subsystem and per-object locks | released just before the register switch | harnesses, no SMP figures |
| RTEMS | BSD-2-Clause across the score | fully partitioned, per-instance ticket locks | no lock held; a disable counter is handed over | commits figures on named hardware |
| ThreadX | MIT | one protection word plus a per-TCB claim bit | released before the scheduler loop | single-core harness, no tables |
| RT-Thread | Apache-2.0 | one scheduler lock over a two-level ready structure | held, dropped on the new stack | single-core harness, no figures |
| ChibiOS | GPL-3.0-only, with a commercial licence offered beside it (`ChibiOS/os/license/`) | one spinlock over per-core OS instances | held | benchmark sequence, not SMP |
| RIOT | LGPL-2.1-only | none; the kernel is single-core | not applicable | IPC and scheduling harnesses, single-core |
| FreeRTOS-LTS | MIT kernel; RP2040 port MIT AND BSD-3-Clause; pico-sdk BSD-3-Clause | two global locks, task then ISR | released before the switch | none for the kernel |
| Composite | no licence file; README states GPL-2.0 with a class-path exception, kernel headers plain GPLv2 | none; zero locks, one CAS per update | not applicable | harness in tree, figures in papers |
| Linux | GPL-2.0 WITH Linux-syscall-note | one per runqueue | not released inside on this path | none in tree for this mechanism |
| KickOS, the AMP window | CECILL-C | none taken | not applicable | the masked-copy cost, banked |
| KickOS, the routed mask touch | CECILL-C | none taken | not applicable | none specific to this path |
| ABSENT: a multikernel with a published lock-versus-message crossover | -- | -- | -- | -- |
| ABSENT: a capability kernel binding every context to a core | -- | -- | -- | -- |
| PARTLY ABSENT: seL4's scheduling contexts -- present, the argument for them is not | -- | -- | -- | -- |
| PARTLY ABSENT: Fiasco.OC's scheduling contexts -- present, the userland that drives them is not | -- | -- | -- | -- |
| ABSENT: message passing with inheritance across the message path | -- | -- | -- | -- |

The last five rows are named so each gap is a record rather than something this document
implies by silence, and two of them turned out to be partial rather than absent. They are at
*Named and absent from this box*, below.

## The ten named kernels

## seL4

**Licence** GPL-2.0-only for kernel code, 2-clause BSD for the user-level libraries, stated in
`seL4/LICENSE.md` lines 7-24 and carried per file as an SPDX tag
(`seL4/include/smp/lock.h` line 4).
**Revision** `28b8f4c4`, `git describe` reports `16.0.0-27-g28b8f4c40`.

**1. Lock domains.** One, and only one. `big_kernel_lock` is declared in
`seL4/include/smp/lock.h` line 50 and defined in
`seL4/src/smp/lock.c` line 14. It is a CLH queue lock: each core owns a
cache-line-aligned node and a request slot, the acquirer swaps its request onto the tail and spins on
its predecessor's slot (`seL4/include/smp/lock.h` lines 22-48, 59-87). A grep of `seL4/src/` and `include/` for
any other exclusion primitive -- spinlock, mutex, per-object lock -- returns nothing. There is no
endpoint lock, no per-TCB lock, no run-queue lock. Every kernel data structure, including all
per-core scheduler state, is protected by the one lock.

**2. Acquisition order.** Not applicable: with a single lock there is no order to state. The lock is
taken INSIDE the interrupt mask. seL4 runs the kernel with interrupts disabled throughout, so the
mask is already in effect at the trap vector, and the acquire is the first thing the C entry point
does (`seL4/src/arch/arm/c_traps.c` lines 22, 59, 126, 142, 159, 179, 200).
The interrupt entry is the one asymmetry: it acquires conditionally, skipping the acquire when the
active interrupt is the remote-call IPI (`seL4/src/arch/arm/c_traps.c` line 89), because that IPI is
serviced from inside the lock's own spin loop. That spin loop polls a software IPI flag and dispatches
a remote call while still waiting for the lock (`seL4/include/smp/lock.h` lines 70-83) -- the lock wait is
therefore a point at which the kernel executes work for another core.

**3. Remote wake.** Inline reach into the peer's queue, followed by a deferred non-blocking IPI. The
whole kernel is under the one lock, so a core making a thread runnable simply enqueues it into the
target core's run queue directly (`SCHED_ENQUEUE`, `seL4/include/object/tcb.h`
lines 112-116). The enqueue macro then calls `remoteQueueUpdate`
(`seL4/src/object/tcb.c` lines 444-461), which decides whether the peer needs
telling: only if the woken thread is in the current domain AND the peer is running its idle thread or a
lower-priority thread. If so it ORs the peer's bit into a per-core pending mask, `ipiReschedulePending`
(`seL4/src/object/tcb.c` line 458). The mask is flushed once, at the end of `schedule()`
(`seL4/src/kernel/thread.c` lines 420-423), as a non-blocking reschedule IPI
(`doMaskReschedule`, `seL4/src/smp/ipi.c` lines 153-160). So the wake itself
never blocks and never fails; multiple wakes to the same peer within one kernel entry coalesce into one
IPI.

The second cross-core mechanism is the blocking remote call, `doRemoteMaskOp`
(`seL4/src/smp/ipi.c` lines 136-151): the arguments are published in a single shared slot inside
`big_kernel_lock` (`seL4/include/smp/lock.h` line 47), the blocking IPI flags are set on each target's node,
and the caller then waits on a sense-reversing barrier, `ipi_wait` (`seL4/src/smp/ipi.c` lines 85-103), until
every target has run the call. This path DOES block waiting on peers. It is used for TLB shootdown, FPU
owner switching and interrupt masking (`seL4/include/arch/arm/arch/smp/ipi_inline.h`).
There is exactly one argument slot, so only one blocking remote call can be in flight kernel-wide --
which is consistent, since the caller holds the one lock. The consequence is worth stating plainly: a
shootdown or a stall is performed with the big kernel lock held, so for its duration no other core can
enter the kernel at all, and the targets answer only because the lock's own spin loop dispatches
remote-call IPIs while waiting (`seL4/include/smp/lock.h` lines 74-81). The blocking remote call and the lock
wait are deliberately interlocked; neither works without the other.

**4. Migration.** A thread has a single `tcbAffinity` field and runs only on that core. Migration is an
explicit act by the holder of a scheduling capability, never a load-balancing decision -- seL4 has no
load balancer. `migrateTCB` (`seL4/src/model/smp.c` lines 13-31) just rewrites
the affinity field and releases the FPU. It is a yank, and the yank is made safe by first stalling the
peer: `remoteTCBStall` (`seL4/src/object/tcb.c` lines 466-479) checks whether
the target thread is the one currently running on its home core, and if so issues a BLOCKING remote call
that makes that core park the thread and switch to idle
(`ipiStallCoreCallback`, `seL4/src/smp/ipi.c` lines 22-79). The caller is
blocked in `ipi_wait` until the peer has done it. Callers are in
`seL4/src/object/schedcontrol.c` lines 21, 55 and
`seL4/src/object/schedcontext.c` lines 154, 320, 357, 370, 381.

**5. Lock across the context switch.** HELD. There is no point at which the lock is dropped mid-switch:
it is acquired at the C trap entry and released in `restore_user_context`, immediately before the
register restore and the exception return (`seL4/src/arch/arm/64/c_traps.c`
line 27; the fastpath has its own release at
`seL4/include/arch/arm/arch/64/mode/fastpath/fastpath.h` line 148). Switching
threads is only a change to `ksCurThread` plus a vspace switch, so nothing about it needs the lock
released. The consequence is that the lock is held for the entire kernel entry, from the trap to the
`eret`.

**6. Kernel stack model.** One stack per CPU, not per thread. `kernel_stack_alloc` is a
`CONFIG_MAX_NUM_NODES`-element array of stacks
(`seL4/include/arch/arm/arch/64/mode/smp/smp.h` line 15, same on RISC-V at
`seL4/include/arch/riscv/arch/model/smp.h` line 19 and on x86 at
`seL4/include/arch/x86/arch/32/mode/model/smp.h`
line 11). On several architectures the current core index is recovered by dividing the stack pointer by
the stack size (`seL4/include/arch/arm/arch/32/mode/smp/smp.h` lines 16-18,
`seL4/include/arch/riscv/arch/model/smp.h` lines 50-52), which forecloses per-thread kernel stacks by
construction. The kernel is event-based: an entry runs to completion and returns to user, so there is no
blocked kernel context to keep a stack for. Operations too long to run atomically are made restartable
rather than stackable -- `preemptionPoint`
(`seL4/src/model/preemption.c` line 16) is called from the loops in
`seL4/src/object/cnode.c` lines 543, 650 and
`seL4/src/object/untyped.c` line 263, and progress is kept in the kernel
objects themselves so the thread re-enters the syscall and continues. This is saved operation state, not
continuations: no callback closure is stored, the syscall is simply re-executed.

**7. Refusal, blocking, PI, fairness.**
- A wake cannot be refused. The run queues are intrusive doubly-linked lists indexed by priority, so
  there is no full condition to hit.
- A cross-core path does block on a peer in two cases: `doRemoteMaskOp` and everything built on it
  (shootdown, FPU, stall). The reschedule IPI path does not.
- Priority inheritance: none. seL4 has no in-kernel PI. Under MCS the related mechanism is scheduling
  context donation down the call stack (`reply_push` and `schedContext_donate`,
  `seL4/src/object/reply.c` lines 9-51,
  `seL4/src/object/schedcontext.c` lines 362-382). It does not carry a priority
  across cores; instead it MOVES THE THREAD. A scheduling context is pinned to a core through `scCore`,
  and donating it to a callee migrates the callee onto that core (`schedcontext.c` line 381), after
  stalling the previous holder's core with a blocking remote call if it was elsewhere (line 370). So a
  cross-core call under MCS is not a priority propagation, it is a migration.
- Fairness: yes, and this is the notable property. CLH is a FIFO queue lock, so a core that has enqueued
  its request is overtaken by no one. The wait is bounded by the number of cores ahead of it. The cost is
  that a core holding the lock cannot be skipped, which is why the spin loop has to service remote-call
  IPIs while waiting.

**8. Published benchmark evidence.** None in this tree. The kernel ships instrumentation, not results:
`KernelBenchmark` selects between kernel-entry tracking, tracepoints and utilisation tracking
(`seL4/config.cmake` lines 210-230, with the sources under
`seL4/src/benchmark/`). Every one of those options is disabled in a
verification build (the `NOT KernelVerificationBuild` guard on lines 222-225). The benchmark suite that
consumes this instrumentation, sel4bench, is a separate repository and is not on this box. The manual
contains no performance figures; a grep of `seL4/manual/` for performance claims finds only a sentence about
hardware performance sampling (`seL4/manual/parts/threads.tex` line 470).

### Claim (a): where the fastpath takes its lock

There is no separate endpoint or queue lock. The fastpath runs entirely under the one big kernel lock,
on the same acquisition as the rest of the kernel entry, and the fastpath code itself never acquires
anything: the lock is already held when it starts, taken by the C trap entry before the fastpath function
is called (`seL4/src/arch/arm/c_traps.c` line 142 for the call fastpath, 159
for the signal fastpath, 179 for the reply-recv fastpath, and 126 for the slowpath -- the same acquire in
every case), and the only lock operation in the fastpath is the RELEASE on the way out
(`seL4/include/arch/arm/arch/64/mode/fastpath/fastpath.h` line 148, and the
equivalent at line 127 of the 32-bit header and lines 100 and 136 of the x86 headers). A whole-tree grep
for a second exclusion primitive returns nothing at all.

The fastpath is moreover same-core only. `seL4/src/fastpath/fastpath.c` lines
160-165 bail to the slowpath when sender and receiver have different affinities. So seL4's fast IPC
number is a same-core number, produced while holding a lock that every other core also needs.

### Claim (b): what seL4's own documents exclude from verification

`seL4/CAVEATS.md` states it directly. The SMP section, lines 130-175:

- SMP exists and is supported by the seL4 Foundation, but is NOT formally verified (lines 131-133).
- SMP plus hypervisor extensions: supported, also not verified (lines 135-137).
- SMP plus MCS: supported and actively developed, but explicitly "experimental", less explored and less
  tested, with no supported Armv7-a boards (lines 139-144).
- SMP plus MCS plus hypervisor: AArch64 only, less tested, lower code coverage (lines 146-148).
- SMP plus the domain scheduler: not supported at all. And the SMP configuration "is not expected to
  satisfy strong intransitive non-interference for information flow" (lines 150-152) -- that is, the
  information-flow result does not carry over.
- Line 156-157 is the one that matters most for reading any "seL4 is verified" claim: because these are
  unverified configurations, ordinary C implementation defects are possible and are NOT excluded the way
  they are in the verified configurations.
- Lines 159-176 name the intended route to assurance on multicore hardware: not verifying the SMP
  kernel, but a static multi-kernel configuration, one seL4 instance per core over disjoint memory, with
  verification of that on the AArch64 roadmap and initial work begun. The stated reason is that it is
  simpler and closer to the existing sequential proofs. The changelog shows that direction being built:
  software-generated interrupts were added for GICv2 and GICv3 platforms specifically in NON-SMP
  configurations, as the signalling mechanism between cores in a multi-kernel setup, exposed as a new
  capability type (`seL4/CHANGES.md` lines 259-264).

Two further exclusions bound the envelope. The functional-correctness proofs cover a named list of
platforms and configurations only -- AArch32 Armv7-a without SMMU and without FPU, AArch64 Armv8-a with
hypervisor extensions only, RISC-V 64-bit without FPU and WITHOUT the fastpath
(`CAVEATS.md` lines 11-50) -- and the proofs are sensitive to configuration parameters and break when
they change (lines 89-102). Separately, the default non-MCS kernel has non-preemptible long-running
operations that must be avoided by system configuration if low latency is required (lines 103-110).

## Fiasco.OC

**Licence** MIT for the microkernel as the project declares it, with third-party subtrees under
other licences, and a handful of per-file headers under `fiasco/src/kern/arm/bsp/qcom/` carrying
GPL-2.0-only OR a vendor reference that no SPDX subpackage covers -- none of them cited here
(LGPL-2.1-or-later, GPL-2.0-only, GPL-2.0-or-later OR BSD-2-Clause). Stated in
`fiasco/README.md` lines 66-70 and machine-readable in
`fiasco/LICENSE.spdx` (the `fiasco` package, `PackageLicenseDeclared: MIT`,
lines 19-24; the third-party packages follow).
**Revision** `6437d44e`, `git describe` reports `r-2026-W36`. Kernel only on this box; no L4Re userland.

**1. Lock domains.** A partitioned set, and no global kernel lock exists. Two layers:

- `Cpu_lock`, a per-core interrupt mask (`fiasco/src/kern/cpu_lock.cpp`).
  This is the only thing guarding per-core state.
- `Spin_lock<T>`, one instance per resource
  (`fiasco/src/kern/spin_lock.cpp` lines 27-130). Instances are scattered
  across the kernel, each named after what it protects: the ASID allocator
  (`fiasco/src/kern/asid_alloc.h` line 434), the kernel memory allocator
  (`fiasco/src/kern/kmem_alloc.cpp` line 42),
  the MMIO mapper (`fiasco/src/kern/kmem_mmio.cpp` line 46), RCU (`fiasco/src/kern/rcupdate.cpp`
  line 125), each IRQ
  chip (`fiasco/src/kern/irq_chip.cpp` line 296, `fiasco/src/kern/irq_chip_generic.cpp` line 13),
  each priority list
  (`fiasco/src/kern/prio_list.cpp` line 58), the per-task PI waiter chain
  (`fiasco/src/kern/space.cpp` line 223), the per-core pending-request queue
  (`fiasco/src/kern/queue.cpp` line 11 via `Spin_lock_coloc`), and more.

The design point that follows: the READY QUEUE HAS NO LOCK AT ALL. `Sched_context::rq` is a per-core
`Ready_queue` (`fiasco/src/kern/sched_context.cpp` lines 55, 66), and the
operations on it assert only that the CPU lock is held
(`fiasco/src/kern/ready_queue_fp.cpp` lines 114, 137;
`fiasco/src/kern/sched_context.cpp` lines 17, 28, 39, 125). A core's run queue
is touched by that core alone; everything else goes through the request mechanism in point 3.

Above the spinlocks sits `Switch_lock`
(`fiasco/src/kern/switch_lock.cpp`), a blocking lock on kernel objects that
implements priority inheritance by helping, and its bootstrap wrapper `Helping_lock`
(`fiasco/src/kern/helping_lock.cpp`). Acquiring a `Switch_lock` is documented as a preemption point
(`fiasco/src/kern/switch_lock.cpp` lines 27-28, 167-169).

**2. Acquisition order.** The one order that is mechanically enforced is mask-then-lock:
`Spin_lock::lock()` asserts the CPU lock is not yet held, takes it, and only then takes the arch word
(`fiasco/src/kern/spin_lock.cpp` lines 181-188). So the spinlock is always
taken INSIDE the interrupt mask, never outside, and a caller who already masked must instead use the
`No_cpu_lock_policy` guard, which asserts the mask is in effect and takes only the arch word
(`fiasco/src/kern/spin_lock.cpp` lines 138-151). That guard is what the PI code uses
(`fiasco/src/kern/pi_mutex.cpp` lines 305, 344, 780, 848, 1019, 1109). No
global ordering between the spinlock instances is written down in the tree, and no lockdep-style checker
exists; the ordering is carried per call site in preconditions, for example the documented precondition
that the home core's pending-queue lock be held when the home core is offline
(`fiasco/src/kern/context.cpp` lines 1462-1464), or the requirement that the
CPU lock NOT be held on entry to the cross-core call interface
(`fiasco/src/kern/cpu_call.cpp` lines 193, 216).

Where two locks of the same kind must be taken together, Fiasco orders them BY OBJECT ADDRESS rather
than by a static rank: the mapping database locks a source and destination capability pair in address
order, and the comment says why -- a second thread may call concurrently with the two arguments swapped
(`fiasco/src/kern/kobject_mapdb.cpp` lines 220-226). That is the one
mechanically reliable ordering rule in the tree, and it works precisely because the locks are peers.

**3. Remote wake.** A published inbox, two levels deep, with a coalescing IPI. The mechanism is the DRQ,
the deferred request queue (`fiasco/src/kern/context.cpp` lines 164-270 for the
types, 1636-1699 for the send). Each context owns a queue of requests aimed at it, plus a second "eager"
queue for requests to run directly in the IPI handler (`fiasco/src/kern/context.cpp` lines 402-408). To act on a
context that lives on another core, a core enqueues a request item into that context's queue rather than
touching the core's scheduler state.

`enqueue_drq` (`fiasco/src/kern/context.cpp` lines 2416-2478, the MP variant;
the single-core variant is at line 1884) does the
following: if the target's home core is the current core, execute the request inline. Otherwise enqueue
the item, re-read the home core in case the target migrated meanwhile, take the target core's
`_pending_rqq` lock, re-check the home core under that lock, and link the target context into that core's
pending list. An IPI is sent only when that list transitions from empty -- which is the coalescing rule:
many pending contexts on one core cost one IPI. That per-core inbox is `Per_cpu<Pending_rqq>`
(`fiasco/src/kern/context.cpp` lines 2062-2083), and its lock costs no extra
word: `Spin_lock_coloc` keeps the lock bit in the spare low bits of the queue head pointer
(`fiasco/src/kern/queue.cpp` lines 9-30 and
`fiasco/src/kern/spin_lock.cpp` lines 55-59, 130). Requests are run in the target context itself, after the
target core switches to it (`Context::handle_drq`, `fiasco/src/kern/context.cpp` line 1471, reached from
`switch_handle_drq`, line 1506, which runs immediately after the register switch).

The caller's choice of `Drq::Wait` or `Drq::No_wait`
(`fiasco/src/kern/context.cpp` lines 1665-1710) decides whether it waits. On
`Wait` the caller does NOT spin: it clears its own ready bits and calls `schedule()`, that is it blocks
as a thread and the reply DRQ wakes it. A queued request can also be withdrawn before the target picks it
up (`Context::abort_drq`, `fiasco/src/kern/context.cpp` line 1754).

**4. Migration.** A thread has a home core, `_home_cpu`
(`fiasco/src/kern/context.cpp` lines 351-354), and changing it is an ask, not a
yank -- but the asker waits. `Thread::migrate`
(`fiasco/src/kern/thread.cpp` lines 1208-1237) publishes a `Migration` record on
the target thread with a compare-and-swap, marking any previous record stale, then either performs the
move locally when it is already on the home core or calls `migrate_xcpu`
(`fiasco/src/kern/thread.cpp` lines 1507-1548), which takes the home core's
pending-queue lock, re-checks the home core under it, posts the thread into that core's pending list and
sends an IPI only if the list was empty. The requester then DROPS the CPU lock and spin-waits on a flag
in the migration record until the home core reports the move done
(`fiasco/src/kern/thread.cpp` lines 1234-1237). Migration is caused by the scheduler interface, that is by a
holder of the scheduler capability; there is no load balancer in the kernel.

Separately, and independently of migration, a thread can TEMPORARILY execute on a core that is not its
home core: the helping path runs a lock owner on the helper's core. The state machine guarding that is
`Running_under_lock` (`fiasco/src/kern/context.cpp` lines 1929-2052), with
three states covering "a thread on a foreign core intends to help" and "this thread is running under a
helping lock". `_switch_exec_common` documents that thread state must be read without assuming core
locality for exactly this reason (`fiasco/src/kern/context.cpp` lines 1272-1278).

**5. Lock across the context switch.** There is no global kernel lock to release. The CPU lock, the
interrupt mask, IS held across the register switch: `switch_cpu` is called with it held and the incoming
context resumes with it still held, servicing its pending requests immediately afterwards
(`fiasco/src/kern/context.cpp` lines 1291-1294). Spinlocks are held across a
switch in specific documented cases, for instance the home core's pending-queue lock held while the
migration path dequeues the thread from the old ready queue
(`fiasco/src/kern/thread.cpp` lines 1517-1530, the offline-core branch of
`migrate_xcpu`, which says so in a comment). The `Switch_lock` is by design held across switches
-- that is what helping means -- and
ownership transfer is explicit (`fiasco/src/kern/switch_lock.cpp` lines 325-400).

**6. Kernel stack model.** One kernel stack per thread. Each `Context` carries a saved kernel stack
pointer, `_kernel_sp` (`fiasco/src/kern/context.cpp` line 362, initialised at
line 660, reset at 673), and the thread control block and its kernel stack share one aligned block of
`THREAD_BLOCK_SIZE` (`fiasco/src/kern/config_tcbsize.h` lines 6-10, 0x800 to
0x2000 depending on configuration), which is how the kernel finds the current TCB from the stack pointer.
A thread blocking in the kernel simply leaves its kernel state on its own stack; there is no
continuation and no saved operation state, which is what lets `Switch_lock` acquisition and DRQ wait be
ordinary preemption points in the middle of kernel code.

**7. Refusal, blocking, PI, fairness.**
- A wake cannot be refused. The request queues are intrusive lists of `Queue_item`
  (`fiasco/src/kern/queue_item.cpp`,
  `fiasco/src/kern/queue.cpp`), with the storage in the requesting context, so
  there is no capacity and no full condition.
- Cross-core paths that wait on a peer: the `Drq::Wait` send blocks the calling thread (it deschedules,
  it does not spin), and `Thread::migrate` spin-waits on the peer to finish the move
  (`fiasco/src/kern/thread.cpp` lines 1234-1237).
- Priority inheritance crosses cores, in two distinct ways. `Switch_lock` inheritance is helping: the
  contender lends its own core to the lock owner, running the owner on a core that is not the owner's
  home core (`fiasco/src/kern/switch_lock.cpp` lines 146-156, 182-218). The
  user-visible `Pi_mutex` propagates an effective priority to an owner or waiter that may be on another
  core through `Context::xcpu_pi_update_prio`
  (`fiasco/src/kern/context.cpp` lines 2660-2725): when the target's home core
  is remote, it takes a per-context remote-state-change lock, ORs the state bits into a published record,
  and posts the context into the peer's pending list. Call sites are
  `fiasco/src/kern/pi_mutex.cpp` lines 804, 987, 989, 1084. The chain itself is
  protected by a per-task lock, `Space::pi_chain_lock`
  (`fiasco/src/kern/space.cpp` lines 222-223). The chain walk is bounded at 128
  hops and treats exceeding that, or finding the initiating waiter as an owner further down the chain, as
  a detected deadlock, which it reports rather than papering over
  (`fiasco/src/kern/pi_mutex.cpp` lines 84-112, 142-144).
- Fairness: none at the spinlock. The AArch64 `lock_arch` is a test-and-set on a single bit with a
  wait-for-event loop, not a ticket and not a queue
  (`fiasco/src/kern/arm/64/spin_lock-arm-64.cpp` lines 14-45; the other
  architectures are in the sibling `spin_lock-*.cpp` files). There is no bound on how long a contender
  waits. Fairness in Fiasco is a property of the scheduler and of the helping protocol, not of the lock
  word.

**8. Published benchmark evidence.** None, and the project asks that none be published without review.
`fiasco/BENCHMARKING` requests that benchmark results be sent to the project
for feedback and approval before publication, on the grounds that the configuration space is wide, and
requires `CONFIG_PERFORMANCE` (`fiasco/src/Kconfig` line 629) to be selected
for any performance analysis. In-tree there is a unit-test framework rather than a benchmark harness
(`fiasco/src/test/utest/framework/utest_fw.cpp`,
`fiasco/src/Makerules.UTEST`), plus a kernel-timing probe inside the debugger
(`fiasco/src/jdb/jdb_kern_info-bench.cpp` and its per-architecture variants).
The only documentation in the tree is the debugger manual
(`fiasco/doc/jdb_manual.rst`). No numbers.

## Zephyr

**Licence** Apache-2.0, stated in `zephyr/LICENSE` and carried per file as an
SPDX tag (`zephyr/kernel/sched.c` line 4,
`zephyr/include/zephyr/spinlock.h` line 4).
`zephyr/README.license` and
`zephyr/doc/LICENSING.rst` lines 8 and 23 both state that the `zephyr/LICENSES/`
directory exists for the REUSE specification and does NOT define the project licence.
**Revision** `681173c8`, `git describe` reports `v4.4.0-14380-g681173c8f62`, dated 2026-09-04. This is a
development revision a long way past the v4.4.0 tag; `zephyr/kernel/smp/` is a directory here and
`zephyr/kernel/scheduler.c`, `zephyr/kernel/spinlock_validate.c`, `zephyr/kernel/timeslicing.c`,
`zephyr/kernel/include/kspinlock.h` and `zephyr/kernel/include/run_q.h` exist as separate files.
Citations are to this
revision.

**1. Lock domains.** One global scheduler lock plus a partitioned set of subsystem and per-object locks.
The scheduler lock is `_sched_spinlock`, defined at
`zephyr/kernel/sched.c` line 36 and declared at
`zephyr/kernel/include/kspinlock.h` line 12, with wrappers at lines 22-80 of
that header (the whole thing compiles away on a single-CPU build). Under it: the ready queue and all
enqueue/dequeue (`zephyr/kernel/include/run_q.h` lines 51-105), thread
scheduling state and the halt protocol (`zephyr/kernel/sched.c` lines 307-374, 849-976), ALL wait-queue pend
and unpend (`zephyr/kernel/sched.c` lines 437-482, 555-568, 795-817), priority changes
(`zephyr/kernel/sched.c` lines 587-641), the per-core `swap_ok` scratch flag, the pending-IPI bitmask, and the
CPU affinity masks (`zephyr/kernel/smp/cpu_mask.c` lines 26-41).

Outside it, each an independent `k_spinlock`: a global lock per subsystem -- mutex
(`zephyr/kernel/mutex.c` line 75), condvar, poll, work, timeout, timer, usage,
memory domains, futexes, CPU bring-up, the IPI work lists -- and a lock embedded in each object instance
for queue, stack, pipe, msgq, mailbox, events, heap and mem_slab. Semaphores are the one striped case:
`sem_locks[CONFIG_SEM_LOCK_STRIPES]` with a hash from the object address
(`zephyr/kernel/sem.c` lines 30-61). The mutex lock is global rather than
per-object on purpose, because the inheritance chain walk touches mutexes other than the one passed in
(`zephyr/kernel/mutex.c` lines 68-74). Separately, the legacy `irq_lock()` becomes a single global atomic under
SMP (`zephyr/kernel/smp/smp.c` lines 12, 57-91).

`CONFIG_SCHED_CPU_MASK` does not partition the scheduler lock; it only adds a per-thread affinity word.

**2. Acquisition order.** No global order is written down, no ordering graph and no cycle detector. What
exists is one structural rule plus scattered pairwise statements:

- The structural rule is object-lock outer, scheduler lock inner. `z_pend_curr` asserts that the caller's
  object lock is not the scheduler lock (`zephyr/kernel/sched.c` line 539) and then takes the scheduler lock
  while the caller's lock is still held (lines 541-552).
- One pairwise order is stated in prose with its failure mode: `zephyr/kernel/mutex.c` lines 35-42 says taking
  the mutex lock from a timer handler reverses the order used on unlock -- mutex lock outer, scheduler
  lock inner -- and deadlocks on SMP. The code matches (lines 422 and 444).
- Timeouts are declared a leaf: the timeout code takes no other lock and calls no other subsystem while
  holding its own (`zephyr/kernel/timeout.c` lines 22-26). Callers go
  scheduler lock -> timeout lock (`zephyr/kernel/sched.c` lines 456-464).
- The generic rule that distinct locks may nest but a lock is never re-taken by its holder is in the API
  docs (`zephyr/include/zephyr/spinlock.h` lines 174-177,
  `zephyr/doc/kernel/services/smp/smp.rst` lines 58-67).

`CONFIG_SPIN_VALIDATE` (`zephyr/subsys/debug/Kconfig` lines 207-220,
`zephyr/kernel/spinlock_validate.c`) checks a different thing from ordering. It
records an owner thread and CPU id in each lock and catches: same-CPU re-acquire (lines 27-37), release
by the wrong thread or CPU (lines 40-65), a per-CPU hold count that must not go negative, a hold time
over `CONFIG_SPIN_LOCK_TIME_LIMIT` cycles (`zephyr/include/zephyr/spinlock.h` lines 319-325), and -- the
interesting one -- a context switch while holding more than the one lock being swapped out
(`zephyr/kernel/spinlock_validate.c` lines 134-173, whose comment names the `lock(A); lock(B); unlock(A); swap`
case). It does not check order between two locks. Packing the CPU id into the owner word is why the
validator caps the build at 4 CPUs, or 8 on 64-bit.

The lock is taken INSIDE the interrupt mask, and the masking is part of the lock primitive:
`k_spin_lock` calls `arch_irq_lock` first and stores the result in the key it returns, then spins on the
lock word (`zephyr/include/zephyr/spinlock.h` lines 192-226). The key is the saved interrupt state, so the
caller cannot separate the two. On a single-CPU build the lock word disappears and the spinlock IS the
interrupt mask (lines 49-67).

**3. Remote wake.** Inline reach into a shared queue, plus a deferred IPI, with no inbox and no message.
By default there is ONE global ready queue (`zephyr/include/zephyr/kernel_structs.h`
lines 219-221, `zephyr/kernel/include/run_q.h` lines 37-47); only `CONFIG_SCHED_CPU_MASK_PIN_ONLY` moves a queue
into each `struct _cpu` (`kernel_structs.h` lines 160-162, `run_q.h` lines 19-40).

The waking core holds the scheduler lock, enqueues into the shared queue, and calls `flag_ipi`
(`zephyr/kernel/sched.c` lines 218-235). `flag_ipi` sends nothing: it ORs a CPU bitmask into the shared
`_kernel.pending_ipi` word (`zephyr/kernel/smp/ipi.c` lines 127-134). The mask
comes from `ipi_mask_create` (same file, lines 137-220), which without `CONFIG_IPI_OPTIMIZE` returns
all CPUs -- that is, broadcast -- and with it set walks the CPUs and sets a bit only where the thread may
run and the resident thread is preemptible and lower priority. `CONFIG_IPI_OPTIMIZE_IDLE` narrows further
to exactly one idle CPU, preferring the thread's last CPU, and records a reservation so a second waker
does not pile onto the same one. The send happens later, in `signal_pending_ipi`
(`zephyr/kernel/smp/ipi.c` lines 222-255), which drains every bit but the caller's own and calls either
`arch_sched_directed_ipi` or `arch_sched_broadcast_ipi`. Its comment requires the scheduler lock held
across it so the decision and the dispatch are atomic against a concurrent `flag_ipi`. The receiving core
runs `z_sched_ipi` (lines 340-362), which does accounting only; the actual reschedule happens on the
normal interrupt-exit path. On AArch64 the send is a GIC SGI per target
(`zephyr/arch/arm64/core/smp.c` lines 218-259).

The tree carries its own admission that the coalescing is not airtight: `zephyr/kernel/sched.c` lines 652-656 is
a TODO noting that `z_reschedule_irqlock` calls `signal_pending_ipi` holding only the IRQ lock and not
the scheduler lock, which can delay a reschedule.

**4. Migration.** A thread changes core freely, and the mechanism is a PULL rather than a push. With one
shared queue, any core reaching a scheduling point takes the highest-priority eligible thread
(`next_up`, `zephyr/kernel/sched.c` lines 57-158) and stamps its own id into the thread
(`zephyr/kernel/sched.c` lines 744-745, `zephyr/kernel/include/kswap.h` line 119). Nobody sends
the thread anywhere and
nobody asks the previous core. There is no load balancer because there is nothing to balance.

A thread RUNNING on another core cannot be moved, and this is arranged structurally: under SMP a running
thread is deliberately kept OUT of the run queue so no other core can see it as a candidate
(`zephyr/kernel/include/run_q.h` lines 76-95 and the comment at `zephyr/kernel/sched.c` lines
87-97). It is re-added
only once the switch is inevitable (`zephyr/kernel/include/kswap.h` lines 137-148). Operations aimed at a thread
running elsewhere go through the halt or IPI protocol instead (`thread_active_elsewhere`,
`zephyr/kernel/sched.c` lines 199-216).

The affinity API is more restricted than "not while running": `cpu_mask_mod`
(`zephyr/kernel/smp/cpu_mask.c` lines 17-44) permits a change only when
`z_is_thread_prevented_from_running` holds -- pending, sleeping, dead, dummy or suspended
(`zephyr/kernel/include/kthread.h` lines 118-124) -- and returns `-EINVAL`
otherwise. A merely READY thread cannot have its mask changed. The doc says the same
(`zephyr/doc/kernel/services/smp/smp.rst` lines 132-139). The mask is capped at 32 cores by a build assertion
(`zephyr/kernel/smp/cpu_mask.c` lines 11-14).

**5. Lock across the context switch.** RELEASED INSIDE, one statement before the register switch, with
interrupts still masked. On the cooperative path (`zephyr/kernel/include/kswap.h`
lines 79-174) everything happens under the scheduler lock -- picking the next thread, the switch spin, the
`_current` swap, re-adding the outgoing thread to the queue, dispatching pending IPIs, publishing the
handle -- and then line 161 drops the lock WORD only (`z_sched_spinlock_release` is built on
`k_spin_release`, `zephyr/include/zephyr/spinlock.h` lines 378-391, which pointedly does not restore the
interrupt key) and line 162 calls `arch_switch`. Interrupts are restored only after the thread resumes
(lines 167-171). The interrupt-exit path is the same shape: one locked region from `zephyr/kernel/sched.c` line
727 to 784, the lock released at 784, and the handle then returned to architecture assembly which does
the register switch (`zephyr/arch/arm64/core/isr_wrapper.S` lines 144-159).

This is where Zephyr's characteristic hazard lives, and the tree names it. A thread record becomes
visible to other cores before `arch_switch` has finished saving its state, and locking cannot fix it
because the scheduler lock cannot be released by the switched-to thread
(`zephyr/kernel/include/kswap.h` lines 36-53). The answer is a busy-wait: `z_sched_switch_spin`
(`zephyr/kernel/include/kswap.h` lines 55-69) spins on a volatile read of the target thread's `switch_handle`
until it goes non-NULL. YES -- another core can spin waiting for an outgoing thread to finish saving its
context, AND IT DOES SO WITH THE SCHEDULER SPINLOCK HELD. The comment at lines 36-39 says exactly that,
and argues the wait is bounded because the target has already released the lock and reaches `arch_switch`
in small constant time. Call sites are `zephyr/kernel/include/kswap.h` line 126 and
`zephyr/kernel/sched.c` line 740,
both inside the locked region. The AArch64 side is explicit about the ordering constraint: FPU and PAC
state must be saved before the outgoing handle is published because the old stack is still in use
(`zephyr/arch/arm64/core/switch.S` lines 66-70), followed by a write barrier
and the handle store, whose comment names `z_sched_switch_spin` as the reason (lines 86-99).

**6. Kernel stack model.** One kernel stack per thread, plus one interrupt stack per core. No
continuations, no saved operation state: a blocked thread's kernel state is its own suspended C frame
plus the callee-saved registers in its TCB
(`zephyr/arch/arm64/core/switch.S` lines 35-62, 101-127). The interrupt stacks
are an array dimensioned by `CONFIG_MP_MAX_NUM_CPUS`
(`zephyr/kernel/init.c` lines 129-144), each core's top pointer stored in its
`struct _cpu` (`zephyr/kernel/init.c` lines 396-398), and the same array serves as the bring-up stack for
secondary cores (`zephyr/kernel/smp/smp.c` lines 159-160). Under `CONFIG_USERSPACE` a user thread's stack object
carries a privileged region at its top that the MMU marks not user-accessible, and exception or syscall
entry switches onto it
(`zephyr/arch/arm64/core/thread.c` lines 20-42, 172-178). A dummy thread exists
purely to complete a switch out of an invalid context (`zephyr/kernel/sched.c` lines 38-41).

**7. Refusal, blocking, PI, fairness.**
- A wake cannot be refused. The run queue node lives inside the thread, so enqueue cannot fail, and
  `flag_ipi` is an idempotent atomic OR into a fixed-width bitmask. The cases where `ready_thread` does
  nothing (`zephyr/kernel/sched.c` lines 227, 239-243) are correctness filters, not backpressure. The one
  refusable thing nearby is the separate IPI work-item facility, where `k_ipi_work_add` returns `-EBUSY`
  if the caller's item is still outstanding (`zephyr/kernel/smp/ipi.c` lines 266-280) -- one in-flight item per
  object, and not on the thread-wake path.
- Cross-core paths that block on a peer: `z_sched_switch_spin`, described above, which spins with the
  scheduler lock held; and `k_thread_abort` or `k_thread_suspend` aimed at a thread running elsewhere
  (`zephyr/kernel/sched.c` lines 307-374), which sends a synchronous IPI and then either spins in ISR context
  until the target clears its halting bits or pends itself on the target's halt queue and swaps away, then
  spins outside all locks until the target's timeout is known cancelled. The doc adds that without a
  usable IPI the fallback is to spin until the foreign core takes any interrupt at all
  (`zephyr/doc/kernel/services/smp/smp.rst` lines 256-266).
- Priority inheritance crosses cores. `zephyr/kernel/mutex.c` implements chain PI, and the boost is applied
  through `z_thread_prio_set` (`zephyr/kernel/sched.c` lines 587-641), which handles the case of a
  thread running
  on another core by updating its priority in place and flagging an IPI at that specific core (lines
  604-620). Because the queue and the lock are global, a boost on core A reaches a thread on core B.
  Three bounds the tree states about itself (`zephyr/kernel/mutex.c` lines 29-50): the chain walk stops after 16
  hops and owners past that keep their pre-boost priority (lines 114-118); on a waiter timeout the
  owner's priority is corrected in the resuming waiter's context rather than at ISR level, so the owner
  runs over-boosted briefly, and the stated reason is the SMP lock-ordering constraint; and that
  correction is not propagated down the chain, so deeper owners stay over-boosted until they release.
  The mutex documentation
  (`zephyr/doc/kernel/services/synchronization/mutexes.rst` lines 62-96)
  describes the algorithm and the hop limit but does not discuss multicore at all.
- Fairness: none by default, and the project says so in its own words. The default acquire is a CAS on a
  single atomic word with a relax loop (`zephyr/include/zephyr/spinlock.h` lines 216-220), and
  `zephyr/doc/kernel/services/smp/smp.rst` lines 75-80 states that it does not guarantee fairness and that one
  core can repeatedly win and starve others; `zephyr/kernel/smp/Kconfig` lines 169-180 goes further and says
  this can live-lock. A FIFO ticket lock exists as `CONFIG_TICKET_SPINLOCKS`
  (`zephyr/include/zephyr/spinlock.h` lines 50-63, 205-214, 329-331) but is marked EXPERIMENTAL and is off by
  default. There is no MCS implementation in the tree.
- The spin body calls `arch_spin_relax`, whose interface contract says the default is a weak function
  that does nothing but a nop (`zephyr/include/zephyr/arch/arch_interface.h`
  lines 1508-1516). On AArch64 it is not a pause at all: it checks for a pending FPU-flush IPI and
  services it (`zephyr/arch/arm64/core/smp.c` lines 304-321). The comment says
  why -- without it, a core can deadlock waiting for a lock whose holder is another core waiting for this
  core's live FPU content. So on this architecture the lock WAIT is a point at which the waiter must
  execute work on behalf of a peer.

**8. Published benchmark evidence.** Harnesses yes, published SMP numbers no.
`zephyr/tests/benchmarks/` holds twenty harnesses. Two are SMP-specific:
`ipi_metric/` under it, the only one dedicated to SMP mechanics, which measures IPI count, wake round trips and
average and maximum wake latency in cycles across six configurations covering broadcast versus directed
IPI and the two `IPI_OPTIMIZE` modes (`zephyr/tests/benchmarks/ipi_metric/tests.yaml` lines 18-23 and the three
sources beside it); and `sched_cpu_mask/`, which measures affinity-API latency and a two-core wakeup
round trip with the run-queue depth varied from 0 to 16 to expose the per-backend cost of the mask
filtering scan (`zephyr/tests/benchmarks/sched_cpu_mask/src/main.c` lines 6-38,
`zephyr/tests/benchmarks/sched_cpu_mask/tests.yaml` lines 9-43). Neither commits reference numbers.

The only numeric results committed to the tree are the sample tables in
`zephyr/tests/benchmarks/latency_measure/README.rst` lines 58-108 and from
line 111, in cycles and nanoseconds. Every row there is an intra-core operation -- context switch by
yield, ISR return, semaphore, mutex, fifo, lifo, event, stack, heap, thread lifecycle -- and the
thread-to-thread rows are all same-core. The harness contains no SMP-specific code, although its
`tests.yaml` does list an SMP QEMU platform, so the same uniprocessor-shaped measurements also get run on
an SMP build. `zephyr/doc/develop/test/benchmark.rst` defines the
infrastructure and publishes no figures, and no other document under `zephyr/doc/` publishes SMP or IPC numbers.

## NuttX

**Licence** Apache-2.0, stated in `nuttx/LICENSE` with attribution in
`nuttx/NOTICE`, and carried per file as an SPDX tag (every one of the 52
tagged files under `nuttx/sched/sched/` declares Apache-2.0).
**Revision** `f0c94bb6`, `git describe` reports `nuttx-8.2-26628-gf0c94bb64a`, dated 2026-07-01.

**1. Lock domains.** Two tiers, and neither "one big lock" nor a fully partitioned set. Tier one is a
single distinguished global lock, `g_cpu_irqlock`
(`nuttx/sched/irq/irq_csection.c` line 62, declared in
`nuttx/sched/sched/sched.h` line 294), taken by `enter_critical_section`
(`irq_csection.c` lines 92-254) and released by `leave_critical_section` (lines 335-433). It covers every
task list and every scheduler state transition: the ready-to-run list, the per-core assigned-task array,
the blocked lists, and TCB priority and affinity fields. Ownership is tracked in `g_cpu_irqset`
(`irq_csection.c` line 66) and a per-core nesting count (line 70).

The distinguishing fact: the lock is owned by a THREAD, not by a core. The owning count lives in the TCB
field `irqcount` (`nuttx/include/nuttx/sched.h` line 628), so ownership
survives a context switch. That single choice determines point 5.

Tier two is roughly eighteen independent spinlocks in `nuttx/sched/`, each protecting only its own structure
and ordered against nothing: the IRQ vector table (`nuttx/sched/irq/irq_attach.c` line 39), the shared-IRQ
chain (`nuttx/sched/irq/irq_chain.c` line 54), the cross-call queues
(`nuttx/sched/sched/sched_smp.c` line 54), the
critical-section monitor, the message free list, the POSIX timer list, three clock locks, the sigaction
free list, address environments, the child-status pool, the work-queue notifier, and the panic path.
Another forty or so exist in `nuttx/drivers/`, `nuttx/fs/`, `nuttx/net/`, `nuttx/mm/`,
`nuttx/libs/` and `nuttx/binfmt/`. Four lock flavours
are available (`nuttx/include/nuttx/spinlock_type.h`): plain, recursive,
reader/writer under `CONFIG_RW_SPINLOCK`, and a seqcount.

Two things the design documentation describes do NOT exist in the code at this revision, and both were
verified by whole-tree grep. `g_cpu_tasklistlock`, the per-task-list lock, appears exactly ONCE in the
entire repository -- as an `extern` declaration at `nuttx/sched/sched/sched.h`
line 302, with no definition and no use anywhere. And `g_cpu_schedlock` and `g_cpu_lockset`, the
per-core scheduler lock described in
`nuttx/Documentation/implementation/smp.rst`, return zero hits in `.c` and
`.h` files. `sched_lock()` is a per-TCB counter, `lockcount`
(`nuttx/include/nuttx/sched.h` line 626, `nuttx/sched/sched/sched_lock.c` lines
67-98), not a lock. The task lists are still under `g_cpu_irqlock`.

**2. Acquisition order.** None stated, none enforced. No lockdep, no lock-class registration, no ordering
assertion, no documented hierarchy; a tree-wide search for ordering language finds six hits, all in an
ESP32 Wi-Fi adapter and none in `nuttx/sched/`, `nuttx/sched/irq/` or the SMP documentation.

What the tree does state about the two tiers is a warning rather than an order:
`nuttx/Documentation/implementation/critical_sections.rst` says that the
critical section established by `spin_lock_irqsave` is a DIFFERENT critical section from the one
established by `enter_critical_section`, that holding one does not stop another core taking the other,
and that the result may not be the protection the caller expects.

The lock is taken INSIDE the interrupt mask, and the two steps are explicitly composed rather than
fused. The bare `spin_lock` (`nuttx/include/nuttx/spinlock.h` lines 183-198,
226-248) does not touch the mask at all. `spin_lock_irqsave_notrace` (lines 446-455) is a two-line
composition: mask at line 450, then lock at line 452. `enter_critical_section` does the same and says
why -- masking first stops the local core's own interrupt handling from waiting on the lock
(`nuttx/sched/irq/irq_csection.c` lines 105-107, mask at 110, lock at 183 or
234). A third layer exists, `spin_lock_irqsave_nopreempt` (`nuttx/include/nuttx/spinlock.h` lines 540-547):
mask, then lock, then `sched_lock`. The scheduler core uses `enter_critical_section`; the leaf subsystems
use the `spin_lock_irqsave` family.

The only runtime instrumentation here bounds TIME, not order:
`CONFIG_SCHED_CRITMONITOR_MAXTIME_BUSYWAIT` (`nuttx/sched/Kconfig` lines
954-961, default disabled) warns when busy-wait exceeds a threshold.

**3. Remote wake.** Inline reach into a shared list, then a single-slot published hint, then a directed
IPI. There is one global ready-to-run list, `g_readytorun`
(`nuttx/sched/init/nx_start.c` line 95); the per-core array
`g_assignedtasks[]` (line 116) holds only the thread each core is currently RUNNING.

`nxsched_add_readytorun` (`nuttx/sched/sched/sched_addreadytorun.c` lines
251-278) picks a target core -- the pinned one if `TCB_FLAG_CPU_LOCKED` is set, else
`nxsched_select_cpu` -- inserts the TCB into the shared list itself in priority order under the global
critical section (line 264), writes the target core into the TCB (line 268), and if the target's current
thread is lower priority calls `nxsched_deliver_task` (lines 270-274). That inline
(`nuttx/sched/sched/sched.h` lines 541-564) either switches locally, or writes
a single-slot per-core hint `g_delivertasks[target]` -- an enum of none/higher/equal, not a list -- and
sends a directed IPI. On AArch64 that is a GIC SGI aimed at the one core
(`nuttx/arch/arm64/src/common/arm64_smpcall.c` lines 95-101); it returns
immediately and waits for nothing. The receiving core runs `nxsched_process_delivered`
(`nuttx/sched/sched/sched_process_delivered.c` lines 64-118), which takes
`g_cpu_irqlock` directly rather than going through `enter_critical_section` to save time (comment at
lines 45-47), consumes and clears the hint, and switches.

The placement heuristic, `nxsched_select_cpu` (`sched.h` lines 567-605), scans the affinity mask,
returns on the first core running its idle thread, and otherwise picks the core whose current thread has
the lowest priority.

**4. Migration.** Yes, freely, and by two different mechanisms depending on whether the thread is
running. `CONFIG_SMP_NCPUS` defaults to 4 with a range of 1 to 32
(`nuttx/sched/Kconfig` lines 368-371); the affinity mask is a 32-bit word in
the TCB (`nuttx/include/nuttx/sched.h` line 623), and `TCB_FLAG_CPU_LOCKED` (line 101) pins a thread and
overrides the mask.

Because the ready list is shared, placement is re-decided at every wake-up, so a thread migrates between
wake-ups with no explicit migration step, and again at dispatch time on the target, where
`nxsched_switch_running` (`sched_addreadytorun.c` lines 145-206) walks the shared list for the highest
priority entry THIS core is allowed to run and claims it. There is also an explicit affinity call
(`nuttx/sched/sched/sched_setaffinity.c`), which refuses on a CPU-locked
thread and, when the current core drops out of the new mask, re-queues the thread by reusing the
reprioritise path. There is no load balancer; a search for one across `nuttx/sched/` finds a single hit, and it
is aspirational prose in the design document.

Yank or ask depends on the state. A thread that is not running sits in the shared list and the deciding
core edits it directly under the global lock -- a yank of shared state. A thread that IS running on
another core cannot be pulled off: `nxsched_remove_running` asserts the thread belongs to the calling
core (`nuttx/sched/sched/sched_removereadytorun.c` lines 141-142). The
deciding core must instead ask, either by mutating the shared fields and delivering a reschedule hint
(`nuttx/sched/sched/sched_setpriority.c` lines 94-98) or by a blocking
cross-call that makes the owning core perform the operation on itself -- suspend pins the victim with
`TCB_FLAG_CPU_LOCKED` first and then calls across
(`nuttx/sched/sched/sched_suspend.c` lines 168-185). Task restart, task
terminate, signal dispatch and backtrace use the same ask.

**5. Lock across the context switch.** Neither held nor released unconditionally: the switch RE-EVALUATES
ownership, because the lock belongs to a thread rather than to a core. The hook is
`restore_critical_section(tcb, cpu)` (`nuttx/include/nuttx/irq.h` lines
408-418), which inspects the INCOMING thread's `irqcount` and, if it is not an owner while this core's
bit is set, clears the core bit and unlocks (`nuttx/include/nuttx/irq.h` lines 99-107). On a single-core build
it compiles to nothing. On AArch64 it is called from
`nuttx/arch/arm64/src/common/arm64_syscall.c` lines 196 and 218, and every
other SMP-capable port has the same call in its switch path. The acquire side matches: a resuming thread
whose `irqcount` is already positive just bumps the count on its next `enter_critical_section`
(`nuttx/sched/irq/irq_csection.c` lines 209-222) instead of re-taking the
spinlock. The IPI-driven switch does the same release at the end of `nxsched_process_delivered`
(`sched_process_delivered.c` lines 115-118).

So a thread that entered the critical section and then blocked has the lock dropped while it is off-core,
and re-established when it comes back. On AArch64 the switch is deliberately funnelled through an
exception (`nuttx/arch/arm64/include/irq.h` lines 467-476) so that it lands
where this handling lives.

**6. Kernel stack model.** One kernel stack per thread in kernel and protected builds, plus one
interrupt stack per core, which is MANDATORY under SMP. `CONFIG_SMP` depends on a non-zero interrupt
stack (`nuttx/sched/Kconfig` line 348), and the help text at lines 355-364
gives the reason: an interrupt handler running on a thread's stack would corrupt it if that thread
migrated to another core mid-interrupt. That is a multicore constraint on the stack model that does not
exist on one core. On AArch64 the stacks are `g_interrupt_stacks[CONFIG_SMP_NCPUS][INTSTACK_SIZE]`
(`nuttx/arch/arm64/src/common/arm64_initialize.c` lines 62-63) with a
top-of-stack table (`arm64_cpustart.c` lines 59-76). The exception frame is pushed on the INTERRUPTED
stack before the switch, so the register context lands on the thread's own stack and only the handler
body runs on the per-core stack
(`nuttx/arch/arm64/src/common/arm64_vectors.S` lines 225-243). Per-thread
kernel stacks under `CONFIG_ARCH_KERNEL_STACK` are one allocation per TCB
(`nuttx/arch/arm64/src/common/arm64_addrenv_kstack.c` lines 64-107). In flat
builds there is no separate kernel stack. Context is a saved register frame, not a continuation and not
saved operation state.

**7. Refusal, blocking, PI, fairness.**
- A wake cannot be refused: the TCB is in the shared ready list before the IPI is sent, so the thread is
  runnable whatever the target does. The hint slot COALESCES rather than queues -- one enum per core,
  skipped entirely if it already holds the requested value (`sched.h` line 551) -- so there is no full
  condition, but also no per-thread delivery accounting. Separately, the target may DECLINE to switch
  (it just took `sched_lock`, or already picked something higher), and the code handles that by
  re-examining the head of the shared list and FORWARDING the delivery to a better core
  (`sched_process_delivered.c` lines 90-114). That forwarding is a distinctive shape: the wake is never
  lost, but which core answers it is not settled at send time.
- Cross-core paths that block on a peer: the ordinary wake does not, and the pause/resume handshake that
  older NuttX used is GONE from this revision -- `up_cpu_pause` has no definition and no caller anywhere
  in the tree, its single remaining occurrence being a stale comment in a SPARC file. What does block is
  the separate cross-call facility: `nxsched_smp_call` enqueues a request on each target's queue, fires
  an IPI, and then waits on one semaphore per target
  (`nuttx/sched/sched/sched_smp.c` lines 204-249). Its queue is intrusive with
  the node in the caller's own request object, so it cannot be full; a de-duplication check stops the
  same object being double-queued. Note that several callers make this blocking call from INSIDE the
  global critical section, for instance suspend, which takes the section at `sched_suspend.c` line 112
  and blocks on the peer at line 184. The panic path is the one place with a bounded wait, polling with a
  countdown and giving up (`nuttx/sched/misc/assert.c` lines 633-655).
- Priority inheritance exists and crosses cores, but is OFF by default: `CONFIG_PRIORITY_INHERITANCE`
  (`nuttx/sched/Kconfig` line 1390) defaults to n. The implementation is
  `nuttx/sched/semaphore/sem_holder.c`, with per-TCB boost and base priority
  and a held-semaphore list (`nuttx/include/nuttx/sched.h` lines 615-617). It crosses cores because boost and
  restore both go through `nxsched_set_priority`
  (`nuttx/sched/sched/sched_setpriority.c` lines 261-313), which runs inside
  the global critical section and whose running-thread branch is written for the remote case: it writes
  the new priority and delivers a reschedule hint to the holder's core (lines 94-98). There is no
  SMP-specific PI path and no documented restriction to one core. The number of tracked holders is a
  compile-time bound (`CONFIG_SEM_PREALLOCHOLDERS`, `nuttx/sched/Kconfig` line 1413, default 4 or 8).
- Fairness: none by default. `spin_lock_notrace` (`nuttx/include/nuttx/spinlock.h` lines 183-198) has two
  bodies. Under `CONFIG_TICKET_SPINLOCK` it draws a ticket and spins until served -- FIFO, bounded by the
  number of contenders. The default body is a bare retry loop on a test-and-set with no queue, no ticket,
  no backoff and no bound. `CONFIG_TICKET_SPINLOCK` defaults to n (`nuttx/sched/Kconfig` lines 286-290), and
  `CONFIG_SMP` selects `SPINLOCK` but NOT `TICKET_SPINLOCK` (line 349), so a default SMP build gets the
  unfair one -- including for `g_cpu_irqlock`, which is taken with the same primitive. The wait-for-event
  and send-event instructions in the spin body are a power optimisation, not fairness: all waiters wake
  and race again. The design document raises the resulting starvation question as an open issue rather
  than answering it (`nuttx/Documentation/implementation/smp.rst`, in the region discussing whether one core
  can hog the lock).

**8. Published benchmark evidence.** None for SMP and none for IPC. There is no SMP scalability number,
no cross-core wake latency, and no context-switch cost anywhere under
`nuttx/Documentation/`. The two SMP documents are prose:
`nuttx/Documentation/implementation/smp.rst` (872 lines) and
`nuttx/Documentation/reference/os/smp.rst` (99 lines).
Both are materially out of date against this revision -- they document `up_cpu_pause`, `up_cpu_resume`,
`g_cpu_lockset`, `g_cpu_schedlock` and per-core assigned-task QUEUES, none of which exist in the code,
while the first also claims to reflect the as-built state.

The kernel tree contains no test or benchmark harness source at all; the programs are in the separate
`nuttx-apps` repository and only documented here. The `smp` example is described in two sentences as
essentially the pthread barrier test with instrumentation
(`nuttx/Documentation/applications/testing/smp/index.rst`); `ostest` says of itself that its coverage is not
very extensive (`nuttx/Documentation/applications/testing/ostest/index.rst` lines 7-9); the mutex performance
entry describes a method and publishes no values, stating it exists to catch regressions rather than to
validate the mutex; and the `osperf` system-profiling entry is a bare title stub with no content. The one
published measurement in the tree is a cyclictest histogram with minimum, average and maximum wakeup
latency on a single-core Cortex-M7 part
(`nuttx/Documentation/applications/benchmarks/cyclictest/index.rst`), which is neither SMP nor IPC.

## RTEMS

**Licence** A deliberate multi-licence collection, with BSD-2-Clause as the preferred licence for code.
`rtems/LICENSE.md` lines 1-38 enumerate BSD-2-Clause, the legacy "RTEMS
License" (GPL-2.0 plus a linking exception), GPL-2.0 as the basis of that and of JFFS2, BSD-3-Clause,
SLAC, CC-BY-SA-4.0, Apache-2.0 and Freescale, and lines 12-14 name BSD-2-Clause "the most common and
preferred license in RTEMS"; CC-BY-SA-4.0 is the preferred licence for documentation (lines 26-29). The
code that matters here is uniform: of the SPDX tags under `rtems/cpukit/score/src/` and
`rtems/cpukit/include/rtems/score/`, 376 are BSD-2-Clause and the only others are one BSD-2-Clause-FreeBSD and
one Beerware. There is no GPL in the score.
**Revision** `f35edbdb`, `git describe` reports `rtems/build/2026-03-04-333-gf35edbdb95`, dated 2026-09-04.
Note the layout: the score headers are at `rtems/cpukit/include/rtems/score/`,
not under `rtems/cpukit/score/include/`.

**1. Lock domains.** Partitioned, with no giant lock, and the lock SET SCALES with the configuration
rather than being a fixed number. A search of `rtems/cpukit/` for the old giant lock returns zero hits; it was
removed in 2016.

The mechanisms are few. `SMP_ticket_lock_Control` is the raw ticket lock, two atomics
(`rtems/cpukit/include/rtems/score/smplockticket.h` lines 60-63).
`SMP_lock_Control` wraps it with a debug owner index and profiling statistics
(`rtems/cpukit/include/rtems/score/smplock.h` lines 86-119).
`ISR_lock_Control` wraps that again and degrades to NOTHING on a single-core build, where the operations
become plain interrupt disable and enable
(`rtems/cpukit/include/rtems/score/isrlock.h` lines 47-106). An MCS lock and a
sequence lock exist as headers but are unused by the kernel; their only consumers are the build spec and
a benchmark, and `smplock.h` lines 57-59 says the lock context type exists so MCS could be adopted later.

The domains are per-instance, not per-subsystem:
- Three locks per processor in `Per_CPU_Control`
  (`rtems/cpukit/include/rtems/score/percpu.h` lines 384-641): one for the
  lock context and the help chain (line 520), one for that processor's watchdog (line 499), one for its
  job list (line 604). Four per-CPU fields are DELIBERATELY not lock-protected and say so: the dispatch
  flag, whose comment tells other processors to use an IPI rather than touch it (lines 430-437), the
  executing and heir pointers, and the message word, which is atomic-only (lines 538-544).
- One per scheduler instance (`scheduler.h` lines 318-332).
- One per thread queue, that is per synchronisation object -- a bare ticket lock rather than the wrapped
  form, so the layout can be reused for Newlib's lock structures
  (`rtems/cpukit/include/rtems/score/threadq.h` lines 433-469).
- FOUR independent locks inside each thread
  (`rtems/cpukit/include/rtems/score/thread.h`): a state lock whose
  documentation enumerates exactly what it covers (lines 841-855), a wait default lock with its own
  enumeration (lines 492-514), a scheduler lock for pending node requests (line 302), and a timer lock
  (line 568).
- One per scheduler node, but only where a pointer is not 8 bytes; on 64-bit the priority is a single
  atomic and needs no lock at all
  (`rtems/cpukit/include/rtems/score/schedulernode.h` lines 217-226).

A handful of genuinely global locks remain, all narrow. One is worth naming because the tree is candid
about it: the thread-queue link registry used for deadlock detection, whose comment admits it is not
scalable and may contend, justified by being entered only on nested resource conflicts
(`rtems/cpukit/score/src/threadqenqueue.c` lines 64-82).

**2. Acquisition order.** No global order is stated IN THIS TREE. A search for ordering language across
the whole checkout finds one hit, and it is inside third-party ACPICA code. There is no `rtems/cpukit/doc`
here and the `rtems/spec/` tree is build YAML only; the RTEMS Software Engineering manual lives in a separate
repository that is not checked out. So the premise that RTEMS documents its lock nesting explicitly is
NOT verifiable from this checkout -- the documentation may well say so, but this tree does not.

What the code does carry is three local rules, and the third is the interesting one:
1. A stated total order on the per-processor locks: acquire in ascending processor index, release in
   descending, with each acquire using the previous processor's lock context
   (`percpu.h` lines 754-802).
2. An avoided order rather than a stated one. The thread wait lock and the thread queue lock are never
   held together. `_Thread_Wait_acquire_critical`
   (`rtems/cpukit/include/rtems/score/threadimpl.h` lines 1940-1987) takes the
   wait lock, reads the queue, and if the thread is enqueued it registers a GATE on a pending-request
   chain, RELEASES the wait lock, then takes the queue lock and re-validates. The gate type is documented
   at `threadq.h` lines 124-140: requests queue up and each predecessor notifies its successor. Hand-off
   instead of nesting.
3. A runtime cycle detector for the one lock graph that cannot be ordered statically. Locks along a
   resource dependency path are acquired by walking queue -> owner's wait lock -> that owner's queue,
   again by gate hand-off (`rtems/cpukit/score/src/threadqenqueue.c` lines
   271-381). The comment at lines 285-291 states the requirement bluntly: since every thread queue has
   its own lock, deadlock at SMP-lock level must be avoided because it would be an unrecoverable deadlock
   of the whole system. A cycle is found through the global link registry and reported as a detected
   deadlock, raising an internal error (lines 131-174, 400-409). A worked three-thread interleaving is
   written out at `rtems/cpukit/score/src/threadqops.c` lines 1240-1266.
   This is a checker for one graph, not a general lock-order validator; the only other checks are
   per-lock owner assertions under debug builds (`smplock.h` lines 246-299, `isrlock.h` lines 321-336).

The lock is taken INSIDE the interrupt mask, and the tree offers both a fused and an unfused form. The
fused `_SMP_lock_ISR_disable_and_acquire_inline` masks first, saving the level into the lock context,
then spins (`smplock.h` lines 321-328). The bare variants do not touch interrupts and instead ASSERT a
non-zero ISR level before taking the lock (`isrlock.h` lines 250-261, 290-304), with the header
explaining that a higher-priority interrupt entering the same critical section is undefined behaviour
(lines 234-249). Both lock layers document that the caller must guarantee the holder is not interrupted
indefinitely once the lock is held (`smplock.h` lines 266-277, `smplockticket.h` lines 153-163).

**3. Remote wake.** Inline reach into the peer's scheduler state, then a bare IPI carrying no message.
Three things are worth separating.

The scheduler state is written directly. Holding the scheduler INSTANCE lock, a core writes the target
processor's heir pointer itself
(`rtems/cpukit/include/rtems/score/schedulersmpimpl.h` lines 730-793 ->
`_Thread_Dispatch_update_heir`, `threadimpl.h` lines 1282-1294). `percpu.h` lines 459-473 documents that
after multitasking start the only writer of the heir field is the scheduler owning that processor, and
that pointer stores are assumed atomic on every supported SMP architecture.

The notification is an IPI with NO payload. `_Thread_Dispatch_request`
(`rtems/cpukit/include/rtems/score/threaddispatch.h` lines 270-286) sets the
local flag for a local target; for a remote one it performs an atomic fetch-or on the peer's message word
WITH A ZERO MASK -- purely a release fence, setting no bit -- and then sends the interrupt
unconditionally, without checking the target's state. The receiver sets its own dispatch flag FIRST,
because that is the common case, and only then exchanges the message word to zero and processes it if
non-zero (`rtems/cpukit/include/rtems/score/smpimpl.h` lines 202-223). So the
IPI itself is the signal.

The bitfield does exist, for other message kinds, and it is posted atomically into the peer's per-CPU
control (`percpu.h` line 544, `smpimpl.h` lines 63-80). At this revision exactly three bits are defined:
shutdown, perform-jobs, and a force-processing bit documented as never actually sent, existing only to
force the fetch. There is NO clock-tick message at this revision -- a whole-tree grep returns zero hits.

**4. Migration.** Two levels, and they behave differently. A SCHEDULER INSTANCE owns a set of processors
(`scheduler.h` line 328), and a THREAD is bound to a home instance, carrying one scheduler node per
instance it may use. Within an instance a thread moves freely and it is a YANK issued remotely: any core
executing scheduler code for that instance may rewrite another core's heir pointer and fire an IPI at
it, and the victim is not consulted and cannot decline. The decision is remote, the execution is local --
the victim core runs its own dispatch and its own context switch. Across instances it is an ASK and it
can FAIL: `rtems_task_set_scheduler` goes through `_Scheduler_Set`
(`rtems/cpukit/include/rtems/score/schedulerimpl.h` lines 930-1030), which
refuses with a resource-in-use status if the thread is enqueued on a thread queue, holds more than one
wait node, or is pinned. Processors themselves can also be moved between instances at run time,
serialised by the object allocator mutex.

Affinity within an instance is a separate mask, and notably the DEFAULT scheduler operation does not
honour it at all -- it merely rejects a set that is not a subset of the online processors
(`schedulerimpl.h` lines 636-652). Real affinity exists only in the affinity-aware SMP schedulers
(priority-affinity, strong APA, and EDF).

**5. Lock across the context switch.** Not a lock at all, and my brief's premise was wrong. No
`SMP_lock` or `ISR_lock` is held across the switch. Two other things are.

Interrupts are RE-ENABLED immediately before the switch and re-disabled after it
(`rtems/cpukit/score/src/threaddispatch.c` lines 324, 330, 343), so the switch
runs with interrupts on and no lock held. What is carried across is the per-processor thread-dispatch
disable counter, which is at 1 on entry and is zeroed at line 352 by WHICHEVER THREAD IS NOW RUNNING, on
whichever processor it now runs on. The processor pointer is deliberately re-read after the switch
because the heir may have migrated and stack and callee-saved values reflect the old environment (comment
at lines 336-341). The same hand-over happens on a thread's very first entry, where `_Thread_Handler`
calls dispatch precisely to drop the level from one to zero
(`rtems/cpukit/score/src/threadhandler.c` lines 141-149).

The other thing is a genuine one-slot lock, and it is per THREAD CONTEXT rather than per core: a volatile
`is_executing` flag inside each thread's saved register context. The contract is written out as
pseudo-code for port authors in
`rtems/cpukit/score/cpu/no_cpu/include/rtems/score/cpu.h` lines 370-415 -- the
outgoing context clears its own flag, a barrier follows, and the incoming context is claimed by
test-and-set; if the claim fails the switching core loops, and INSIDE THAT LOOP it re-reads its own
executing and heir pair and may retarget to a newer heir before trying again. The AArch32 implementation
matches exactly: the claim loop uses load-exclusive and store-exclusive on a byte, with a retarget path
that reloads the per-CPU control and recomputes the heir context before jumping back
(`rtems/cpukit/score/cpu/arm/cpu_asm.S`, claim loop at line 100, retarget at
line 160). So ownership of a thread's register context IS a spinlock, RELEASED by the outgoing core right
after saving registers and ACQUIRED by the arriving core -- the opposite direction from a lock handed
forward. During the hand-over the switching core parks its stack pointer on the per-processor interrupt
frame area.

The blocking path is explicit about giving the lock up early: `_Thread_queue_Enqueue`
(`rtems/cpukit/score/src/threadqenqueue.c` lines 411-496) raises the dispatch
disable level and THEN releases the queue lock, and the comment at lines 476-484 states the consequence
plainly -- from that point the lock is gone, other processors may already have satisfied or timed out the
request, and the thread must cancel its own blocking operation using the wait flags if so.

**6. Kernel stack model.** One stack per thread, plus one interrupt stack per processor. There is no
kernel/user split at all -- RTEMS is a single address space -- so there is no separate kernel stack, no
continuation and no saved operation state. The thread stack is described by a size and a low address
(`rtems/cpukit/include/rtems/score/stack.h` lines 66-76) with a per-thread free
callback, allocated through configurable allocator hooks. The interrupt stacks are per-processor bounds
in the per-CPU control (`percpu.h` lines 395-400, with assembly offsets at lines 943-950) carved from one
contiguous region whose size depends on the configured processor count
(`rtems/cpukit/include/rtems/score/isr.h` lines 108-134). On SMP there is
additionally a per-processor interrupt frame area used as the temporary stack during the context-switch
hand-over described in point 5 (`percpu.h` line 476), and a context-switched flag recording that a thread
has an interrupt frame on its own stack so nested dispatches are not stacked on it (lines 409-416).

**7. Refusal, blocking, PI, fairness.**
- A wake cannot be refused, and there is no bounded queue anywhere on the path. The message word is a
  single machine word written by fetch-or, so concurrent sends coalesce idempotently and cannot overflow;
  a plain dispatch request does not even set a bit. The only failure in the area is a STATE error, not a
  capacity one: submitting a job to a processor in a state that cannot run jobs is fatal
  (`rtems/cpukit/score/src/percpujobs.c` lines 102-131).
- A thread wake never blocks the waker. A separate facility does: the per-processor job list, used for
  cross-core maintenance, where `_SMP_Multicast_action` issues one job per target and then busy-waits for
  every one (`rtems/cpukit/score/src/smpmulticastaction.c` lines 71-108). Two
  details there are worth carrying: while waiting, the waiter keeps servicing its OWN messages so the
  system still progresses even if IPIs are not working (`percpujobs.c` lines 107-130) -- the same
  shape seen in seL4's lock spin loop and Zephyr's AArch64 spin relax; and the job storage is a
  caller-supplied array on the caller's stack, one slot per processor, so there is no shared queue to
  fill.
- There is also a DEFERRED LOCAL inbox, and its justification is directly relevant: the scheduler helping
  protocol appends a thread to the CURRENT processor's help chain rather than to one related to the
  thread, and the comment at `schedulersmpimpl.h` lines 608-620 says why -- identifying a processor
  related to the thread is hard, and doing so would need an IPI (`schedulersmpimpl.h` lines 622-641,
  drained in `threaddispatch.c` lines 176-213).
- Priority inheritance crosses cores AND crosses scheduler instances. The mechanism is that a thread
  queue keeps one priority queue PER SCHEDULER INSTANCE
  (`threadq.h` lines 397-431, whose comment states the goal as FIFO fairness among the highest-priority
  thread of each instance), and inheritance adds a scheduler node of the WAITER's instance to the OWNER
  thread via posted add/remove requests under the owner's scheduler lock
  (`threadimpl.h` lines 1684-1720,
  `rtems/cpukit/score/src/threadscheduler.c` lines 75-127). The owner may then
  be dispatched by any of those instances -- "helping"
  (`threaddispatch.c` lines 136-173, and the thread-state documentation at `thread.h` lines 275-284,
  which says a thread may execute using a scheduler node of another thread). The holder is NOT forced
  onto the waiter's core; it gains an additional right to be scheduled on the waiter's instance. MrsP
  (`rtems/cpukit/include/rtems/score/mrsp.h` lines 49-70, citing Burns and
  Wellings 2013) is the other protocol: one ceiling priority per scheduler instance, waiters SPIN rather
  than release their processor, and a preempted owner can borrow the right to execute on a waiter's
  processor. Its waiters use a sticky enqueue that busy-waits on the wait flags instead of blocking
  (`threadqimpl.h` lines 959-986). One naming caveat: OMIP appears exactly once in the tree, in a feature
  list (`rtems/cpukit/doxygen/mainpage.h` line 118), with no file named for it
  and no comment mapping it to an implementation; the mapping onto the per-instance queues plus helping is
  an inference, not a quoted claim.
- Fairness: yes, by construction and by stated intent. The SMP lock IS a ticket lock, and `smplock.h`
  lines 52-59 says so and says it is for fairness under concurrent attempts. The implementation draws a
  ticket with a relaxed fetch-and-add and spins on an acquire-ordered load until served
  (`smplockticket.h` lines 103-151, release at 178-197). The wait is bounded in strict FIFO order: a core
  holding ticket k waits behind at most the holders ahead of it and cannot be overtaken. Two honest
  limits: the bound is in NUMBER OF PREDECESSORS, not in time -- the wall-clock bound is set by the
  longest critical section, which is exactly why both lock layers require that the holder not be
  interrupted indefinitely -- and the counters are plain unsigned ints that wrap, harmless for equality
  but modular for the profiling queue-length figure. Every wrapped lock, and so every ISR lock and every
  thread queue lock, inherits this.

**8. Published benchmark evidence.** This is the one tree of the five that commits NUMBERS, tied to named
hardware, along with the scripts that interpret them.
- `rtems/testsuites/smptests/smplock01/` is a direct SMP lock benchmark. Its
  `.doc` names the benchmarked operations and records the platform as a PowerPC QorIQ P1020E at 800 MHz,
  and the committed `smplock01.scn` holds a 239-line JSON block of per-lock-type, per-core operation
  counts across one to four cores, for the ticket lock and the MCS lock among others. Two analysis
  scripts ship beside it, `smplock01perf.py` and `smplock01fair.py` -- the second computes a normalised
  coefficient of variation across the per-core counters, so the project ships its own FAIRNESS metric for
  its own lock.
- `rtems/testsuites/tmtests/` is the Classic API timing suite, 44 entries,
  whose README states the purpose as measuring directive execution times for comparison across versions,
  boards and architectures, averaged over at least 100 invocations, with a caveat about timer resolution.
  `tmcontext01` publishes context-switch cost by call nesting depth under hot-cache and dirty-cache
  conditions, recorded on a GR740/LEON4 four-core simulation, and ships a plotting script. `tmfine01`
  publishes event, mutex and message operation counts for one to four cores including a contested-mutex
  row.
- `smpmigration01` and `smpmigration02` publish cycle counts, per-core token counts and per-core cycle
  deviations, again on the recorded P1020E.
- The generated validation suite carries seven performance test cases driving a measurement harness whose
  reporting backend emits, per measurement, the sample count, minimum, first percentile, all three
  quartiles, ninety-ninth percentile, maximum and median absolute deviation in nanoseconds, optionally
  with every sorted sample and a load sweep
  (`rtems/cpukit/libtest/t-test-rtems-measure.c` lines 378-467, 771-772).
- There is also a runtime profiling API rather than a test
  (`rtems/cpukit/include/rtems/profiling.h` lines 55-74, 320), which reports
  the maximum duration of disabled thread dispatching as a thread dispatch latency measure and, on SMP,
  statistics for every lock in the system: maximum acquire time, maximum section time, contention counts
  and total section time, with the instants precisely defined
  (`rtems/cpukit/include/rtems/score/smplockstats.h` lines 58-136).
- The bulk of the 63-entry SMP suite is pass/fail, including the IPI flood test and the MrsP test whose
  `.doc` includes ensuring the helping protocol works.
- No design document, manual or SMP whitepaper is in this checkout; the prose lives in separate
  repositories that are not present.

## ThreadX

Licence MIT, read from `threadx/LICENSE.txt` and repeated as an SPDX
line in every source file (for example `ports_smp/cortex_a53_smp/gnu/src/tx_thread_smp_protect.S`
lines 1-10). Revision `44d7c95c`, re-derived with `git -C threadx
rev-parse --short=8 HEAD`; `git describe` reads `v6.5.1.202602a_rel-1-g44d7c95c`. Paths below are
relative to that tree.

**1. Lock domains.** Two, and only one of them is global.

- A single kernel-wide protection structure `_tx_thread_smp_protection`
  (`common_smp/inc/tx_thread.h:182`): a flag word, the owning core id and a recursion count. It
  covers everything the SMP kernel touches: the priority lists, the per-core execute list
  `_tx_thread_execute_ptr` (`common_smp/inc/tx_thread.h:175`), every object queue, the timer state.
  There is no partitioning by subsystem.
- A per-TCB claim bit and claim lock, used only by the assembly scheduler
  (`ports_smp/cortex_a53_smp/gnu/src/tx_thread_schedule.S:117-155`, reset at
  `tx_thread_system_return.S:160-166`, `tx_thread_context_restore.S:284`,
  `tx_thread_stack_build.S:151`). The C kernel never touches it; grepping `common_smp/` for the
  ready bit returns nothing. Its job is the window in which a thread has been picked for a core but
  its register state is not yet safely stored, so a second core must not also pick it.

The port also carries an optional FIFO wait list beside the global lock
(`common_smp/inc/tx_thread.h:187-192`: list, per-core wait counts, head, tail). Whether it is used
is a per-port decision: the Cortex-A7 and A78 ports drive it
(`ports_smp/cortex_a7_smp/gnu/src/tx_thread_smp_protect.S`, with the queue manipulation in the
adjacent `tx_thread_smp_protection_wait_list_macros.h`), the Cortex-A53 port includes the macro
header but never calls into it.

**2. Acquisition order.** The global lock is taken strictly inside the interrupt mask: DAIF is set
first, the core id is read, then the load-exclusive on the flag
(`ports_smp/cortex_a53_smp/gnu/src/tx_thread_smp_protect.S:69-101`). Release reverses that
(`tx_thread_smp_unprotect.S:63-113`). The two domains never nest in the C kernel, so there is no
stated order between them; the per-TCB claim lock is taken by the scheduler after the global lock
has already been dropped. On the A53 port the spin loop restores the caller's interrupt posture
before retrying, so a core waiting for the kernel lock does service its own interrupts, and with
`TX_ENABLE_WFE` it parks in WFE until the releasing core issues SEV.

**3. Remote wake.** Inline reach into the shared structures, then an IPI. There is no inbox.
A resume or suspend runs the full rebalance
(`common_smp/src/tx_thread_smp_rebalance_execute_list.c`), which rebuilds the whole core-to-thread
mapping from the priority lists while honouring each thread's allowed-core mask, writes it into
`_tx_thread_execute_ptr` (`common_smp/inc/tx_thread.h:868-890`) and then pokes each core whose
assignment changed. The poke is `_tx_thread_smp_core_preempt`, a bare SGI write on the A53 port
(`ports_smp/cortex_a53_smp/gnu/src/tx_thread_smp_core_preempt.S:107-120`), called from
`common_smp/inc/tx_thread.h:834`, `common_smp/src/tx_thread_system_suspend.c:525` and `:833`,
`common_smp/src/tx_thread_time_slice.c:287`. The IPI carries no payload; the target reads the
execute pointer for its own core.

**4. Migration.** Yes, and it is a yank. No thread asks to move: the rebalance decides on every
scheduling event, on whichever core happens to be running it, and a thread already executing on
another core is displaced by sending that core an IPI. The only application control is a mask:
`_tx_thread_smp_core_exclude` (`common_smp/src/tx_thread_smp_core_exclude.c`) sets
`tx_thread_smp_cores_excluded` / `tx_thread_smp_cores_allowed`
(`common_smp/inc/tx_api.h:532-533`), which the rebalance consults before placing the thread.

**5. Lock across the switch.** Released inside. The switch-out path saves the thread stack pointer,
moves the core onto its own system stack, sets the outgoing thread's claim bit, then clears the
preempt-disable flag and the whole protection structure before branching into the scheduler loop
(`ports_smp/cortex_a53_smp/gnu/src/tx_thread_system_return.S:160-175`). The scheduler then runs
unlocked, and re-claims only the one TCB it is about to run, via the per-thread lock.

**6. Kernel stack model.** One stack per thread plus one system stack per core
(`_tx_thread_system_stack_ptr[TX_THREAD_SMP_MAX_CORES]`, `common_smp/inc/tx_thread.h:162`). The core
switches onto its system stack for the scheduler loop and the unlocked idle wait. No continuations
and no saved operation state; a blocked thread's kernel state lives on its own stack.

**7. Benchmark evidence the project publishes.** The tree ships the Thread-Metric harness
(`utility/benchmarks/thread_metric/`, described in `thread_metric_readme.txt`): eight event-rate
tests over a fixed interval, covering cooperative and preemptive scheduling, message passing,
synchronisation, memory allocation and interrupt handling. It is single-core by construction and
there are no result tables in the tree. The SMP evidence is functional, not numeric: 110 files of
regression tests under `test/smp/regression/`.

**8. Bearing on the KickOS questions.**

- Refusal on a full queue: yes, `tx_queue_send` with `TX_NO_WAIT` returns `TX_QUEUE_FULL` rather
  than blocking (`common_smp/src/tx_queue_send.c:271-282`, `:414`).
- Blocking on a peer: yes, twice. Acquiring the global lock spins until the owner releases it, and
  the scheduler spins on a TCB whose claim bit a peer has not yet set, looping back to pick another
  thread (`tx_thread_schedule.S:131-145`).
- Priority inheritance: it crosses cores by construction, since it only rewrites shared TCB fields
  under the one global lock (`common_smp/src/tx_mutex_get.c:118-380`). There is no core-local
  fast path to defeat.
- Fairness bound on the lock: per port. The A53 port has none, a plain retry loop. The A7 and A78
  ports interpose the FIFO wait list, which bounds a waiter by the number of cores ahead of it.

---

## RT-Thread

Licence Apache-2.0, read from `rt-thread/LICENSE` and from the SPDX line
in each file (`src/scheduler_mp.c:1-5`). Revision `7fa651bd`; `git describe` reads
`v5.0.2-2827-g7fa651bd29`. Paths below are relative to that tree.

**1. Lock domains.** One global scheduler lock, plus a general-purpose spinlock type the rest of
the system uses freely.

- `_mp_scheduler_lock` (`src/scheduler_mp.c:47`) is the kernel lock. It covers the global ready
  table `rt_thread_priority_table` and its bitmap, every per-CPU ready table, and the thread state
  fields.
- The ready queue itself is partitioned in two levels: a global priority table for unbound threads,
  and a per-CPU priority table plus bitmap inside `struct rt_cpu` (`rt-thread/include/rtdef.h:685-712`). Both
  are under the same one lock, so this is a partitioned data structure with a single lock domain,
  not a partitioned lock set.
- `struct rt_spinlock` with `rt_spin_lock` / `rt_spin_lock_irqsave` (`src/cpu_mp.c:63-131`) is the
  ordinary driver and subsystem lock. It is not a kernel domain.
- `_cpus_lock` (`src/cpu_mp.c:25`, `rt_cpus_lock` at `:168`) is a legacy all-CPU big lock kept for
  BSP code; the scheduler explicitly drops it on entry and never takes it
  (`src/scheduler_mp.c:503-505`).

**2. Acquisition order.** Inside the mask, and the mask is local-only. `SCHEDULER_LOCK`
(`src/scheduler_mp.c:79-89`) disables local interrupts, increments the current thread's critical
nesting, then takes the spinlock; unlock reverses it exactly. `rt_spin_lock` raises the scheduler
critical count before taking the lock, and `rt_spin_lock_irqsave` disables local interrupts first
(`src/cpu_mp.c:63-118`). The kernel lock and a subsystem spinlock nest in that order by
construction, since the scheduler never calls out while holding its own lock.

**3. Remote wake.** Inline reach into the target queue under the global lock, followed by an IPI.
`_sched_insert_thread_locked` (`src/scheduler_mp.c:280-360`) writes the thread into either the
global table or the bound core's table, then sends `RT_SCHEDULE_IPI`: to every core except itself
for an unbound thread, to the single bound core otherwise. The IPI carries nothing; the handler
just runs the scheduler.

There is a second, separate mechanism for cross-core work that is not a wake: a per-CPU request
inbox in `components/drivers/smp_call/smp_call.c`. Each core owns a lock-free request queue plus a
slot array, a caller enqueues a request and sends `RT_SMP_CALL_IPI`
(`smp_call.c:150-161`), and the handler drains the queue.

**4. Migration.** Yes, decided by the scheduler, a yank. An unbound thread lands in the global
table and is taken by whichever core next schedules; a core that must give up its current thread
re-inserts it into the global table, from where another core may take it
(`src/scheduler_mp.c:645-660`). Affinity is `rt_sched_thread_bind_cpu`
(`src/scheduler_mp.c:1501-1570`): binding a running thread to a different core sends the IPI that
forces it off the core it is on.

**5. Lock across the switch.** Held across it. `rt_schedule` picks the successor with the lock held
and calls `rt_hw_context_switch` without releasing it (`src/scheduler_mp.c:870-884`); only the
no-switch branch unlocks in place. The arch switch routine calls back into
`rt_cpus_lock_status_restore` -> `rt_sched_post_ctx_switch` once it is on the new stack, and that is
where the lock is dropped (`src/cpu_mp.c:243-250`, `src/scheduler_mp.c:1221-1238`; the call sites
are `libcpu/aarch64/common/mp/context_gcc.S:50`, `:78`, `:133`). Local interrupts stay masked
across the whole window. The consequence is explicit in the code: while one core is mid-switch,
every other core is spinning for the kernel lock.

**6. Kernel stack model.** One stack per thread. The aarch64 SMP port
(`libcpu/aarch64/common/mp/`) declares no per-CPU kernel stack; the exception frame is built on the
interrupted thread's stack and the stack pointer is stored into the TCB by the switch path. No
continuations, no saved operation state.

**7. Benchmark evidence the project publishes.** A performance test group under `src/utest/perf/`
with its own README: context switch time (`context_switch_tc.c`), interrupt latency
(`irq_latency_tc.c`), and per-primitive throughput for events, mailboxes, message queues and
semaphores. All of it is single-core in shape. The SMP evidence is functional: `src/utest/smp/`
(affinity, binding, preemption, spinlock, assigned idle cores) and
`components/drivers/smp_call/utest/`. No published numbers in the tree.

**8. Bearing on the KickOS questions.**

- Refusal: the cross-core call API refuses rather than waits. `rt_smp_call_request` returns
  `-RT_EBUSY` if the caller's request object is still in use by a previous call, and `-RT_EINVAL`
  if a wait flag is passed to the request form (`smp_call.c:173-190`).
- Blocking on a peer: yes, in three places. The kernel lock spins. `SMP_CALL_WAIT_ALL` spins on an
  atomic mask until every target core has run the callback (`smp_call.c:306-310`). The broadcast
  path spins on a per-target request slot's own lock before it can even enqueue
  (`smp_call.c:275-278`, commented in the source as a spin on previous occupation).
- Priority inheritance: crosses cores, as everything runs under one lock over shared TCBs
  (`src/ipc.c`).
- Fairness bound: yes on aarch64. `rt_hw_spin_lock` is a ticket lock with WFE parking
  (`libcpu/aarch64/common/cpu.c:60-146`), so a waiter is served in arrival order.

---

## ChibiOS

Licence GPL-3.0 (version 3 only), read from `ChibiOS/license.txt` and from
the header of every source file (`os/rt/src/chschd.c:1-17`). Revision `fd2e59c3`; the tree carries
no tag on HEAD. Paths below are relative to that tree. This is the GPL tree: everything here is
description, no expression is carried over.

**1. Lock domains.** One global kernel spinlock, and per-core scheduler state behind it.
ChibiOS SMP is built from OS instances: each core runs its own instance
(`os/rt/include/chinstances.h`, with the instance type at `os/rt/include/chobjects.h:405-510` and
its ready list field at `:420`), each instance owns its own
ready priority queue and its own current-thread pointer, and every thread belongs permanently to one
instance. There is exactly one lock for all instances: `port_spinlock_take` /
`port_spinlock_release`, one hardware spinlock number on the RP2 port
(`os/common/ports/ARMv6-M/smp/rp2/chcoresmp.h:60`, `:160-176`). So the data is partitioned per core
and the lock is not.

**2. Acquisition order.** Inside the mask. `port_lock` raises BASEPRI or disables IRQ and then takes
the spinlock; `port_unlock` releases the spinlock and then lowers the mask
(`os/common/ports/ARMv7-M/chcore.h:855-892`, same shape in `ARMv6-M/chcore.h:520-530` and
`ARMv8-M-ML/chcore.h:640-660`). There is only one kernel lock, so no inter-lock order exists.

**3. Remote wake.** Inline reach into the peer instance's ready queue, under the one global lock,
plus a best-effort notification. `chSchReadyI` checks whether the thread's owner instance is the
current core and, if not, notifies the owner before inserting
(`os/rt/src/chschd.c:285-299`); `chSchWakeupS` does the same and returns without considering a local
preemption (`os/rt/src/chschd.c:402-425`). The notification on the RP2 port writes a fixed
reschedule word into the SIO inter-core FIFO, and only if the FIFO reports space
(`os/common/ports/ARMv6-M/smp/rp2/chcoresmp.h:145-158`). If the FIFO is full the write is skipped:
a message is already pending, so the peer will reschedule anyway. The receiving handler drains the
FIFO and does nothing else; the reschedule comes from the normal interrupt epilogue
(`os/common/ports/ARMv6-M/smp/rp2/chcoresmp.c:130-190`).

**4. Migration.** None. A thread's owner instance is fixed when it is created, defaulting to the
creating core (`os/rt/include/chthreads.h:84`, `os/rt/src/chthreads.c:129-133`), and there is no API
to change it. The scheduler asserts that the thread it is switching away from belongs to the
current instance (`os/rt/src/chschd.c:318`). This is the partitioned end of the design space: cores
share objects and one lock, never threads.

**5. Lock across the switch.** Held across it. `chSchGoSleepS` picks the successor and tail-calls
`chSysSwitch` (`os/rt/src/chschd.c:310-341`), which expands to trace and statistics hooks and then
`port_switch` (`os/rt/include/chsys.h:270-276`) with no lock operation anywhere in between. The
resumed thread returns from `port_switch` still holding the lock and its own caller releases it; a
thread running for the first time releases it in the port's thread entry trampoline
(`os/common/ports/ARMv6-M/compilers/GCC/chcoreasm.S:99-112`, which calls the spinlock release then
enables interrupts before jumping to the thread body). While one core is mid-switch the other core
cannot enter any kernel critical section.

**6. Kernel stack model.** One stack per thread. Each thread carries its own working area with the
stack base recorded for the overflow checks that the port wraps around `port_switch`
(`os/common/ports/ARMv6-M/chcore.h:429-441`). Interrupt handlers on the Cortex-M ports run on the
main stack while threads run on the process stack, which is a per-core exception stack rather than a
per-core kernel stack. No continuations, no saved operation state.

**7. Benchmark evidence the project publishes.** The RT test suite ships a benchmark sequence:
`test/rt/source/test/rt_test_sequence_012.c`, titled Benchmarks, measuring messages, context
switches, semaphore and mutex operations, queues and thread creation. There is also a separate
core benchmark under `test/corebmk/`. Neither is SMP-specific and neither carries result tables in
the tree.

**8. Bearing on the KickOS questions.**

- Refusal: the cross-core notification is dropped when the hardware FIFO is full
  (`chcoresmp.h:150-157`). That is a refused signal that is safe only because the design makes the
  signal idempotent: the state the peer must observe is already in the shared ready queue, and the
  notification only asks it to look.
- Blocking on a peer: yes. Every kernel critical section on either core contends for the same
  spinlock, and that lock is held across context switches, so a core can wait on a peer for the
  duration of a switch.
- Priority inheritance: mutexes are global objects with no instance awareness
  (`os/rt/src/chmtx.c` contains no SMP conditional), so inheritance rewrites the owner's priority
  wherever it runs, under the one lock, and the owner's instance is notified through the same
  ready path.
- Fairness bound: none. `port_spinlock_take` is a bare poll on the SIO spinlock register, no
  ticket, no queue, no backoff.

---

## RIOT

Licence LGPL-2.1-only, read from `RIOT/LICENSE` and from the SPDX line in
each core file (`core/sched.c:1-4`). Revision `92715ebf`; `git describe` reads
`2026.10-devel-496-g92715ebf97`. Paths below are relative to that tree.

**RIOT's kernel is single-core, and this row is therefore not an SMP precedent.** `core/` contains
`sched.c`, `thread.c`, `mutex.c`, `msg.c`, `mbox.c`, `cond.c`, `thread_flags.c`, `msg_bus.c` and
`lib/`; there is no SMP file, no per-CPU structure, no core id, no IPI. A case-insensitive grep of
`core/` for SMP, multicore or a core-id accessor returns exactly one hit, and it is a warning in the
opposite direction: `core/lib/atomic_c11.c:403` notes that the software-only barrier used there is a
no-op that would break on SMP. The scheduler's own state is a set of file-scope volatiles
(`core/sched.c:56-74`), not indexed by core. Dual-core parts exist in `cpu/` but the second core is
outside the kernel; the only mentions are hazard notes, for example
`cpu/rpx0xx/periph/timer.c:42` and `cpu/rpx0xx/ldscripts/RP2040.ld:41`.

Of the nine points, five are **not applicable**: lock domains, acquisition order between locks,
remote wake, migration, and whether a kernel lock is released inside the switch. RIOT has no kernel
lock at all, so there is nothing for the last of these to answer; it is not the answer "held" or
"released", it is the absence of the object. The four that remain:

**Kernel critical sections.** Plain local interrupt masking, `irq_disable` / `irq_restore`
(`core/sched.c:315`, `:340-348`). Deferred switching goes through a request flag,
`sched_context_switch_request` (`core/sched.c:74`, set at `:291`), consumed on interrupt exit.

**Kernel stack model.** One stack per thread, recorded in the TCB as start and size
(`core/include/thread.h:196-202`). Interrupts on the Cortex-M ports run on a separate ISR stack
(`cpu/cortexm_common/vectors_cortexm.c:79`, queried through `thread_isr_stack_start` and friends at
`core/include/thread.h:527-537`). No continuations, no saved operation state.

**Benchmark evidence the project publishes.** A whole directory of harnesses, `RIOT/tests/bench/`:
`msg_pingpong`, `mutex_pingpong`, `thread_flags_pingpong`, `thread_yield_pingpong`, `sched_nop`,
`runtime_coreapis`, `sizeof_coretypes`, plus timer and atomics benchmarks, with a shared
`Makefile.bench_common`. These are IPC and scheduling benchmarks and they are the most directly
relevant published harness set of the five projects in this part, but they measure a single-core
kernel.

**Bearing on the KickOS questions.** Refusal on a full queue exists and is explicit:
`msg_try_send` returns zero rather than blocking when the target's queue is full
(`core/msg.c:45-50`, `:74`). Priority inheritance exists on mutexes, raising the owner's priority on
contention and restoring the recorded original on release (`core/mutex.c:78-83`, `:218-222`) -- a
single-core design, so the cross-core question does not arise. Nothing blocks on a peer and there is
no lock to be fair about.

---

## FreeRTOS-LTS

Licence MIT, read from `FreeRTOS-LTS/LICENSE.md` and from the SPDX line in
the kernel sources (`FreeRTOS/FreeRTOS-Kernel/tasks.c:1-6`). The RP2040 port inside it is dual
licensed, `SPDX-License-Identifier: MIT AND BSD-3-Clause`
(`FreeRTOS/FreeRTOS-Kernel/portable/ThirdParty/GCC/RP2040/port.c:1-6`), with the BSD text in that
port's own `LICENSE.md`. The pico-sdk it builds on is BSD-3-Clause
(`pico-sdk/LICENSE.TXT`, and the SPDX line in
`src/rp2_common/hardware_sync_spin_lock/include/hardware/sync/spin_lock.h:1-5`).

**Revision: this tree has no `.git`.** It is a release snapshot, so there is no HEAD to re-derive.
It names itself in `manifest.yml`: release `202604.00-LTS`, with `FreeRTOS-Kernel` at `V11.3.0`
(confirmed by `tasks.c:2` and `FreeRTOS-LTS/include/task.h:57`). The pico-sdk tree beside it does have a `.git`:
revision `98a542c1`, `git describe` reads `2.3.0`. Kernel paths below are relative to
`FreeRTOS-LTS/FreeRTOS/FreeRTOS-Kernel/`.

**1. Lock domains.** Two, both global, split by who may take them rather than by what they protect.

- The **task lock**, held only by task-level code. Its purpose is to make one core wait while
  another core's scheduler is suspended (`tasks.c:5209-5217`).
- The **ISR lock**, which protects the ready lists from concurrent task and interrupt access.
- Both are port-provided: `portGET_TASK_LOCK` / `portGET_ISR_LOCK` with a core id argument. Ready
  lists themselves are global and shared; `pxCurrentTCBs[]` and `xYieldPendings[]` are per core.

On RP2040 the two map to two of the RP2040's 32 SIO hardware spinlocks, `configSMP_SPINLOCK_0` and
`configSMP_SPINLOCK_1` (`portable/ThirdParty/GCC/RP2040/include/portmacro.h:258-268`). The kernel
needs them recursive and the hardware locks are not, so the port adds a software owner table and a
per-lock recursion count around the try-lock (`portmacro.h:218-256`).

**2. Acquisition order.** Stated in the source and enforced by convention: interrupts off first,
then the task lock, then the ISR lock. `vTaskEnterCritical` disables interrupts, and on the first
nesting level takes the task lock then the ISR lock (`tasks.c:7038-7048`). `vTaskSwitchContext`
does the same with the order written down in a comment at the call
(`tasks.c:5217`: the task lock must always be acquired first). Release is the mirror
(`tasks.c:5299-5300`). The interrupt-safe entry takes only the ISR lock
(`tasks.c:7098-7104`), which is exactly why the order matters: a task-level holder of the ISR lock
alone could be interrupted into a second acquisition.

The RP2040 port takes the SIO locks with the pico-sdk's `spin_lock_unsafe_blocking`, the variant
that does **not** touch the interrupt mask, because the kernel has already masked them. The safe
variants are the ones that mask; see point 8.

**3. Remote wake.** Inline reach into shared ready lists, then a fire-and-forget IPI. A task becomes
ready in the shared list, and `prvYieldForTask` (`tasks.c:891-1000`) walks the cores, finds one
running something lower-priority and eligible under the target's affinity mask, and calls
`prvYieldCore` on it. `prvYieldCore` (`tasks.c:350-365`) marks the victim's TCB as scheduled-to-yield
and calls the port's `portYIELD_CORE`. On RP2040 that is `vYieldCore`
(`portable/ThirdParty/GCC/RP2040/port.c:500-514`): a single write into the SIO inter-core FIFO,
non-blocking, and the source comment states that if the FIFO is full an interrupt must already be
pending. No inbox, no payload.

**4. Migration.** Yes, a yank, decided by the kernel on whichever core runs the selection.
`prvSelectHighestPriorityTask` (`tasks.c:1080-1270`) re-assigns tasks to cores on every switch, using
`xTaskRunState` in the TCB as the core id a task is currently on. Affinity is a per-task mask,
`uxCoreAffinityMask` (`tasks.c:384`), set at creation through the `...AffinitySet` creation variants
(`tasks.c:1377`, `:1489`, `:1608`, `:1784`) or later through `vTaskCoreAffinitySet`. A task never
asks to move.

**5. Lock across the switch.** Released before it. `vTaskSwitchContext` takes both locks, selects the
new task into `pxCurrentTCBs[xCoreID]`, and releases both locks before returning
(`tasks.c:5217-5300`). The register-level switch happens after that, in the port's PendSV handler
(`portable/ThirdParty/GCC/RP2040/port.c:518` onwards). The function asserts that it is never called
from inside a critical section (`tasks.c:5223-5224`), which the source notes is an SMP-only
requirement. Interrupts are still masked over the switch itself, but no kernel lock is.

**6. Kernel stack model.** One stack per task, recorded in the TCB. The Cortex-M0+ switch saves the
callee-saved registers onto the outgoing task's own stack and reloads from the incoming one; the
hardware exception frame is on the same stack. No per-CPU kernel stack, no continuations, no saved
operation state.

**7. Benchmark evidence the project publishes.** None in this tree for the kernel. The LTS snapshot
carries per-library test directories for the network and cloud libraries (`FreeRTOS/coreMQTT/test`,
`FreeRTOS/FreeRTOS-Plus-TCP/test`, and so on) but `FreeRTOS/FreeRTOS-Kernel/` has no test or
benchmark directory at all: only `examples/`, `portable/` and the kernel sources. No SMP benchmark,
no IPC benchmark, no results.

**8. Bearing on the KickOS questions.**

- Refusal: yes, in two shapes. `xQueueGenericSend` with a zero block time returns `errQUEUE_FULL`
  (`queue.c:1094-1096`, `:1159-1161`). And the cross-core yield notification is dropped when the
  hardware FIFO is full (`port.c:510-512`), on the same idempotence argument ChibiOS uses.
- Blocking on a peer: yes, and one case is deliberate. Taking the task lock blocks while another
  core has the scheduler suspended, which the comment at `tasks.c:5209-5216` gives as the reason the
  task lock exists. Separately, `prvCheckForRunStateChange` (`tasks.c:822-885`) makes a core that has
  been asked to yield release both locks, re-enable interrupts so the pending yield is serviced,
  re-disable, retake both locks, and re-test -- an unbounded loop driven by peers.
- Priority inheritance: crosses cores. `xTaskPriorityInherit` (`tasks.c:6650-6743`) raises the
  holder's priority and, under `configNUMBER_OF_CORES > 1`, calls `prvYieldForTask` on the holder,
  which will target whichever core it is on.
- Fairness bound: none. The RP2040 SIO spinlock acquire is a bare retry loop on the lock register
  (`pico-sdk src/rp2_common/hardware_sync_spin_lock/include/hardware/sync/spin_lock.h:302-315`), no
  ticket and no queue. The port's recursion wrapper adds no ordering.

**The pico-sdk contract, verified by path.** Two statements bear directly on holding such a lock
across a copy or a context switch, and both are in
`pico-sdk/src/rp2_common/hardware_sync/include/hardware/sync.h`:

- Line 37: the default spinlock methods, naming `spin_lock_blocking`, always disable interrupts
  while the lock is held, because use from both IRQ handlers and user code is expected; the same
  sentence adds that spin locks are only expected to be held for brief periods.
- Line 311: the contract a caller of a striped lock must abide by is to hold it briefly, with IRQs
  disabled, and not while holding another spin lock.

The implementation header repeats it as a convention in the acquire routine itself
(`hardware_sync_spin_lock/include/hardware/sync/spin_lock.h:302-315`): no WFE and no backoff,
justified in the comment by these locks being very short lived, never blocking, and run with
interrupts disabled, so the only party that can be delaying you is another core and it will be done
shortly. `spin_lock_blocking` is that unsafe acquire wrapped in save-and-disable-interrupts
(`:345-348`). FreeRTOS's RP2040 port does not violate this by using the unsafe variant -- the kernel
has already masked interrupts -- but the brevity half of the contract is a property of what the
kernel does while holding the lock, and it is the half that a copy or a switch under the lock would
break.

---

## The four rows the first pass missed, and the five named at the end

Part C covers the four rows the first survey pass did not open and the five named at the end.
Rows 1 and 2 are external trees, read and never copied; rows 3 and 4 are this tree's
own, so they are quoted freely and still cited by path. Revisions below were re-derived with
`git -C <path> rev-parse --short=8 HEAD` at the time of writing; re-derive again before citing.

## Composite -- a lock-free capability kernel whose reclamation is timestamp quiescence

**Licence, and where it was read.** There is NO top-level licence file. `ls` of
`composite` shows no `LICENSE` and no `COPYING`; the only files of
that name anywhere in the tree are `composite/src/composer/LICENSE` and the two Pintos notices at
`composite/src/platform/i386/LICENSE.pintos` and `composite/src/platform/x86_64/LICENSE.pintos`, none of which
covers the kernel. The statement that governs the tree is prose in
`composite/README.md:53`, which says the code is licensed under GPL
version 2.0 with the class path exception unless otherwise noted, and that significant portions
of user level are BSD.

**The class-path wording is in that README line and in NO source header.** A tree-wide grep for
`classpath`, `class path`, `linking exception` and `SPDX-License-Identifier` finds the exception
only at that one README line. The kernel sources carry a different and weaker notice: a plain
statement that redistribution of the file is permitted under the GNU General Public License v2,
with no exception wording and no SPDX tag. The files under `composite/src/kernel/` that carry that plain
GPLv2 notice are `capinv.c:1-6`, `retype_tbl.c:1-7`, `tcap.c:1-10`,
`composite/src/kernel/include/liveness_tbl.h:1-8`,
`composite/src/kernel/include/captbl.h:1-10`, `composite/src/kernel/include/per_cpu.h:1-7` and
`composite/src/kernel/include/ipi_cap.h:1-7`. The files that carry NO licence header at all are `captbl.c`,
`liveness_tbl.c`, `pgtbl.c`, `ulinv.c`, `composite/src/kernel/include/cap_ops.h`,
`composite/src/kernel/include/cc.h`, `composite/src/kernel/include/assert.h` and
`composite/src/kernel/include/asm_ipc_defs.h`. The only SPDX tags in the tree are
`BSD-3-Clause` under `composite/src/components/implementation/simple_vmm/vmm/` and `MIT` inside the
vendored musl. So the reading recorded in `CONTEXT.local.md` -- GPL-2.0 with class path
exception -- is the README's, and it is a tree-level statement rather than a per-file one; a
clean-room posture against this tree has to rest on the README plus the plain-GPLv2 headers, not
on a licence file.

**Revision.** `2f77a25a`. `git status --porcelain` is clean apart from the uninitialised
submodules described below. `composite/README.md:46` says the code is pre-alpha quality, and that some
parts are quite solid while many are not.

### The three traps, each verified rather than assumed

**The tree is un-`make init`'d, and the missing symlink is real.** `composite/src/kernel/include/` holds a
file `chal.h` but NO directory `chal`; `ls src/kernel/include/chal` fails. Every generic kernel
source includes through that path -- `composite/src/kernel/pgtbl.c:7` includes `composite/chal/chal_proto.h`,
`composite/src/kernel/capinv.c:11` includes `composite/chal/call_convention.h`,
`composite/src/kernel/include/assert.h:5`
includes `composite/chal/cpuid.h` -- so the generic kernel cannot be read as a closed unit in this
checkout. The targets are present and readable under the platform directory instead:
`composite/src/platform/i386/chal/` holds `call_convention.h`, `chal_config.h`, `chal_plat.h`,
`chal_proto.h`, `cpuid.h`, `defs.h` and a `shared/` directory beside them. So no answer here is
actually BLOCKED by the
missing symlink -- it is an inconvenience of resolution, and the platform half was read directly.
`composite/src/Makefile:60-62` is where `init` recurses into `components` and `platform`.

**The `ps` and `ck` submodule directories are EMPTY, and that DOES block answers.**
`composite/src/components/lib/ps/ps` and `composite/src/components/lib/ck/ck` each contain nothing
but `.` and `..`;
`git submodule status` prints them with the leading `-` that means not initialised, at
`f7bcdbe0` and `26bb45af` respectively, per `.gitmodules`. Those are the parallel-sections and
concurrency-kit libraries that carry the user-level memory reclamation. So this survey can say
what the KERNEL does about lifetime and cannot say anything about the user-level reclamation
those two libraries implement. That gap is recorded, not guessed at.

**The ARM port is `NUM_CPU 1`, so the SMP story is x86-only.**
`composite/src/platform/armv7a/chal/shared/cos_config.h:20` sets `NUM_CPU` to 1 outright.
`composite/src/platform/i386/chal/shared/cos_config.h:20-23` sets `NUM_CPU_KERNEL` to 8 and defaults
`NUM_CPU` to it. `composite/src/platform/` holds `armv7a`, `i386`, `x86_64` and `archived`. Everything
below about cross-core behaviour is therefore read from the x86 side, and the ARM port is a
uniprocessor port that compiles the same generic kernel.

**The docs for exactly the interesting topics are literally a TODO stub.**
`composite/doc/dev_manual/01_intro.md:146` is the `### User-level Scheduling` heading and `:148` is a
single blockquoted `TODO`; `01_intro.md:157` is `### Wait-Free, Parallel Kernel` and `:159` is
the same. Both then list published papers by URL and nothing else.
`composite/doc/dev_manual/01_intro.md:163,165` (`Access Control for Time`) and `:169,171`
(`Minimal, Specialized OSes`) are the same shape. `composite/doc/README.md:41-42` says outright that the
only current documents are the style guide, the Rust note and that file itself. So everything
useful about the mechanism comes from source.

### Lock domains

There are none. A grep of `composite/src/kernel/` for `spin_lock`, `spinlock`, `mutex`, `cos_lock`,
`lock_take` and `ck_spinlock` returns nothing. The kernel is seven C files --
`capinv.c`, `captbl.c`, `liveness_tbl.c`, `pgtbl.c`, `retype_tbl.c`, `tcap.c`, `ulinv.c` --
and the mutual exclusion in all of them is a single compare-and-swap per update with an error
return on failure, never a retry loop: `composite/src/kernel/captbl.c:92,128,172,181`,
`composite/src/kernel/capinv.c:139,154,158,187,288,300,362,365`,
`composite/src/kernel/include/cap_ops.h:59,177,185`, `composite/src/kernel/include/component.h:47,51`,
`composite/src/kernel/include/liveness_tbl.h:78,120`, `composite/src/kernel/include/captbl.h:255`. A grep for a
`while` construct around a CAS in those files finds none. Failure surfaces to the caller as
`-ECASFAIL` (`composite/src/kernel/include/shared/cos_errno.h`), so the RETRY is the user-level caller's
and the kernel path stays straight-line.

The complement to having no locks is that state is per core. `composite/src/kernel/include/per_cpu.h`
reaches the current thread out of a per-core info block carried on the kernel stack
(`per_cpu.h:21-25`), with a cache-aligned `struct per_core_variables` (`:32-35`) as the
alternative it says costs an extra page touch. `composite/src/kernel/include/pgtbl.h:44-53` declares the
TLB-quiescence array as one cache-line-aligned entry per CPU.

**And the capability itself is core-bound.** `composite/src/kernel/include/shared/cos_types.h:207` defines
a type check that additionally requires the capability's own `cpuid` field to equal the calling
core's. Thread, temporal-capability and asynchronous-receive capabilities all carry that field
(`composite/src/kernel/include/thd.h:93,121`, `composite/src/kernel/include/tcap.h:29,53`,
`composite/src/kernel/include/inv.h:33,42`), and the
core-checked form is used on the switch, the send and the receive paths
(`composite/src/kernel/capinv.c:630,655,775,1675,1751,1768`,
`composite/src/kernel/include/inv.h:205,209,216`). So a
thread capability resolved from the wrong core is refused with `-EINVAL` rather than serialised.
That is one reason no lock is needed, and it is also the closest thing on this box to the absent
row 6 below.

### Acquisition order

Not applicable -- there is no lock to order. What replaces an acquisition order is an ORDER OF
CHECKS on the deactivation path, and it is strict. `composite/src/kernel/include/cap_ops.h:158-192`
(`cap_kmem_freeze`) refuses unless the reference count is at most one and the memory is not
already frozen, stamps `frozen_ts` with the cycle counter, and only then CASes the frozen flag
in. `composite/src/kernel/capinv.c:120-158` then requires the frozen flag before anything else, runs the
quiescence check, CASes a scan flag in to keep two cores from scanning the same page, scans, and
CASes the scan flag back out on both the success and the failure exit. `kmem_deact_post`
(`composite/src/kernel/capinv.c:177-200`) clears the kernel-memory bit with a CAS, drops the retype
reference, and zeroes the page.

### THE RECLAMATION MECHANISM -- what quiescence is, and what timestamp it keys on

This is the row's reason for existing, so it is stated in full.

**There are TWO quiescence periods and they key on different timestamps.**

The first is the KERNEL quiescence period. Its timestamp is the cycle counter read by `rdtscll`,
defined for x86 at `composite/src/kernel/include/liveness_tbl.h:20-22` as the `rdtsc` instruction. The
period is a CONSTANT, not a barrier and not a grace-period protocol:
`composite/src/platform/i386/chal/shared/cos_config.h:83-84` sets `KERN_QUIESCENCE_PERIOD_US` to 500 and
derives `KERN_QUIESCENCE_CYCLES` from it, and the comment above it states the intent -- the
kernel quiescence period is the worst-case execution time in the kernel plus the worst-case of a
compare-and-swap. The armv7a file repeats the same two lines at
`composite/src/platform/armv7a/chal/shared/cos_config.h:69-70`. The test itself is one subtraction:
`composite/src/kernel/include/shared/cos_types.h:222` defines `QUIESCENCE_CHECK` as the current stamp minus
the past stamp being greater than the period.

So reclamation is a DEADLINE rather than a handshake. Nobody publishes that they have left the
kernel; the kernel argues that since a core cannot be inside a kernel entry longer than the
worst-case, any core that had the stale reference has necessarily left by now.

**The liveness table is where the stamp lives.** `composite/src/kernel/include/liveness_tbl.h:25-30` is the
entry: a 64-bit epoch and a 64-bit deactivation timestamp, packed. The table is a flat array of
2^20 entries (`:17-18`, `:92-93`), zeroed at boot by the whole of
`composite/src/kernel/liveness_tbl.c:3-12`. A reference to an object is a `struct liveness_data`
(`liveness_tbl.h:35-38`) holding an id and the epoch as it was when the reference was taken.
`ltbl_isalive` (`:95-103`) compares the recorded epoch against the table's current one and calls
the object dead the moment they differ. `ltbl_expire` (`:106-123`) writes the deactivation
timestamp FIRST, then issues `cos_mem_fence`, then bumps the epoch with a single CAS -- the
ordering matters because a reader that sees the new epoch must not be able to see a stale stamp.
`ltbl_timestamp_update` (`:172-189`) does the same fence before reading the counter, with a
comment saying the barrier is there so the counter read is not hoisted above the store.
`ltbl_isfreeable` (`:129-141`) is the question the row's title names: it reads the entry's
deactivation stamp, reads the counter now, and answers yes when stamp plus period is in the past.

**Where it is applied, both on free and on REUSE.** `captbl_del`
(`composite/src/kernel/include/captbl.h:377-424`) does not free a slot. It updates the liveness timestamp
first (`:401`), sets the slot's type to that tree's quiescing marker and records the liveness id (`:406-410`),
fences, and only then clears the allocation bit; when the cacheline's allocation map empties, the
whole cacheline takes the same quiescing type while its size field is deliberately kept because
the quiescence check still needs it (`:416-421`). The matching check is on the ALLOCATION side,
not on a reclaim sweep: `captbl_add` (`composite/src/kernel/include/captbl.h:277` onward, the
check at `:304-350`) refuses to reuse a
slot still carrying that quiescing type until the period has elapsed, and when the cacheline is
being re-sized it walks every entry in that cacheline and applies the same test to each, because
resizing changes what the neighbours mean (`:306-337`). Either failure returns `-EQUIESCENCE`. A
deactivated capability-table page is scanned entry by entry under the same rule before its memory
can go back (`composite/src/kernel/captbl.c:193-230`, called from `captbl_kmem_scan` at `:233-247`).

**The second period is TLB quiescence, and its timestamp is a flush record rather than a
constant.** `composite/src/kernel/include/pgtbl.h:44-53` declares one cache-line-aligned entry per CPU with
two fields: the last periodic flush, updated by the timer, and the last mandatory flush, updated
by the TLB-flush inter-processor interrupt (`composite/src/kernel/capinv.c:39` writes the mandatory one).
`chal_tlb_quiescence_check` (`composite/src/platform/i386/chal_pgtbl.c:150-180`) first asks whether the
periodic flush on the CURRENT core is already later than the unmap stamp -- since the periodic
flush happens on every core, one core's record answers for all, as its comment says, assuming
consistent counters -- and only if not does it walk every core's mandatory-flush record and
demand each be later than the stamp. So the TLB period is a real observed event, unlike the
kernel period which is a constant.

**Which of the two applies is decided by level.** `composite/src/kernel/capinv.c:127-135` takes the TLB
period for a top-level capability table and the kernel period for every other level, the comment
naming the reason as an optimisation that avoids a current-component lookup on the invocation
path. `composite/src/platform/i386/chal_pgtbl.c:862-873` makes the same split for page tables.
`composite/src/kernel/retype_tbl.c:315-329` requires TLB quiescence before a frame changes type.

**In one sentence:** Composite never waits for readers -- it stamps a deactivation with the cycle
counter, marks the slot quiescing rather than free, and refuses reuse or reclamation until
the stamp is older than a CONSTANT worst-case kernel-execution period (or, for anything
translation can cache, until every core's recorded TLB flush is later than the stamp), so the
guarantee is an argument about the maximum time a core can hold a stale reference rather than an
observation that no core still holds one.

### Remote wake

Composite's cross-core wake is a PER-PAIR RING plus an inter-processor interrupt, which makes it
the closest external shape on this box to KickOS's own row 3.
`composite/src/kernel/include/ipi_cap.h:18-21` states N by N rings for N cores, of 16 slots, with the size
required to be a power of two. `struct xcore_ring` (`:29-35`) is a cache-line-separated sender
index and receiver index around a fixed array of `struct ipi_cap_data` (`:23-27`), each slot
holding the target receive capability, its epoch and a copy of the component info.
`struct IPI_receiving_rings` (`:43-49`) groups all source rings for one destination core
contiguously, with a comment saying they are laid out that way because the receiver scans them,
plus a rotating scan start whose stated purpose is to prevent starvation.

The producer side is `cos_ipi_ring_enqueue` (`:92-118`): it indexes the ring by destination and
by its OWN core id, computes the next index, and **refuses with `-EBUSY` when the ring is full**
(`:104`) rather than waiting. It writes the slot, publishes the new sender index, then fences.
`cos_cap_send_ipi` (`:120-130`) calls that and, only on success, calls `chal_send_ipi`. So the
raise follows the publication, and a full ring is an error the caller sees. Note for the design
space: the slot write and the index publication at `:110-113` have no barrier BETWEEN them, only
one after; that is sound on the x86 store ordering this port targets and is not a portable
pattern, which is exactly the point KickOS's own contract makes differently (row 3).

The consumer is `cos_ipi_ring_dequeue` (`:53-67`): it compares the two indices, copies the slot
out, advances the receiver index and fences. `cos_ipi_arcv_get` (`:69-84`) resolves the receive
capability out of the copied component info and carries a FIXME saying the epoch and liveness are
not actually checked there -- worth recording, since it is the one place the liveness mechanism
described above is named and not applied.

### Migration

There is no thread migration in `composite/src/kernel/`. The core-bound capability check above makes the
opposite choice: a thread, its temporal capability and its receive endpoint each carry a `cpuid`
and are refused from any other core. Movement between cores is a user-level scheduling question,
and the documentation for it is the `TODO` stub named above, so this survey cannot say what the
project's answer is.

### Is the lock released inside the switch

Not applicable -- there is no lock. The switch itself is `cap_thd_switch`
(`composite/src/kernel/capinv.c:387` onward), and it takes no synchronisation of any kind.

### Kernel stack model

Not read out of this checkout with confidence. The per-core information block is reached FROM the
kernel stack (`composite/src/kernel/include/per_cpu.h:20-23`), which says the stack is per core rather than
per thread, but the definition of that block and the entry sequence live behind the missing
`chal` symlink and in `composite/src/platform/i386/`, and this row did not chase them. Recorded as not
established here.

### Published benchmark evidence

In-tree there is a cross-core microbenchmark harness but no committed numbers:
`composite/src/components/implementation/tests/micro_xcores/` holds `test_ipi_roundtrip.c`,
`test_ipi_n_n.c`, `test_ipi_switch.c` and `test_ipi_interference.c`, each accumulating into a
per-core `perfdata` structure, and `composite/src/components/implementation/tests/` also carries
`bench_lock`, `bench_sem`, `bench_syncipc`, `bench_sched_yield` and `bench_chan_*`. The claims
themselves are external: `composite/doc/dev_manual/01_intro.md:166` points the kernel's details at a 2015
RTAS paper, `:151-155` points user-level scheduling at four papers, and
`composite/doc/dev_manual/01_intro.md:139-141` claims the fastest round-trip inter-protection-domain
communication the authors know of, compared against seL4 in a 2019 RTAS paper. None of those
numbers is in the tree, so the evidence for them is not checkable from this checkout.

## Linux -- a production kernel whose remote wakes queue on the target's own list

**Licence, and where it was read.** `linux/COPYING:1-19` states the kernel
is provided under `GPL-2.0 WITH Linux-syscall-note`, and points at `linux/LICENSES/preferred/GPL-2.0`,
`linux/LICENSES/exceptions/Linux-syscall-note` and `linux/Documentation/process/license-rules.rst`. The
scheduler files carry their own tags on line 1: `linux/kernel/sched/core.c` is `GPL-2.0-only`,
`linux/kernel/sched/sched.h`, `linux/kernel/sched/features.h` and `linux/kernel/sched/smp.h` are `GPL-2.0`.

**Revision, and the caveat.** `89050b10`. Version 6.12.75 from `Makefile:2-5`; no reachable tags,
this being a vendor-style checkout. **THIS CHECKOUT IS NOT CLEAN.**
`git status --porcelain` in that checkout reports
`linux/arch/arm64/configs/bcm2711_defconfig` as modified. Nothing else is dirty and nothing is
untracked. None of the paths cited below is that file, but the caveat travels with every citation
into this tree: what is quoted here is HEAD plus one local edit to a defconfig.

### The mechanism

The deferred path is called the ttwu wakelist. A waker that takes it never touches the target's
runqueue at all: it pushes the wakee onto a lockless list the target owns, pokes the target, and
returns. The target performs the enqueue itself, under its own lock, out of an inter-processor
interrupt handler. The stated reason, in the scheduler's own locking overview at
`linux/kernel/sched/core.c:507-521`, is that wakeups crossing a
last-level-cache boundary would otherwise bounce runqueue state between caches, and that the
design refuses to hold two runqueue locks at once.

**The functions.** The predicate is `ttwu_queue_cond`
(`linux/kernel/sched/core.c:3884-3936`), wrapped by
`ttwu_queue_wakelist` (`:3938-3947`), which also tests the feature flag and syncs the target's
scheduler clock before handing the task over. The enqueue is `__ttwu_queue_wakelist` (`:3831`).
The drain on the target side is `sched_ttwu_pending` (`:3772-3807`, prototype at
`linux/kernel/sched/smp.h:7`). The fallback that locks the remote runqueue directly is `ttwu_queue`
(`:3958-3970`), which tries the wakelist first at `:3963` and, if refused, takes `rq_lock` at
`:3965` and calls the shared `ttwu_do_activate` (`:3668`) at `:3968`. The entry point over all of
it is `try_to_wake_up` (`:4142`).

**The list is not the scheduler's own.** There is no scheduler-private list head. The wakelist
rides the generic call-function machinery: the head is the per-CPU `call_single_queue` in
`linux/kernel/smp.c` (initialised at `:102-110`, pushed at `:410`,
claimed at `:488`), a lockless singly-linked list that the runqueue lock does not protect. The
link field in the task is `wake_entry` (`linux/include/linux/sched.h:816`), and the node is stamped
once at fork time with a type tag in `__sched_fork` (`linux/kernel/sched/core.c:4491`) -- that tag is
what routes it to `sched_ttwu_pending` rather than to a per-node function pointer. Beside the
list there is a per-runqueue hint, `ttwu_pending` (`linux/kernel/sched/sched.h:1128`), set at
`core.c:3837` and cleared at `core.c:3805`; the LIST is the authority and the hint exists so that
the idle-CPU queries at `linux/kernel/sched/syscalls.c:213` and `linux/kernel/sched/fair.c:10704,11714,12854`
do not call a CPU idle while a wakeup is in flight.

**The poke, and when there is none.** `__smp_call_single_queue` (`linux/kernel/smp.c:378`) pushes and
sends the interrupt **only when the push found the list empty** (`:410-411`), so a batch costs one
interrupt and later wakers ride the one already pending. Delivery is
`send_call_function_single_ipi` (`:112-120`) and the handler is
`generic_smp_call_function_single_interrupt` (`:455`) into `__flush_smp_call_function_queue`
(`:474`). Before sending, it asks `call_function_single_prep_ipi`, which lives in the scheduler at
`linux/kernel/sched/core.c:3815-3823`: if the target's idle task is spinning in a polling idle loop, it
sets the reschedule flag in that task's flags atomically and returns false, so **no hardware
interrupt is sent at all** and the idle loop drains the list itself. The reschedule interrupt is
a separate thing and is NOT what the wakelist uses; it is the poke for the direct path, where the
task is already enqueued under the remote lock (`resched_curr` at `core.c:1102`,
`wake_up_idle_cpu` at `:1200`, `kick_process` at `:3435`).

### The conditionality, which is the part that bears on KickOS

There are two decision points, in order.

**The early one is a spin-avoidance device, not a lock-avoidance one.**
`linux/kernel/sched/core.c:4266-4268`, inside `try_to_wake_up` and under the task's own lock, takes the
wakelist when an acquire load says the wakee is STILL referenced by another CPU's context switch
as the outgoing task, and the gate below passes for the task's CURRENT CPU -- placement has not
run yet at that point. The alternative, at `:4276`, is to SPIN until the remote switch is
finished. So the first reason Linux defers is to avoid waiting on a peer that is mid-switch,
which is the same hazard KickOS's own contract names when it rules that a deadline path is a
publication plus a completion cell and never a wait.

**The second is the gate list proper**, `ttwu_queue_cond` at `core.c:3884-3936`, checked in this
order, and it is a short-circuit chain so the order is load-bearing:

1. a scheduler-extension task refuses the deferral outright (`:3892-3893`), because the external
   policy may need the placement call to actually run;
2. the stop class refuses it (`:3896-3897`);
3. an inactive CPU, meaning one in a hotplug transition, refuses it (`:3904-3905`);
4. an affinity mask that does not contain the target refuses it (`:3908-3909`);
5. **not sharing a last-level cache ACCEPTS it** (`:3915-3916`) -- this is the positive gate, and
   the comparison is `cpus_share_cache` at `:3861-3867` on a per-CPU cache-domain id;
6. the target being the local CPU refuses it (`:3918-3919`), which is safe after step 5 only
   because the cache comparison short-circuits true for a CPU against itself;
7. a remote same-cache target with NOTHING runnable accepts it (`:3932-3933`), the comment at
   `:3921-3931` saying the runnable count rather than an idle query is used so tasks do not stack,
   and noting that the count legitimately reads zero while the target is descheduling the very
   task being woken;
8. everything else refuses it (`:3935`).

Above all of that sits a feature flag, tested at `:3940`. It is defined twice in
`linux/kernel/sched/features.h`: false under the real-time preemption configuration at `:76`, true
otherwise at `:83`. So the mechanism is on by default, OFF on the real-time configuration, and
switchable at runtime.

**So: defer when the target is in a different cache domain, or when it is remote and idle; lock
the remote runqueue directly when the target is local, or is remote-but-same-cache with other
work to do.** That conditionality is the finding for KickOS. Linux does not treat a deferred
remote wake as strictly better; it treats it as the answer when the remote runqueue state is FAR,
or when the target is about to be idle anyway and can pay the cost itself. Where the state is
near and the target is busy, it pays the lock.

Interrupts do not gate the decision; they are what makes it safe, the comment at `:4254-4259`
noting the waker holds its region with interrupts disabled so the poke cannot come back before
the other side has released the task.

### Ordering

Waker side: a full barrier after the task lock (`:4180`), a read barrier forcing the runqueue
flag to be read after the state (`:4207`), an acquire promoted from a control dependency
(`:4234`), and the acquire load of the still-running flag at `:4266` that pairs with the release
store in `finish_task` (`:4977`). The hint at `:3837` is written BEFORE the list push, so any
observer of the queue also sees it. The push itself is a compare-and-swap loop in
`linux/lib/llist.c:33`, and `linux/kernel/smp.c:398-409` records that architectures with out-of-order
interrupt delivery must add their own synchronisation -- the coherency of the list is not by
itself enough.

Drain side: the batch is claimed whole with an exchange of the head (`linux/include/linux/llist.h:266`,
used at `linux/kernel/smp.c:489`) and then reversed at `:490` to restore arrival order. There is a
should-never-happen guard at `linux/kernel/sched/core.c:3786-3787` that degrades to the spin if the
outgoing task is somehow still running, and a corrective CPU fixup at `:3789-3790`. The hint is
cleared at `:3805`, deliberately after the loop and before the unlock, the comment at `:3795-3804`
saying an earlier clear would let placement stack several tasks onto this CPU. On the no-interrupt
polling route the ordering is carried by a barrier in the idle loop at `linux/kernel/sched/idle.c:347`.

### Lock domains, acquisition order, migration, the switch, the stack

The lock domain is one per runqueue. The acquisition rule that matters here is stated at
`linux/kernel/sched/core.c:4125-4137`: the wakeup path tries hard to take exactly ONE runqueue lock,
and the wakelist exists in large part to keep it that way; the file is explicit at `:507-521`
that migrating wakeups are awkward precisely because the design refuses to hold two. Migration
proper is the mask plus a stop-class worker and is outside this row. The lock is NOT released
inside the switch on this path -- `sched_ttwu_pending` takes the target's lock once at `:3782`
and releases it at `:3806`, with the whole batch activated in between, so N deferred wakeups to
one CPU cost one interrupt and one acquisition; `linux/kernel/smp.c:583-588` is where the tagged tail
of the batch is handed over wholesale. The only documented window inside the hold is the class
callback at `:3694-3700`, which may drop and retake. Linux's kernel stack model is per thread
and is not what this row went looking for.

### Published benchmark evidence

None in tree for this mechanism. There is no file under `linux/Documentation/scheduler/` about the
wakelist at all; the rationale is entirely in source comments, the ones cited above plus the
feature-flag note at `linux/kernel/sched/features.h:79-82` and the function comment at `core.c:3825-3830`
stating the intent that the wakee's CPU pays the wakeup cost rather than the waker's.
`linux/Documentation/scheduler/membarrier.rst` touches the same barrier the ordering comments pair
against but says nothing about this path. So the design is documented and the numbers are not.

## KickOS -- the tree's own AMP window: per-pair rings and publication rules

This row exists so the survey says where KickOS already SITS in the design space rather than only
where others sit. M9.3 reuses these rings and their publication rules as written, so nothing here
is a proposal.

**Licence and revision.** CECILL-C, stated at `LICENSE:1` as an SPDX identifier and repeated as a
header on the files cited below. This tree, on branch M9; no revision is pinned here because this
branch's own commits do not survive a squash.

### The ring shape

The whole shared object is one window, and the array is by class then by destination then by
source: `kernel/include/kickos/ampwindow.h:233-243` declares a ring of a head, a tail and a fixed
slot array, and a window as a three-dimensional array of those indexed
`[class][to][from]`. So an ORDERED PAIR carries TWO rings -- a CALL ring and a REPLY ring, each
one-directional -- and a pair of nodes talking both ways has four. The selector is
`ring_for(Class, to, from)` at `kernel/amp/ampwindow.cc:295-298`. A call goes into the CALL ring
for the destination (`ampwindow.cc:440`) and an answer into the REPLY ring for the original sender
(`ampwindow.cc:1664`). The two classes are named at `ampwindow.h:142-147` with the reason on the
same lines: a CALL slot is the receiving node's record of the caller until the reply is sent, a
REPLY slot is released as it is taken.

The two indices are free-running 32-bit counters, compared unmasked and masked only for indexing
(`ampwindow.h:226-229`), each on its own cache line because they have one writer each and
different owners. `head` belongs to the producer, `tail` to the consumer
(`ampwindow.h:235-236`). Occupancy is modular subtraction (`ampwindow.cc:165-168`), and the
producer indexes off its OWN head, never off the far tail (`ampwindow.cc:399`). The slot array is
a power of two by static assertion (`ampwindow.h:79-81`), four slots deep from
`user/include/kickos/sys/abi.h:320`, and each slot is fixed at the local message bound of 256
bytes (`ampwindow.h:84`, `abi.h:506`) -- the doc records that measurement at `:1247-1249` as what
makes the one-mechanism freeze affordable.

There is a THIRD cursor on the CALL ring and it is deliberately NOT in the window. The consumer
keeps a private taken-and-released pair (`ampwindow.cc:56-69`) under the invariant that the tail
is at or behind it and it is at or behind the head, because a far side able to write it would
decide when its own slots are reclaimed.

**One ring per ordered pair is forced rather than preferred.** `docs/design-multicore.md:947-953`
gives the reason: a single inbox per node would put several producers on one head, and per
ordered pair the head has exactly one writer, which is what makes the occupancy count sound at
all. The same argument is in the header at `ampwindow.h:231-232`, adding that moving a shared
head would need a read-modify-write -- which N9 (`docs/design-multicore.md:1301`) forbids above
the seam.

### The ordering rules

**Publication precedes the raise, in one function.** `ampwindow.cc:400-406` writes the port, the
tag, the length and the payload; `:410` stores the incremented head; `:413` rings. The comment at
`:407-409` states the constraint plainly -- every slot write must stay above that store, nothing
enforces it, and a peer acquiring the index reads whatever the slot holds when it does. The seam
says the same at `arch/include/kickos/arch/arch.h:118-119`, and the GICv3 raise emits its own
barrier first (`arch/arm64/common/arch_arm64_gicv3.cc:513-515`) because the architecture orders a
software-generated interrupt against nothing. On the receiving side the copy stays ABOVE the tail
store (`ampwindow.cc:611-613`), the slot being the producer's again the instant it lands.

**The barrier is a FULL one on BOTH sides and is neither half of an acquire/release pair.**
`docs/design-multicore.md:1050-1056` is the freeze: the publication and the seat read are a store
then a load, and so are the peer's seating and its drain; release and acquire order the opposite
direction and leave this one free, and the outcome where BOTH sides read the older value is
exactly what a store buffer produces, so a barrier on one side alone does not remove it. The
primitive is `arch_ipi_fence`, declared at `arch/include/kickos/arch/arch.h:125-127` and
implemented as a full fence per backend -- `arch/arm64/armv8a/klock_armv8a.cc:173-181`,
`arch/riscv/rv64imac/klock_rv64imac.cc:224-232`,
`arch/arm/chip/rp2350/doorbell_rp2350.cc:261-268` -- each carrying the same one-line warning. Both
sides use it: the sender fences and RE-READS the seat before deciding to defer
(`arch/arm64/common/arch_arm64_gicv3.cc:498-499`), and the receiver fences before its first
service pass (`ampwindow.cc:1721-1727`) and again when a parked core wakes
(`klock_armv8a.cc:268-278`). What keeps the fast path free is that the seat flag is monotonic
(`docs/design-multicore.md:1058-1062`), so only the unseated reading is made behind the fence.
Because nothing in the emulator can witness a missing barrier, it is checked out of the linked
image: `tests/static/check_ipi_fence.sh` disassembles the function body and refuses a deleted
barrier, a one-directional operand, an undecodable body or an empty corpus.

**The ring is the AUTHORITY and a raise is only a HINT.** `docs/design-multicore.md:1025-1028`:
a peer whose interrupt-controller state is not yet published cannot be targeted, its message is
published anyway and only its NOTICE is deferred, and a skipped raise costs latency and never a
message. The sender says the same at `arch/arm64/common/arch_arm64_gicv3.cc:473-476` and COUNTS
the skipped raise (`:503-504`, readable through `arch.h:152-153`). The receiver earns it by
rescanning unconditionally: `node_service` (`ampwindow.cc:772-855`) walks every one of this node's
inboxes and takes no hint from which doorbell fired, looping the reply rings at `:786-812` and the
call rings at `:825-854` under fixed per-pass budgets (`ampwindow.h:86-92`); it runs once at init
before this node can wait on anything (`ampwindow.cc:1727`) and again before a parked core
unmasks (`klock_armv8a.cc:276-277`).

**There is exactly ONE raise that is not a hint**, and the doc carves it out at
`docs/design-multicore.md:1030-1041`: a credit return, meaning an advance of a reply ring's tail.
Its only reader is the reservation inside the take, and that runs only from a doorbell, so a
node refused for want of a reply slot has nothing of its own to publish and nothing to rescan for
it. The code rings unconditionally after any tail movement at `ampwindow.cc:616-627`, and the
header repeats it at `:299-303`.

### Can a full ring refuse

**Yes, and the two rings answer differently -- which is the reclamation asymmetry the doc
announces at `:776`.**

The CALL ring refuses at PUBLISH and never waits. `ampwindow.cc:392-396` returns a full verdict
when the ring is at depth, and an incredible far tail returns a depth verdict at `:387-391`; the
verdict set is at `ampwindow.h:164-172`. Userspace sees an errno:
`kernel/syscall/syscall_ipc.cc:130-137` maps full to busy and depth to a broken-pipe errno, and
`:242` states the rule that far endpoints never park. `docs/design-multicore.md:1064-1067` calls
that the back-pressure the contract already carries, reached in bounded time and counted at both
ends.

The REPLY ring is built so that a publication is never refused, and the refusal is moved one step
earlier to the CONSUMER'S TAKE. `docs/design-multicore.md:830-838` is the reason: a reply refused
at publish is a loss no path can retry, the reply capability having been consumed to reach the
publication at all, so the invariant is that used plus owed never exceeds the depth. The
admission test is `reply_reserved` (`ampwindow.cc:526-565`), which counts what this node owes and
short-circuits on a full reply ring at `:560-563`; the refusal is its own verdict at
`ampwindow.cc:708-712`, taken AFTER the verdict and deliberately leaving the consumer's cursor
unmoved, because a refusal that advanced it would skip that call for good. It is kept distinct
from an empty ring on purpose (`ampwindow.h:149-161`, doc `:802-808`), and the drain breaks out
on it (`ampwindow.cc:840-843`). `docs/design-multicore.md:821-828` says why the asymmetry is
required and not a preference: one ring per direction carrying both classes deadlocks, and no
depth removes it.

If a reply publication is refused anyway -- reachable only from a peer whose reply tail regressed
-- nothing blocks, spins or retries the payload. `inbound_reply` (`ampwindow.cc:1652-1683`) counts
it and defers the OBLIGATION while the payload is lost; the obligation is later discharged as a
zero-length reply (`ampwindow.cc:737-769`) and expires after a fixed number of passes
(`ampwindow.h:106-123`).

So no producer blocks or spins on a full ring anywhere in that file. The only outcomes are an
error return, a consumer-side decline, or a deferred obligation. The reason a bounded WAIT was
refused rather than merely not chosen is at `docs/design-multicore.md:1043-1048`: the publication
can be reached from a doorbell handler, where waiting spends a core with its interrupts masked on
a peer that may never start. Note for M9.3, whose row reads "the depth that cannot fill": the
depth today CAN fill and the refusal is the designed answer, so that sub-milestone is asking for
a different property than the one this window has.

Reclamation differs by class in the same direction. A CALL slot is released at the REPLY, not at
the take, and the tail advances over the leading run of released slots
(`ampwindow.cc:631-656`), so replies may complete out of order while reclamation stays in order
(`ampwindow.h:386-391`). A REPLY slot is released at the TAKE (`ampwindow.cc:613`,
doc `:817-819`). One class per ring is enforced with its own verdict and counter
(`ampwindow.cc:500-507`), and the drain order -- replies, then deferred answers, then calls
(`ampwindow.cc:784-786,814-825`) -- is a liveness rule rather than a latency preference
(doc `:847-854`).

### The single-writer discipline

On a ring indexed `[class][to][from]`, the head is written only by the source node and the tail
only by the destination node (`ampwindow.h:235-236`, `ampwindow.cc:410` against `:613` and
`:655`). The slot body is the producer's. A node's counter row is that node's alone
(`ampwindow.h:393-402`). The doorbell request and answer cells are each single-writer
(`arch/include/kickos/arch/doorbell_protocol.h:41-42`).

**What enforces it is structure, not an assertion, and the doc says so rather than leaving it
implicit.** `docs/design-multicore.md:947-957`: the receiving side CANNOT check this half, the
cell being the far node's, so the discipline sits in the shape rather than in a validation
clause. Three things carry it. The API removes the ability to speak for a peer -- the counter
calls take no node parameter and derive it, because a caller that could name the row would be
able to speak for a peer (`ampwindow.h:478-481`). No read-modify-write exists above the seam
(N9, doc `:1301-1305`, checked tree-wide by `tests/static/check_atomic_rmw.sh`): the mint rows are
written whole rather than or-ed in, an or being a read-modify-write two nodes can interleave into
a lost bit (`ampwindow.cc:1689-1692`). And the layout keeps two writers off one line
(`ampwindow.h:226`, `doorbell_protocol.h:35-36`). Anything a peer must not write is kept OUT of
the window on purpose -- the mint table, the strike counters and the private inbox cursor all
live outside it (`ampwindow.cc:22-69`).

The only writes that look like exceptions are two deliberate resynchronisations, and each writes
only the index its own node owns (`ampwindow.cc:473` and `:195-196`).

Configure time carries the rest (N10, doc `:1307-1315`): the shared-kernel predicate is
`cmake/smp_predicate.cmake` keyed on the model and not on a core count, driven from
`CMakeLists.txt:577-586`, and the AMP-shaped refusals are at `CMakeLists.txt:611-631`. The
node-to-core map must be stated or the build is refused (`kernel/amp/ampmap.cc:6-8,25-26`), and
the posture asserts are at `ampwindow.h:43-55`.

### What the contract says it does NOT guarantee

`docs/design-multicore.md:1083-1097` names two limits, and they are recorded here because a
survey that only listed the guarantees would misstate where KickOS sits. First, nothing stops an
application building a cross-node wait cycle: the machine guarantees only that a reply is always
sendable, priority does not cross (N6e, `:672`), and avoiding a cycle is a partitioning decision
taken across two configurations that no green run witnesses. Second, a peer's memory is not out
of reach: every node maps the same writable kernel RAM, so on a part whose bus enforces nothing
the boundary is DESCRIBED and not enforced, and the validation defends a node against a malformed
peer and never a hostile one. The same warning is in the header at `ampwindow.h:15-20`.

The receiving side's own obligations are at `docs/design-multicore.md:959-1021`, and the shape
that has bitten three times is worth carrying into any comparison: a lookup keyed on the wrong
space -- a handle where an index was stored, a row keyed by sender where both ends were needed, a
band sized for one receiving node where every ordered pair has records. None faults and none
refuses; the lookup resolves to nothing and the path simply does not happen. All three were found
by an arm asserting a POSITIVE outcome, and an arm checking that a message did not arrive wrongly
would have passed all three.

### The nine points, where they apply

Lock domains, acquisition order and whether a lock is released inside the switch do NOT apply:
the window takes no lock at all, and that is the design rather than an omission. Remote wake is
the doorbell, and it is a hint over an authoritative ring as described above. Migration does not
apply -- an AMP node is a single-core kernel (N6, `docs/design-multicore.md:466`) and a name
crosses between kernels, never a capability (N6d, `:645`). The kernel stack model is unchanged by
this mechanism. Published benchmark evidence: the masked-copy cost is measured and banked in
`TODO.md:4258-4267` -- a 256-byte span against a floor at zero bytes on two parts, with the
conclusion that two such copies happen per message and that the mechanism waits for M9.3 rather
than being built early.

### Where this puts KickOS in the design space

Against row 1, the shapes rhyme closely and the ORDERING MECHANISM differs in a narrower way than
it first looks. Composite also uses one ring per ordered core pair with a power-of-two depth, also
refuses rather than waits when the ring is full, and also publishes before it raises; it too has no
barrier between its slot writes and its index publication
(`composite/src/kernel/include/ipi_cap.h:110-113`), and it too fences after. **This tree's publish
path has no barrier there either**, and `ampwindow.cc:407-409` says so in its own comment; what
orders it is the head being a release store (`ampwindow.h:226-228`). So the difference is a release
store against TSO plus a volatile index, not a full barrier against none. The full fence this tree
demands on BOTH sides (`docs/design-multicore.md:1050-1056`, held by
`tests/static/check_ipi_fence.sh`) answers a different pairing: the publication against the peer's
seat read, a store then a load, which release and acquire do not order. Against row 2,
Linux defers CONDITIONALLY and takes the remote lock when the state is near; KickOS has no remote
lock to take in the AMP shape, so its equivalent of that conditionality is the decision to skip a
raise, not the decision to defer an enqueue.

## KickOS -- the tree's own routed controller-mask touch

**Licence and revision.** CECILL-C (`LICENSE:1`), this tree, on branch M9; no revision is pinned
here, this branch's own commits not surviving a squash.

**The file, found.** `kernel/irq/irq_route.cc` (178 lines), with its
header at `kernel/include/kickos/irq_route.h` (41 lines). The other
matches for that name under the tree are object files and worktree copies, not sources.

### What it is, stated plainly

**It is a control-plane touch of an interrupt-controller MASK, and it is NOT an interrupt
handover. No thread moves.** The code shows it three ways. The whole payload that crosses is a
line number and an operation byte (`irq_route.cc:66-71`) -- no thread pointer, no handler, no
binding, no stack, no capability. The whole work performed on the far core is one of three
controller calls, mask, unmask or clear-pending (`irq_route.cc:39-59`), and the service body calls
nothing else (`:173`). The far side's service body then writes only its answer sequence (`:174`)
and touches neither the scheduler nor any queue; the file contains no scheduler call at all.

The reason it is the TOUCH that travels and not the thread is stated in both places.
`irq_route.cc:10-12`: a server holding the wait capability is PLACED on its line's core and a
grant that cannot reach it is refused, but a passer-by is never moved, so its touch is routed
instead. `docs/design-multicore.md:289-295` gives the case -- the console handover masking the
kernel's own transmit line inside a publish call is a thread merely passing the line on its way
to something else, its affinity is its own and narrowing-only, and re-placing it as a side effect
of an unrelated syscall would be a yank. Thread movement is a different mechanism entirely
(`kernel/irq/irq.cc:705-733`), on the wait-capability path, and it asks with a refusal rather than
forcing.

The handler stays on the owning core by hardware rather than by convention: on the LX6 the
interrupt matrix routes a source into exactly one core's bank and sinks it in the other
(`arch/xtensa/lx6/arch_xtensa.cc:841-842`), and nothing in `irq_route.cc` changes that routing.
What makes the routing necessary at all is that the words those three seam members
read-modify-write are image-wide, so a touch from the wrong core loses a mask or a latched raise
with no fault anywhere (`irq_route.cc:6-8`). The LX6 ships a debug guard that terminates on a
wrong-core touch (`arch/xtensa/lx6/arch_xtensa.cc:254-266`, called from `:798`, `:837`, `:857`).

### The protocol, and the wait

Entry points: `irq_line_op` (`irq_route.cc:117-147`) for a caller that is not the routed core by
construction, and `irq_line_op_local` (`:110-113`) for one that is, every ISR-context caller being
in the second class. They are two entries rather than one runtime branch on purpose
(`:105-109`): the red-zone gate walks the callgraph and a runtime test is invisible to it, so
merging them would put the wait's panic tail on the interrupt path. The serving half is
`kickos_irq_route_service` (`:159-176`). This file is the only one in the kernel layer permitted
to call the three controller seam members, checked by
`tests/static/check_irq_line_op_sole.sh` against a one-file allowlist.

The cells are one row per asking core, one slot per ordered pair, each written by one core
(`irq_route.cc:61-80`). The sequence is: write the payload, then bump the sequence whose release
store the far side acquires, then raise, then wait (`:87-89`, `:99-100`) -- the same publication-
before-raise rule as row 3. **The answer IS the completion and there is no second cell**
(`:90-93`), the precondition being that every backend's doorbell service body drains after its
request snapshot and before it stores its answer. That ordering is checked out of the sources by
`tests/static/check_route_service_order.sh` across three named backend bodies, and a missing body
is a failure rather than a pass.

The wait is the generic doorbell rendezvous at
`arch/common/doorbell_protocol.cc:274-303`. It spins on the answer SEQUENCE and never on the
raise (`:267-273`), because on a part whose wake can be erased by a set racing a clear a
raise-watching wait would sit until its bound expired and then kill the machine over a race whose
whole cost is meant to be latency. It polls its OWN doorbell inside the loop (`:299`) because two
cores can each be an initiator waiting on the other. Local interrupts are OFF throughout: every
kernel caller is inside an interrupt lock (`kernel/include/kickos/irqlock.h:33-38`; call sites at
`kernel/irq/irq.cc:210,213,386,541,893`), and the service body on the far side runs masked too.
There IS a bound and it is FATAL rather than a graceful exit: a fixed spin count
(`doorbell_protocol.cc:43-45`) after which the core prints and terminates (`:290-296`). Neither
`line_op_ask` nor `irq_line_op` has a return value, so there is no failure path back to the
caller -- the only two exits are answered or terminated. ISR context is refused
(`irq_route.cc:133-144`): the ask is taken only outside a handler, because a rendezvous entered
from one would block an interrupt on a peer, and the release build still performs the operation
locally because dropping it would turn a wrong-core touch into a MISSING one.

### What it is reachable on

**The backend hook is `arch_irq_line_core`** (`arch/include/kickos/arch/arch.h:721-723`, with the
no-owner sentinel on the same lines). Two production definitions exist in the tree.

The **LX6 (Xtensa, ESP32)** answers a real owning core for a bound line
(`arch/xtensa/lx6/arch_xtensa.cc:843-850`, resolving through the bind-table scan at `:234-249`),
because its matrix routes a source into exactly one core's bank. Under one core the scan compiles
out and it answers no owner.

The **two shared-kernel backends define the hook NOWHERE**. This was checked rather than assumed:
a grep over `arch/arm64/**` and over `arch/riscv/**` returns zero definitions, so the ARM GICv2
and GICv3 backend and the RISC-V backend both link the arch-neutral lone-translation-unit
fallback at `arch/common/arch_irq_line_core_default.cc:12-16`, which answers NO OWNER
unconditionally. Its own comment states the case (`:7-8`): a controller that raises every line on
every core, or one that routes by a mechanism the kernel is not told about, has no core to name.
Resolution is by the lone-translation-unit rule stated at `arch/CMakeLists.txt:10-40`, with the
default listed at `:51` and the one-symbol rule checked per board off the link map by
`tests/static/check_seam_defaults.sh`.

With no owner the touch is performed LOCALLY: `irq_line_op` asks only when the owner is valid and
is not this core (`irq_route.cc:133`) and otherwise falls through to the inline path (`:146`).
The doc records the same at `docs/design-multicore.md:297-299`. The file says it about itself at
`irq_route.cc:14-17` -- compiled on three backends and firing on one, and not to be deleted as
dead, because a backend that starts routing needs no kernel change. The whole routed half is
compiled out at one kernel core (`irq_route.cc:61,102,119,145,149,177`). There is also a bound
check on the owner before it is used as a shift and an index (`:127-132`), against the kernel core
count rather than the image core count, because under AMP a kernel drives fewer cores than the
image has. `docs/design-multicore.md:296-303` records the measurement: on the ESP32 board exactly
one line is routed, the console's transmit line.

### Is it the tree's one blocking cross-core wait

In the KERNEL layer, yes, other than the benchmark harness -- it is the only one that blocks on
behalf of a driver-visible operation. The claim as the brief states it needs one qualification,
and it is recorded rather than smoothed over: the ARCH layer has three more waits on the same
primitive and the same fatal bound -- the ARM64 instruction-side rendezvous
(`arch/arm64/armv8a/klock_armv8a.cc:195`, called from the executable-mapping paths in
`arch/arm64/armv8a/aspace_armv8a.cc:700,771,806`), the RISC-V translation rendezvous
(`arch/riscv/rv64imac/klock_rv64imac.cc:251`, from
`arch/riscv/rv64imac/aspace_rv64imac.cc:323,883,912`), and the one-shot doorbell bring-up
selfcheck (`arch/common/doorbell_protocol.cc:147,167`). Beside those sit two spins that are not
this primitive: the kernel-lock acquire, which spins until a remote core releases and polls its own
doorbell inside the loop and is UNBOUNDED (`klock_armv8a.cc:229-240`), and the bounded peer-start
await at `kernel/init/kmain.cc:203-220`. The AMP window of row 3 spins on nothing at all.

### The nine points, where they apply

Lock domains: none taken by the routed path. The service body takes no kernel lock
(`irq_route.cc:155-156`), because the seam declares those three members self-bracketed with local
masking and needing no caller lock (`arch/include/kickos/arch/arch.h:700-703`). Acquisition order:
the caller already holds the interrupt lock and the routed touch adds nothing under it. Remote
wake, migration, lock-in-switch and the kernel stack model do not apply -- nothing here wakes,
moves or switches a thread. Published benchmark evidence: none specific to this path; the doorbell
round trip it rides is what `roadmap.md` asks M8.7's instrument to carry.

### Where this puts KickOS in the design space

This is the one place KickOS does what its own contract elsewhere refuses. The multicore file's
own words, under its ruling on the routed mask touch, are that on a deadline path the shape is a
publication plus a
completion cell and never a wait, and that a blocking cross-core wait spends the asker's core
interrupt-masked on a peer that may be spinning for the very lock the asker holds. The routed
touch IS such a wait, and the survey records it as the exception the tree already carries rather
than as a finding. It is reachable on one backend, for one line on one board today, and its
failure mode is a terminate rather than an errno.

## Named and absent from this box

These five are named so each gap is a RECORD rather than something the survey implies by silence.
None was fetched, and nothing here describes an internal that was not read -- an absent row says
what the reference is FOR, not what it does. An absent row is never a verdict about the project.
The wording each row answers to is the M9.0 paragraphs of `roadmap.md` and `TODO.md`; the box
inventory is `CONTEXT.local.md` under its heading naming the prior-art trees on this box,
whose closing line names L4Re, NOVA/Hedron, Hubris, Barrelfish and Genode as absent and
records QNX as documentation-only.

**A multikernel with a published crossover between shared memory with locks and message passing
-- Barrelfish.** The description names it closely enough to identify: a multikernel is Barrelfish's
own term, and the crossover it asks for is the published comparison of a shared data structure
protected by a lock against explicit message passing as core count rises, which is the argument
that project is built on. `CONTEXT.local.md` lists Barrelfish among the trees absent from
this box. What it would speak to in M9 is the milestone's central question rather than a detail
of it: KickOS's contract already splits the fleet into a shared kernel where the hardware earns
it and AMP where it does not (`docs/design-multicore.md:3` and section 1), and M9's stop
condition asks at what point the coarse lock stops being the right answer. A published crossover
is exactly the shape of evidence that question wants, and the roadmap forbids using it as a
ranking. Adding it needs a source checkout; the crossover argument itself is in the project's
papers, so documentation alone would answer the narrow question and only a checkout would let a
finding be cited by path the way M9.0 requires.

**A capability kernel that binds every execution context to a core -- NOVA, and its Hedron
line.** The roadmap does not name it, but `CONTEXT.local.md` lists NOVA/Hedron among the
absent trees and the description matches that design's execution-context-per-core rule, so the
identification is recorded as probable rather than certain. What it would speak to is M9.2's
ownership question and M9.5's lifetime precondition: if every context has a home core by
construction, a whole class of cross-core resolve disappears, and KickOS is deciding right now
how much of that it wants (the mask and the default core set at `docs/design-multicore.md:1732`
onward, and the placement rules at `:1804`). **This box is not empty of the idea, only of that
project**: Composite, read in row 1 above, already refuses a thread, temporal or receive
capability resolved from a core other than the one recorded in it
(`composite/src/kernel/include/shared/cos_types.h:207`). So the gap is
a second, differently-argued instance, not the concept. Adding it needs a checkout.

**A scheduling-context and budget design -- TWO ROWS, because the roadmap's single one named a
gap that neither project has.** The roadmap listed one such design among the references not on
this box and elsewhere called the same design required reading before M10's temporal half is
proposed. Both candidate projects have their KERNEL half checked out here, and what is missing
from each is a different thing, so the row is split rather than resolved one way. Neither is a
verdict about either project, and the two designs are not ranked against each other.

**Row A -- seL4's mixed-criticality scheduling contexts. Present; the ARGUMENT is what is
absent.** `seL4` at revision `28b8f4c4` carries `seL4/src/object/schedcontext.c`,
`seL4/src/object/schedcontrol.c` and `seL4/src/kernel/sporadic.c`, all under the configuration
symbol `CONFIG_KERNEL_MCS`, whose own definition line in `seL4/config.cmake` calls that
configuration not verified. `seL4/include/kernel/sporadic.h:1-30` states the budget model in its
own header comment -- a period and a queue of refills, each refill an amount and a time, the sum
of the refills being the scheduling context's budget, implemented after the published
sporadic-server correction and deliberately without the priority management. Those files carry
`SPDX-License-Identifier: GPL-2.0-only`. The manual covers it at `seL4/manual/parts/threads.tex`.
**Absent**: the published design paper that argues the mechanism, the verification tree, and any
configuration on this box that builds the mixed-criticality variant. So M10's temporal half can
read the mechanism here today and cannot read the case for it.

**Row B -- Fiasco.OC's scheduling contexts. The kernel half is present; the half that DRIVES it
is absent.** `fiasco` at revision `6437d44e` carries `fiasco/src/kern/sched_context.cpp` with
fixed-priority, weighted-fair-queue and fixed-priority-plus-weighted-fair-queue variants beside
it, so the shape is readable and it is a different shape from row A's. **Absent**: the L4Re
userland, which `CONTEXT.local.md` already lists among the trees not on this box and which is
what a scheduling context is actually programmed through. So the mechanism is readable and its
use is not, which is the mirror of row A's gap rather than the same gap twice.

**A message-passing system with priority inheritance across the message path -- the description
points at QNX, and the identification is left OPEN.** Neither `roadmap.md` nor `TODO.md` names a
project for this one; both describe the property. `CONTEXT.local.md` records that QNX is
documentation-only on this box, and QNX's synchronous message passing with priority inheritance
along the message path is the design the description fits best, so that is the reading -- but it
is a reading, and the survey records it as unnamed by the source rather than settled. What it
would speak to is a question KickOS has already frozen for the AMP shape and has open for the
shared one: `docs/design-multicore.md` N6e (`:672`) rules that priority does NOT cross between
nodes and that the two scales are not comparable, while the effective-priority recompute over an
unbounded donor set is one of the paths `roadmap.md` names as owing a capping mechanism. A
message path that carries inheritance is the other answer to that, and reading one would say what
it costs. There is no source to fetch: this is documentation only, so adding the row means
reading published specification rather than checking anything out, and any finding from it could
never be cited by path into a tree the way M9.0 asks.

## Where KickOS sits in the design space

Ten readings, each one a placement and none of them a ranking.

**The lock-domain axis has both ends occupied, and KickOS occupies both of them at once.**
The shared-kernel backends sit where seL4, ThreadX, ChibiOS and RT-Thread sit: one lock over
everything. The AMP window sits where Composite sits: no lock at all. RTEMS and Fiasco are at
the far end, per-instance and per-object locks with no global lock left, and RTEMS reaches it
with a runtime cycle detector for the one lock graph it cannot order statically. M9's second
outcome is one step along that axis and not a jump to its end.

**Whether the kernel lock is held across the context switch splits the set almost evenly, and only
one project records a reason.** seL4, RT-Thread and ChibiOS hold it; Zephyr, ThreadX and
FreeRTOS release it, Zephyr dropping the lock word one statement before `arch_switch` with
interrupts still masked. NuttX does neither, its lock belonging to a thread rather than to a
core, so the switch re-evaluates ownership. **Zephyr is the one that writes down why**, at
`zephyr/kernel/include/kswap.h:36-53`: the scheduler lock cannot be released by the switched-to
thread, which is what forces the release before `arch_switch` and what its busy-wait on the outgoing
handle exists to cover. The others record no rationale, no measurement and no note about the
contention the choice creates. KickOS holds, and a decision to change that has one argument in this
set to read and no measurement anywhere.

**The published inbox is what a project builds when it does NOT have a lock over the peer's
state, and KickOS proposes to build one while keeping the lock.** Eleven of the twelve external
rows have a cross-core wake at all, RIOT's kernel being single-core, and eight of those eleven
wake a peer by reaching into shared or peer-owned scheduler state under a lock and then sending a
hint. The three that publish into something the target owns each lack the lock that
would let them do otherwise: Composite has no locks, Fiasco has no global lock, and Linux
refuses to hold two runqueue locks at once and says so at the mechanism. `roadmap.md` already
rules that ownership and the rings land under the lock in both outcomes, and the survey's
finding is that this combination is not one the set demonstrates. It is not thereby wrong; it
is untested ground, which is the argument for landing the protocol while there is still one
lock and no race to chase.

**Linux's deferral is CONDITIONAL, and the condition is the interesting part.** It defers when
the target is in a different cache domain or is remote and idle, and pays the remote lock when
the state is near and the target is busy; the whole mechanism is off on its real-time
configuration. So the reference for a remote-work inbox is not "publication beats reaching in",
it is "publication beats reaching in WHEN the state is far". KickOS has no measurement that
says which side of that line its fleet sits on, and the entry envelope says M8.12 cannot
produce one.

**No project in the set can refuse a thread wake for capacity.** Every one uses intrusive
lists whose storage is in the thread, or an idempotent bitmask, or a single word written by
fetch-or. Two drop a cross-core NOTIFICATION on a full hardware FIFO -- ChibiOS and the
FreeRTOS RP2040 port -- and both justify it by the state already being published, so the
signal is only a request to look. That is the same shape as this tree's rule that a ring is the
authority and a raise only a hint. `roadmap.md`'s ruling that a wake or a reply is never
refused on full therefore has no counterexample here, and the surveyed answer to "what if the
notification is dropped" is idempotence rather than capacity.

**A blocking cross-core wait is the norm, and on the ordinary paths nobody bounds it.** seL4
blocks for TLB shootdown,
FPU handover and thread stall; Fiasco blocks a migration requester on a peer flag; NuttX blocks
a cross-call from inside its own global critical section; RTEMS busy-waits a multicast; Zephyr
spins on an outgoing thread's handle WITH the scheduler lock held; RT-Thread, ThreadX and
FreeRTOS each have their own. This tree has one too, the routed controller-mask touch, and its
own contract calls that shape wrong for a deadline path. So the exception KickOS already
carries is the surveyed norm. **The bounds that do exist sit on failure paths rather than ordinary
ones**: NuttX bounds its panic pause in time and gives up, and this tree's own doorbell rendezvous
is bounded in spin count with a fatal tail. On the paths a deadline runs through, no tree in the set
states how long a core may be held by a peer.

**Three surveyed kernels and this tree make the WAIT a point at which the waiter executes work
for a peer**, and this bears directly on M9.1's open question about opening the interrupt mask
between spin attempts. seL4's CLH spin loop dispatches remote-call IPIs, and its blocking
remote call only works because of it. Zephyr's AArch64 spin relax services a pending FPU-flush
IPI, its comment naming the holder-waiter deadlock it prevents. RTEMS's multicast waiter keeps
servicing its own messages so the system progresses even if IPIs are not working. And this
tree's own doorbell rendezvous polls its own doorbell inside the loop, because two cores can
each be an initiator waiting on the other. Each states a local reason; whether this is a
general requirement of the shape is not answerable from the trees.

**Every fairness bound in the set is in NUMBER OF PREDECESSORS, never in time.** seL4's CLH and
RTEMS's ticket lock are both strict FIFO, RT-Thread's aarch64 lock is a ticket, and ThreadX
interposes a FIFO wait list on some ports and not others with no note explaining the split.
Against that, Zephyr ships a ticket lock marked experimental and off by default and states that
its default can live-lock, and NuttX's ticket lock is off by default with its own design
document leaving the starvation question open. So M9.1's "fair arbitration per backend" has
precedent in both directions. What no project supplies is the term that turns a predecessor
count into a wait: none publishes a worst-case kernel-entry or critical-section duration for
its SMP configuration, which is the same term the entry envelope names as absent here.

**On the stack model, KickOS is with the majority and seL4 pays a specific price for being
outside it.** Every surveyed kernel but one keeps per-thread privileged execution state. seL4 is
the one that shares a stack per CPU, and it does not save an operation's progress -- it makes
long operations restartable from the original program counter. That is the property the stack
investigation turns on, and it is a design fact rather than a vote.

**And the assurance argument does not point where it is usually read as pointing.** seL4's own
`CAVEATS.md` states that SMP is supported and NOT verified, that in the unverified
configurations ordinary C defects are possible and not excluded as they are in the verified
ones, and that the intended route to assurance on multicore hardware is a static multi-kernel,
one instance per core over disjoint memory, with verification of that on the roadmap. That is
architecturally the shape of this tree's AMP window rather than of its shared kernel. It is
recorded here as what the project says about itself, and it is not an argument for or against
anything KickOS does.

## What the survey does not settle

### Part A: what these five do not settle

- **What any of this COSTS.** Four of the five publish no numbers at all for their own SMP or IPC claims;
  they ship instrumentation, harnesses and regex-checked output formats instead. Only RTEMS commits
  figures, and those are tied to two specific parts (a PowerPC P1020E and a simulated GR740) and to its
  own lock and dispatch paths. Nothing here tells KickOS what a lock domain choice costs on an A53, and
  no cross-project number in this survey would be comparable even if it existed.
- **The wall-clock bound behind every fairness claim.** seL4's CLH and RTEMS's ticket lock both give a
  FIFO bound in NUMBER OF PREDECESSORS. Neither project states a bound on how long a holder may hold,
  which is what actually determines the wait; RTEMS states the requirement that a holder not be
  interrupted indefinitely without stating a figure, and seL4's caveats mention non-preemptible
  long-running operations without quantifying them. No tree here publishes a worst-case kernel-entry or
  critical-section duration for its SMP configuration.
- **How to CHECK an acquisition order.** Not one of the five ships a general lock-order validator. Zephyr
  validates depth, ownership and hold time but explicitly not order; RTEMS ships a runtime cycle detector
  for the thread-queue graph only; Fiasco orders one peer-lock pair by object address; NuttX has nothing.
  Whether a stated order can be mechanically enforced is left open by all five.
- **Whether a deferred wake has a latency bound.** Every project coalesces, and two admit the seam.
  Zephyr carries a TODO that one reschedule path dispatches IPIs without the scheduler lock and can delay
  rescheduling; NuttX's single-slot hint means the core that answers a wake is not settled at send time,
  since an unsuccessful delivery is FORWARDED to a different core. No tree bounds the resulting delay.
- **Whether the blocking cross-core paths are safe where they are used.** NuttX makes a blocking
  cross-call from inside its global critical section, with no comment and no assertion explaining it. I
  record the shape; nothing in that tree settles whether it is intended.
- **What the two microkernels' own documentation says beyond this checkout.** Fiasco ships only a
  debugger manual here and asks that benchmark results be reviewed before publication. RTEMS's Software
  Engineering manual and requirements prose are in separate repositories not checked out, so the
  expectation that RTEMS states its lock nesting explicitly is unconfirmed rather than refuted.
- **Whether Fiasco's helping has a bound.** A thread can execute on a core that is not its home core for
  the duration of a helping episode, and the migration requester spin-waits on a peer flag. Neither
  duration is bounded in the tree.
- **One cross-cutting shape this survey found but cannot evaluate.** Three of the five make the LOCK WAIT
  or the CROSS-CORE WAIT a point at which the waiter executes work on a peer's behalf: seL4's CLH spin
  loop dispatches remote-call IPIs, Zephyr's AArch64 spin relax services a pending FPU-flush IPI to avoid
  a holder/waiter deadlock, and RTEMS's multicast waiter keeps servicing its own messages so progress
  continues even if IPIs are not working. Each tree states a local reason. Whether this is a
  general requirement of the shape or three independent workarounds is not answerable from the trees.

### Part B: what these five do not settle

- **No numbers.** Not one of the five publishes a measured SMP or cross-core IPC result in its tree.
  ThreadX and RIOT ship the best-developed harnesses (Thread-Metric, `RIOT/tests/bench/`) and both are
  single-core in construction; RT-Thread's `src/utest/perf/` is likewise per-primitive and
  single-core. So the cost of any of these lock designs, including the cost of holding a lock across
  a switch, is not answerable from these trees.
- **Why held-across-the-switch was chosen.** RT-Thread and ChibiOS both hold the kernel lock across
  the switch, ThreadX and FreeRTOS both release it. Neither pair records a rationale, a measurement,
  or a note about the contention this creates.
- **Whether the dropped notification is ever wrong.** ChibiOS and the FreeRTOS RP2040 port both drop
  an inter-core notification on a full FIFO, both justified by a pending interrupt covering it.
  Neither tree contains a proof, a test, or an argument for the case where the pending message is
  consumed between the space check and the peer's next scheduler pass.
- **The wait list's cost.** ThreadX ships a FIFO fairness list for its kernel lock on some ports and
  not others, with no note anywhere in the tree explaining the split or what it costs. Whether the
  A53 port omits it on purpose or by omission is not determinable from the source.
- **Bounded cross-core wait.** RT-Thread's `SMP_CALL_WAIT_ALL` and its per-slot spin, and FreeRTOS's
  `prvCheckForRunStateChange`, are both unbounded loops with no stated worst case. None of the trees
  states a bound on how long a core may be held by a peer.
- **Per-port confirmation.** Every lock detail above was read on one port per project (ThreadX
  Cortex-A53 with the A7 and A78 wait-list variants noted, RT-Thread aarch64, ChibiOS RP2 on
  ARMv6-M, FreeRTOS RP2040). ThreadX has already demonstrated that this layer varies by port, so
  none of these should be taken as a project-wide statement about the lock algorithm.

### Part C: what these rows do not settle

**They do not settle the capability-lifetime question, and row 1 was not read to settle it.**
`roadmap.md`, under capability lifetime as M9.5's named precondition, gives three candidates and
rules that the protection stays until a
replacement is DESIGNED. What this part establishes is only what the third candidate IS in the one
implementation on this box: a constant worst-case period keyed on the cycle counter, with the slot
held in a quiescing state rather than freed, and reuse refused until the period has passed. Two
things that would be needed before it could be proposed here are absent. The period's soundness
rests entirely on a worst-case kernel-execution bound, and KickOS's own inventory of unbounded
paths, which `roadmap.md` inventories under what may not be used as a bound, is the reason it does
not have one yet. And the counter Composite
reads is assumed consistent across cores by the comment at
`composite/src/platform/i386/chal_pgtbl.c:154-160`; whether that
assumption survives translation to this fleet is not a question this row asked.

**They do not establish that a deferred remote wake is the right shape.** Row 2's finding is the
CONDITIONALITY, not the mechanism: Linux defers when the remote runqueue state is far or the target
is idle, and pays the lock otherwise, and turns the whole thing off on its real-time
configuration. A survey row cannot say which side of that line KickOS's fleet sits on, because the
measurement that would answer it is M8.7's instrument and M9's own entry envelope, not a reading of
another tree.

**They do not price anything.** No build, no test, no bench and no target was run for any row
here. Every number quoted is one already banked in this tree (`TODO.md:4258-4267`) or one another
project states about itself, and in Composite's case the published numbers are not even in the
checkout.

**Row 1's answers about user-level reclamation are blocked and stay blocked.** The two submodule
directories that carry it are empty in this checkout. A `make init` would fill them, which is a
write to a surveyed tree and was not done; if that question matters later it is a separate,
deliberate act.

**Row 1's SMP reading is x86-only and cannot be transferred.** The ARM port is a uniprocessor
port. `roadmap.md` already rules that a single-core kernel is not an SMP precedent, so
nothing about how that design behaves on a weakly ordered part is established here.

**Rows 3 and 4 describe the tree and decide nothing about it.** In particular: the observation
that M9.3's row asks for "the depth that cannot fill" while the window's depth CAN fill and
refuses is recorded as a question for that sub-milestone, not answered here. So is the observation
that the routed touch is the one blocking cross-core wait in a tree whose own contract says a
deadline path never waits.

**Three of the five stay absent and two turned out to be partial.** The scheduling-context row
was checked and split in two, because both candidate projects have their kernel half on this box
and each is missing a different thing: seL4's mixed-criticality sources are here and the paper
arguing them is not, while Fiasco.OC's scheduling contexts are here and the L4Re userland that
programs them is not. So M10's temporal half has a mechanism to read today and no case for either
shape. Barrelfish, NOVA/Hedron and the message-path inheritance row were not fetched, on purpose.
Nothing here is a verdict about any of them.

**Two claims the M9.0 item requires were NOT checked by this part.** `TODO.md`'s M9.0 item says two
things must be checked before either is cited: whether the most-cited precedent's synchronous
communication fast path takes its queue lock on the same acquisition, and what that project's own
documents say its verification envelope excludes. Both are about seL4, which is not a row of part
C. They are flagged here so the milestone does not close believing part C covered them.
