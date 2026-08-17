<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->

# SMP candidates and the staged model (M5)

> **Status: EXPLORATORY** -- a spike, not a contract. Nothing here is implemented. M5 is the
> milestone after the M4 driver era.

Status: DESIGN SPIKE. Forward-looking. This is M5. No build/runtime code change
here -- it ranks the multi-core parts in hand by the gate that decides whether a
shared kernel is reachable at all, and it carries the cross-core IPC ring plus
doorbell design. It does NOT decide AMP versus a shared kernel: see the OPEN section
below, which is the one thing to settle before any SMP code is written.

## The real gate

SMP feasibility is not decided by "does it have two cores." It is decided by two
coupled facts:

1. **The inter-core atomic primitive.** A shared kernel needs mutual exclusion
   that holds ACROSS cores -- masking local interrupts does nothing to another
   core. That requires either an architectural atomic (LDREX/STREX exclusives, a
   compare-and-swap) or a hardware spinlock block. Either one is sufficient, and
   WHICH one the silicon has does not cap how fine the locking can get: it caps
   how a lock is IMPLEMENTED, and which lock ALGORITHMS are reachable. A part with
   32 hardware spinlocks and no exclusives can carry 32 independent locks; what it
   cannot carry is an algorithm needing atomic exchange or fetch-add, such as a CLH
   queue lock or a sense-reversing barrier.
2. **Arch-switch maturity in KickOS.** SMP reworks the switch and lock paths, so a
   part whose single-core switch path is already solid and well-understood is a far
   safer SMP vehicle than one whose switch path is still fragile.

Ranked against that gate, the fleet sorts cleanly.

## Candidate ranking

### RP2350 (2x Cortex-M33) -- BEST
armv8-M exclusives (LDREX/STREX) enable fine-grained locking, not just a single
big lock. It is well documented, and the MPU (PMSAv8) and TrustZone work already
lands here, so the arch-switch and protection paths are the ones under active
development. Bonus for the uniform thesis: the RP2350 ships 2x Cortex-M33 AND 2x
Hazard3 (RISC-V), with the core set selectable at boot -- so the SMP model can be
proven on BOTH an ARM and a RISC-V variant of ONE chip, holding the board constant.
That is a gift: it demonstrates that the SMP model is a uniform goal with a
per-arch mechanism (exclusives on M33, A-extension atomics on Hazard3), on
identical silicon.

One inversion to hold onto, because it runs opposite to the intuition that ranks
this part first. Erratum RP2350-E2 aliases the SIO spinlocks, so pico-sdk DEFAULTS
`PICO_USE_SW_SPIN_LOCKS` to 1 on this part and emulates a spinlock with
`ldaexb`/`strexb`, needing `ACTLR.EXTEXCLALL` force-set per core before exclusives
are even observed across cores. On RP2040 the same SIO block works natively. So the
exclusives here are a WORKAROUND for a silicon defect in the primitive RP2040 uses
successfully -- not a capability upgrade that unlocks finer locking. RP2350 still
ranks first, on documentation, PMSAv8 maturity and the dual-ISA proof, but not
because its atomics buy locking granularity.

### RP2040 (2x Cortex-M0+) -- BIG LOCK FIRST, AND NOT CAPPED THERE
armv6-M has NO LDREX/STREX and the SIO bus is non-atomic, so the SIO hardware
spinlock block (32 spinlocks) is the only cross-core primitive. That is enough for
a single big kernel lock and, contrary to what this document previously claimed, it
is also enough for more than one: FreeRTOS V11 ships a dual-core RP2040 port
(`portable/ThirdParty/GCC/RP2040` in the local FreeRTOS-LTS tree) that builds TWO
kernel locks from two distinct SIO spinlocks, `PICO_SPINLOCK_ID_OS1` and `OS2`,
claimed at startup. The kernel above them performs no atomic read-modify-write at
all, so a plain test-and-set by peripheral read is the whole hardware requirement.

So what actually bounds RP2040 is the SIO spinlock COUNT and hold-time discipline,
not the absence of exclusives. Two independent limits do apply, and they are the
honest reasons to keep this part conservative: its critical sections mask with
PRIMASK (all interrupts off) rather than a graded BASEPRI, which a copy under the
lock will feel; and no lock ALGORITHM needing atomic exchange is reachable, so a
CLH-style FIFO queue lock is not available and a lock here cannot offer bounded
FIFO fairness.

Value: cheap bring-up of the SMP MODEL (core-1 launch, per-core run-queue,
per-core tickless state, the lock discipline) on the simplest possible dual-core
part.

### ESP32 WROOM (2x Xtensa LX6) -- DOABLE BUT HARDEST, DEFERRED
It HAS S32C1I (a compare-and-swap), so cross-core locking is possible in
principle. But the windowed ABI makes SMP context handling genuinely complex, and
the prerequisite only just landed: the single-core windowed fresh-thread-start bug
was FIXED (commit 700ec98, the rfe-start work). That bug was the blocker -- a
windowed thread that could not even start cleanly single-core could not be an SMP
context. SMP on LX6 is now UNBLOCKED, but it stays ranked last and is gated on the
staged model being proven on M-profile first. Do not open Xtensa SMP until the
model is solid on RP2350 / RP2040.

### The rest -- single-core, or not symmetric
K64F, XMC4800, STM32F411, RX72M are single-core: out of scope for SMP. The
ESP32-C6 is a single high-performance RISC-V core plus a non-symmetric low-power
core -- that is an AMP / heterogeneous shape, NOT SMP, and does not belong in this
ranking.

## The staged model

SMP is introduced in stages, each of which is independently correct and shippable,
and none of which regresses the single-core builds.

1. **Big-kernel-lock SMP first.** One lock around the whole kernel: cores run user
   code in parallel and serialize whenever they are in-kernel. This is correct on
   EVERY dual-core part regardless of its atomic primitive -- SIO spinlock on
   RP2040, exclusives on RP2350, S32C1I CAS on Xtensa -- because it needs only ONE
   working cross-core lock. Bring this up on RP2040 or RP2350 -- whichever is on the
   bench -- since the BKL runs on both.

   The critical invariant: a SINGLE-CORE build stays BYTE-IDENTICAL. It does NOT
   get there by the lock being uncontended, which was this document's earlier
   reasoning and is wrong: an uncontended lock still costs its acquire, its fences
   and its storage. It gets there by the lock COMPILING TO NOTHING. Both reference
   kernels on this box do it that way and neither relies on the optimiser -- seL4
   expands `NODE_LOCK` to `do {} while (0)` and folds `CURRENT_CPU_INDEX()` to a
   literal 0, declaring its per-core struct only under `ENABLE_SMP_SUPPORT`;
   FreeRTOS gives `portGET_TASK_LOCK` an empty macro body at one core and carries
   the bifurcation up into function signatures. Copy the mechanism, not the
   uncontendedness argument. This matters here specifically because
   `system/include/kickos/sys/atomic.h` already records GCC out-lining an
   `always_inline` candidate at -Os.

   On the lineage: this matches seL4's big-lock SMP lineage in the LOCK and not yet
   in the HOLD TIME, and in seL4 the two are one design. seL4's lock is taken on the
   first line of every trap handler and released only at user return, so what keeps
   it cheap is the FASTPATH -- a wall of bail-out guards that refuses a message over
   four words, any extra capability, an ASID allocation, and a partner on another
   core, leaving a critical section of one capability lookup, a queue pop, four
   REGISTER moves and one address-space register write. There is no memory copy in
   its fastpath at all. KickOS today has one IPC path that copies up to
   `KOS_EP_MSG_MAX` bytes, mints a reply capability into a PEER task's table and
   reinserts into the ready queue -- the shape of seL4's SLOWPATH. Adopting the lock
   without the fastpath/slowpath split adopts the serialisation and none of the
   mitigation. Note also that a CLH queue lock, the algorithm seL4 chose for bounded
   FIFO fairness, needs atomic exchange and so is not reachable on RP2040 at all.

2. **Fewer things inside the lock, before more locks.** Per-core run-queues and a
   shorter critical section, as an OPTIMIZATION layered on the correct BKL, never a
   rewrite of it. Two cautions on scope, both from reading the reference kernels
   rather than from first principles.

   First, more locks is not where the win is. Every SMP port in the local FreeRTOS
   tree stops at TWO locks -- including the armv8-M ones, which have exclusives and
   decline to use them -- and seL4 stops at one. Two looks like the endgame in this
   class of kernel, not a way station, so a plan whose stage 2 is "many fine-grained
   locks on RP2350" is chasing something neither reference kernel attempts.

   Second, the payoff is bounded by hold time, not by lock count. FreeRTOS's split
   is instructive because the two locks have deliberately different hold profiles: a
   long-held task lock covering a scheduler-suspension region, and a short ISR lock
   that is the ONLY lock interrupt context may take, so an ISR on one core makes
   progress while another core blocks inside a queue operation. That asymmetry, not
   the count, is what buys concurrency. Note FreeRTOS holds its locks across an
   unbounded `memcpy` and across the scheduler's CHOICE, but never across the
   register switch or the block -- which is the line to aim at, and a weaker
   constraint than "never hold a lock across a switch".

3. **ESP32 LX6 SMP after.** Once the model is proven on M-profile, port it to the
   Xtensa windowed ABI (CAS-backed lock, windowed-context SMP save/restore), the
   hardest of the three and deliberately last.

## The constraint that shapes everything

SMP is a PER-CHIP CAPABILITY, not a uniform kernel property. The design goal is an
SMP-AWARE kernel whose single-core build is UNCHANGED -- the same discipline the
MPU backend follows (a chip either has the mechanism or it does not; the kernel
above the seam is uniform). A part gets exactly the SMP tier its silicon earns:
none (single-core), or shared-kernel (any dual-core, since one working cross-core
lock is the whole entry requirement). What the silicon does NOT decide is lock
granularity -- see the gate above. This is M5.

## OPEN: AMP or a shared kernel first

This is unresolved, and the three records disagree, so nothing here should be read
as a verdict. `roadmap.md` says "not two AMP instances"; `TODO.md`'s M5 heading says
AMP first on RP2040 and attributes that verdict to this document, which has never
contained it. Resolve it before writing SMP code, because it decides where the
M4.9.x groundwork points.

Evidence that arrived after the original ranking, and it cuts toward AMP being
respectable rather than a retreat: seL4's own `CAVEATS.md` states that its SMP
configuration is unverified, every verified configuration in its tree pins one node,
and its stated route to higher assurance for multicore is a STATIC MULTI-KERNEL --
one kernel instance per core over disjoint memory -- "because they are simpler and
closer to the current sequential seL4 proofs." The project this document cites as
the big-lock precedent has aimed its own assurance roadmap away from the big lock.

The number that should decide it is not in this document and is cheap to get: what
fraction of a call/reply round-trip is spent inside `IrqLock` today, measured
single-core on the bench. A shared kernel's payoff is Amdahl-bounded by exactly that
fraction, and the "about 2x" figure below is unproven.

## M6 lands the MMU, so do not design M5 into a corner

`roadmap.md` makes M6 the MMU / new-platform milestone. That has three consequences
for the choices above, and they cut against treating MPU-only as permanent.

- **Cross-core TLB maintenance is deferred, not inapplicable.** An MPU has no
  translation cache, so today there is nothing to shoot down. Under an MMU there is,
  and both reference kernels answer it the same way: a BLOCKING all-core rendezvous
  (seL4's `doRemoteMaskOp` plus `ipi_wait`, or firmware `sbi_remote_sfence_vma` on
  RISC-V). So an M5 cross-core transport that can carry ONLY asynchronous
  fire-and-forget notification will need extending in M6. Design the doorbell so a
  blocking rendezvous can be layered on it.
- **The primitive gap closes exactly where the MMU appears.** A blocking rendezvous
  needs fetch-add, which armv6m and rxv3 cannot emit -- but no MMU-class target is
  armv6m. The parts that force the no-RMW constraint are not the parts that will
  carry M6.
- **Object reclamation gets harder, so pick an answer that survives translation.**
  Holding one lock across the whole resolve-to-use span (seL4's answer) works with
  or without an MMU. A timestamp-quiescence scheme (Composite's) additionally
  becomes the mechanism M6 needs for retyping mapped memory. Either survives; a
  design that leans on "no address translation exists" does not.

`docs/design-mmu-era-exploration.md` enumerates the five places the single-physical-
address-space assumption is baked in. It is the companion to this document, and its
title still says post-M6 while `roadmap.md` says M6.

## The hazard catalogue this document does not carry

`docs/design-capability-table.md` section 8 holds the real M5 work: a catalogue of
uniprocessor assumptions in the capability path, longer and sharper than anything
here, ending on the blocker underneath -- that a pointer `cap_resolve` has just
resolved can be freed by another core. Read the two together; neither is complete
alone. Note that a third answer to that blocker exists beside RCU and hazard
pointers: seL4 carries no reclamation machinery at all, because a lock spanning the
whole resolve-to-use window makes a concurrently-resolved pointer impossible. Its
residue is the handful of places a lock cannot reach -- another core's current
thread, its current scheduling context, its FPU owner -- and it clears those with a
blocking stall IPI.

## If an atomic RMW is ever added, it is a per-arch seam, not one mechanism

`system/include/kickos/sys/atomic.h` exposes no read-modify-write surface today, and
`docs/reference/style.md` carries the rule. What that rule should NOT be read as is
"an RMW is impossible here": a lock-bracketed RMW is implementable on every backend,
and the libatomic argument only rules out getting one from the toolchain. If one is
ever wanted, the shape is the same shape the MPU backends already use -- one seam,
one contract, a per-arch implementation -- because the cheapest correct mechanism
genuinely differs per part.

Measured cost of taking a lock, which is what a uniform-lock design would pay
everywhere:

| Backend | Lock mechanism | Acquire cost |
|---|---|---|
| single-core: armv7m, rxv3, xtensa LX6, armv6m microbit | `IrqLock` | one BASEPRI / PSW / `mstatus` write, no retry loop |
| RP2040, dual, no exclusives | SIO HARDWARE spinlock | one peripheral read to claim, plus a fence |
| RP2350, dual, SIO aliased by E2 | `ldaexb`/`strexb` SOFTWARE spinlock | a 6-instruction retry loop plus two fences |

Two consequences, and the second is a correctness rule rather than a cost.

- **A uniform lock would tax the parts that do not need one.** On RP2350 a direct
  exclusive RMW is about half the instructions of a lock-bracketed one and needs no
  fence at relaxed ordering, and E2 does not touch the exclusives -- only the SIO
  spinlock registers. So the RP2350 backend should use the exclusives directly and
  the spinlock not at all. Note the inversion this produces: a spinlock is CHEAPER
  on RP2040, which has the working peripheral, than on RP2350, which emulates it.
- **On a dual-core armv6m build, `IrqLock`-bracketing an RMW is WRONG, not merely
  slow.** Masking local interrupts does nothing to the other core. That backend must
  be the SIO spinlock specifically. A build that can select the cheap masking form on
  a dual-core part is a silent correctness bug, so the seam should refuse it at
  compile time rather than trust the selection.

A single-writer counter needs none of this and keeps a plain load, add and store on
every backend. The seam question only arises for a field with two real writers, and
`docs/reference/invariants.md` records the fields whose serialisation is a critical
section rather than an atomic.

## Hardware mechanics that decide the design

RP2040 (RP-008371-DS). SIO is core-local hardware on each M0+ IOPORT (DS 2.3.1):
CPUID reads 0 or 1 (DS 2.3.1.1); 32 hardware spinlocks where a read is an
attempt-claim, a write is a release and core 0 wins a tie (DS 2.3.1.3); two 32-bit
x 8-deep inter-processor FIFOs with a per-core IRQ, 15 on core 0 and 16 on core 1
(DS 2.3.1.4). ARMv6-M has no LDREX/STREX and the SIO/IOPORT itself "does not support
atomic accesses at the bus level" (DS 2.1.2), so the SIO spinlock is the ONLY
cross-core mutex on this part and nothing lock-free is reachable. DMB/DSB/ISB do
exist. Core-1 launch (DS 2.8.2) is a restartable six-word FIFO handshake
`{0, 0, 1, VTOR, SP, PC}`, each word echoed back by core 1 before core 0 advances, a
`0` preceded by draining core 1's replies and a `__sev()`.

RP2350 (RP-008373-DS). Each of two processor sockets is filled at reset by EITHER a
Cortex-M33 or a Hazard3 per the OTP ARCHSEL register, the unused one held in reset
with clocks gated (DS 3.9). ARCHSEL is sampled only at reset, so a watchdog reset can
flip architecture in software. Mixed Arm plus RISC-V is physically possible but out of
scope, needing two separate program images (DS 3.9.2). SIO v2 keeps the 32 spinlocks
(DS 3.1.4) and CPUID (DS 3.1.2), and BOTH processor types have native atomics behind a
global exclusive monitor (DS 2.1.6), so an Arm ldrex/strex and a RISC-V amo.w can
share one variable (DS 3.9.2). Doorbells are new: 8 flags per direction where
DOORBELL_OUT_SET raises the peer's SIO_IRQ_BELL (int 26, DS 3.1.6), accumulative
ring-once/answer-once events. FIFOs shrink to 32-bit x 4 deep behind SIO_IRQ_FIFO
(int 25). Secure and Non-secure SIO banks are SEPARATE, each with its own spinlocks,
FIFOs and doorbells, so NS code cannot starve S code (DS 3.1.1), which makes bank
selection a TrustZone-M33 concern. Core-1 launch (DS 5.3) is the same handshake plus
an arch marker. A RISC-V platform timer is added (DS 3.1.8).

**Erratum RP2350-E2: writes above SIO +0x180 alias the spinlocks.** The SDK uses an
atomic-memory workaround. Prefer native atomics over SIO spinlocks on M33 and Hazard3
regardless of stepping.

**A RUNNING KickOS image detaches SWD, and the CAUSE IS NOT ESTABLISHED.** The symptom is
measured on both RP parts: J-Link finds the SW-DP and then fails to power up the DAP, so a
reflash needs BOOTSEL or a power cycle. The chip-wide-SLEEP-when-both-cores-idle explanation
was a HYPOTHESIS and was never confirmed, and no busy-idle knob has ever existed in this tree,
so do not derive an SMP idle policy from either. Measure it before designing against it.

## Cross-core IPC invariants

The transport itself (per-direction SPSC ring, DMB-ordered publish, FIFO-as-doorbell,
`Channel` = ring plus a receiver-owned Semaphore, the `.shared_ipc` pow2 region) is
recorded in `TODO.md` under M5. These are the invariants it rests on.

IPC-1  No shared mutable state crosses cores WITHOUT explicit ordering. A
  naturally-aligned 32-bit access is single-copy atomic on the AMBA fabric, but
  nothing orders two of them across cores: a producer separates payload store from
  index publish with a DMB, a consumer separates index load from payload load.
IPC-2  Exactly one writer per index, so no read-modify-write ever lands on a word the
  peer writes. That is the sole reason a ring would otherwise need CAS.
IPC-3  A blocked `recv` parks on ITS OWN core's run queue and is woken by ITS OWN
  core's ISR, so the wakeup crosses cores as an interrupt and never as a cross-core
  scheduler poke. A spurious wake is harmless: the receiver re-checks and parks again.
IPC-4  The shared window is the ONLY memory writable by both cores, and it holds
  indices and slot bytes and no pointer into either private space.
IPC-5  Per-direction SPSC needs NO lock, so no structure written by BOTH cores may sit
  on the fast path. The console obeys the rule: core 0 owns the UART, core 1 posts
  records into its own SPSC direction and rings the doorbell, and core 0's ISR feeds
  its existing TX backend. ONE ring written by both cores is the MPMC case, which on
  M0+ needs the SIO spinlock; a zero-copy pool allocator is the same case.

A doorbell dropped on a full FIFO loses no message, because the peer's ISR drains
every non-empty ring regardless. Level-of-work semantics, not edge.

**The FIFO reclaim hazard.** The bootrom used the FIFO for `wait_for_vector` and
leaves stale handshake and echo words behind. BOTH cores must drain their own end and
clear FIFO_ST.ROE and FIFO_ST.WOF BEFORE enabling `SIO_IRQ_PROCn`, or a spurious
doorbell fires on first use. Ring init has its own order: core 0 zeroes both rings and
DMBs BEFORE starting the launch handshake, and the handshake is itself the
happens-before edge, so the secondary must NOT re-init them.

## Prerequisites forwarded here from landed work

  - **Clock retune, `docs/design-m3-clock-select.md` ruling 5.** The P-state
    transition is SINGLE-CORE ONLY, because the safe sequence relies on `IrqLock`
    quiescing the one and only timer. An SMP port needs a cross-core quiesce: a
    per-core SysTick re-arm plus a barrier so no other core reads a half-updated
    anchor. Flagged there, not solved there.
  - **The `volatile`-to-`std::atomic` conversion is DONE, in M4.9.2, and does not wait for
    this design.** Every cross-thread field is now a relaxed `std::atomic` with the order
    spelled at each access; `volatile` is left only for MMIO, for an object the compiler
    must not elide, and for a 64-bit cross-thread field, a relaxed 64-bit atomic load being
    a `__atomic_load_8` libcall on every backend and a freestanding link carrying no
    libatomic. `../reference/style.md` states the rule. Measured on all five backends: a
    relaxed 32-bit load and store are the same single instruction as `volatile`, so the
    conversion was free where it applied.
    **What this bought is a type, not ordering.** Relaxed says nothing a second core will
    honour, so every ordering question this design raises is still open; what changed is
    that the accesses are now defined rather than UB, and each one is a named place for the
    acquire or release to go. Three residues land here:
    - the 64-bit fields that stayed `volatile`. None remain: the two that mattered were
      each narrower than their width said, so both were narrowed to a 32-bit atomic and
      given the ordering the width had been denying them;
    - the seven per-chip clock extenders, a `_high`/`_last` word pair made coherent by
      `arch_irq_save` alone: `stm32f103`, `stm32f302`, `stm32f411`, `sam3x8e`, `imxrt1062`,
      `rx72m` and the lx6 CCOUNT fallback. On SMP an interrupt mask on one core excludes
      nothing on another, so the pair needs a seqlock or a per-core anchor whatever type the
      words have. Note what a seqlock costs here first: the fold is done BY THE READER, so
      every reader is a writer and there is no single writer to seqlock behind. The four
      parts with a wrap ISR can move the fold into it and gain one; `imxrt1062`, `rx72m` and
      the lx6 fallback poll only and would have to grow one. The wrapper carries no fence
      surface either, and a seqlock reader needs an acquire FENCE that an acquire load does
      not supply;
    - the publication barrier in `user/include/kickos/sys/byte_ring.h`, which was a
      consumer `-D` escape hatch rather than a release store on the now-atomic index.
  - **Console reclaim, `docs/design-m3-console-handover-stageii.md` ruling 7.** No
    hook is reserved for it. Under AMP or SMP the other core's console driver must be
    stopped or fenced before reclaim, or it races the polled panic writer. Reclaim is
    reshaped by this design rather than retrofitted.

## Needs bench silicon to confirm

  - DMB sufficiency for cross-core ring visibility on the real RP2040 fabric (M0+
    store buffer plus SIO ordering). IPC-1 is the claim that must be PROVEN.
  - Core-1 handshake timing, that no residual echo or `__sev` word survives the
    reclaim, and `SIO_IRQ_PROCn` latency once reclaimed. On RP2350, core-1 launch and
    doorbell IRQ latency, the Secure/Non-secure bank split under a TrustZone M33
    build, and RP2350-E2 aliasing on the actual stepping.
  - Static partition sizes (per-core arena versus shared window, slot count and size)
    against real app and console-ring footprints, and the PMSAv6 grant of the window
    on BOTH cores under an MPU build (natural alignment, no arena overlap).
  - BKL contention and the ~2x claim, measured on a real 2-core workload.
  - Cross-ISA parity: one SMP image as dual-M33 versus dual-Hazard3 producing
    identical scheduler behaviour on one RP2350 board.
