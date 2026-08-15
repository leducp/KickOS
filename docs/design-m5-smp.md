<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->

# SMP candidates and the staged model (M5)

> **Status: EXPLORATORY** -- a spike, not a contract. Nothing here is implemented. M5 is the
> milestone after the M4 driver era. See `design/README.md` for the marker taxonomy.

Status: DESIGN SPIKE. Forward-looking. This is M5. No build/runtime code change
here -- it ranks the multi-core parts in hand by the ONE gate that actually decides
SMP feasibility, and fixes the staged model (big-kernel-lock first, fine-grained
only where the hardware earns it). It also carries the AMP-vs-SMP feasibility
verdict and the cross-core IPC ring plus doorbell design, both folded in here. The
SMP-BKL endgame referenced in `roadmap.md` (M5) is detailed here.

## The real gate

SMP feasibility is not decided by "does it have two cores." It is decided by two
coupled facts:

1. **The inter-core atomic primitive.** A shared kernel needs mutual exclusion
   that holds ACROSS cores -- masking local interrupts does nothing to another
   core. That requires either an architectural atomic (LDREX/STREX exclusives, a
   compare-and-swap) or a hardware spinlock block. Which one the silicon has caps
   how fine the locking can ever get.
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

### RP2040 (2x Cortex-M0+) -- BIG-LOCK ONLY
armv6-M has NO LDREX/STREX and the SIO bus is non-atomic, so there is no path to
fine-grained locking here -- ever. The only cross-core primitive is the SIO
hardware spinlock block (32 spinlocks). That is enough for a single big kernel
lock and nothing finer. Value: cheap bring-up of the SMP MODEL (core-1 launch,
per-core run-queue, per-core tickless state, the BKL discipline) on the simplest
possible dual-core part, capped structurally at one lock.

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
   working cross-core lock. The critical invariant: a SINGLE-CORE build stays
   BYTE-IDENTICAL, because the big lock is always uncontended there (redefine
   `IrqLock` as local-IRQ-off + one global lock; on a one-core build the lock is
   never contended and folds away). Bring this up on RP2040 or RP2350 -- whichever
   is on the bench -- since the BKL runs on both. This matches the seL4 big-lock
   SMP lineage.

2. **Fine-grained SMP only where exclusives exist.** Per-core run-queues,
   lock-free fast paths, and finer locks -- introduced ONLY on parts with real
   atomics, i.e. RP2350 (M33 exclusives, or Hazard3 A-extension). This is an
   OPTIMIZATION layered on top of the correct BKL, never a rewrite of it. RP2040
   never advances past stage 1 (no atomics), and that is fine -- the BKL is its
   permanent ceiling.

3. **ESP32 LX6 SMP after.** Once the model is proven on M-profile, port it to the
   Xtensa windowed ABI (CAS-backed lock, windowed-context SMP save/restore), the
   hardest of the three and deliberately last.

## The constraint that shapes everything

SMP is a PER-CHIP CAPABILITY, not a uniform kernel property. The design goal is an
SMP-AWARE kernel whose single-core build is UNCHANGED -- the same discipline the
MPU backend follows (a chip either has the mechanism or it does not; the kernel
above the seam is uniform). A part gets exactly the SMP tier its silicon earns:
none (single-core), big-lock (any dual-core), or fine-grained (atomics present).
This is M5.

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
