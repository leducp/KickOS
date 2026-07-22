<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->

# SMP candidates and the staged model (M5)

Status: DESIGN SPIKE. Forward-looking. This is M5. No build/runtime code change
here -- it ranks the multi-core parts in hand by the ONE gate that actually decides
SMP feasibility, and fixes the staged model (big-kernel-lock first, fine-grained
only where the hardware earns it). Companion to `docs/design-multicore.md` (the
AMP-vs-SMP feasibility study) and `docs/design-multicore-ipc.md` (the cross-core
IPC ring + doorbell). The SMP-BKL endgame referenced in `roadmap.md` (M5) is
detailed here.

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
