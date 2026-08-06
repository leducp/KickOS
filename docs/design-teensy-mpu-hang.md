<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->

# Teensy 4.1 (i.MX RT1062, Cortex-M7) MPU-enforce hang -- root-caused + fixed

> **Status: LANDED** -- the fix shipped (`c072712`) and the enforcement selftest passes with a
> clean soak. The durable teaching is Book ch.7.6 (memory types and speculative access); the
> shared seam it added is `kickos_arm_mpu_fixed` in `arch/arm/common/`.
> See `design/README.md` for the marker taxonomy.

Decision record for a first-silicon defect on the fleet's first M7 under MPU enforcement: its root
cause (Cortex-M7 speculative access to Normal memory, NXP ERR011573 / Arm 1013783-B) and the
shipped fix. The board-level account is `reference/boards.md`, the seam's contract is
`reference/porting.md`, and the teaching is Book ch.7.6.

## Symptom (confirmed on silicon)

REPRO: `cmake --preset teensy41-st` (its base variant enforces by default; banner `mpu enforce`, plan `1..43`) hangs
DETERMINISTICALLY at test 6 `rr_interleave`. The failure intersects exactly
{ MPU enforce } x { KOS_POLICY_RR } x { M7 }: no-MPU RR passes, and MPU priority-preempt (test 3)
passes.

The freshly-switched worker's FIRST instruction never retires. Its first marker never prints, the PC
is frozen, and NO fault is reported until an unrelated IRQ preempts it and unsticks the core. The
switch itself physically completed (the PendSV epilogue ran). A stall with no fault is not a
protection violation, it is an access that never completes.

## Root cause -- Cortex-M7 speculative access to unbacked Normal memory

The M7 issues speculative instruction and data prefetches to any address the memory map types as
**Normal**, which is architecturally permitted because a Normal read has no side effects, and must
not speculate into Device or Strongly-ordered memory. Documented as **NXP ERR011573** and **Arm
erratum 1013783-B**.

This port made an unbacked window Normal. Code is XIP from FlexSPI at `0x6000_0000` but the
Teensy's flash populates only **8 MiB**, and a privileged thread left on the ARMv7-M PRIVDEFENA
background map has the entire `0x6000_0000-0x9FFF_FFFF` (1 GiB FlexSPI + SEMC) typed Normal. So a
worker executing near the top of the populated image prefetched PAST the real 8 MiB into unbacked
FlexSPI space, whose AHB slave never asserts a response. The M7 retires in order, so the current
already-fetched valid instruction could not retire behind the outstanding speculative access and
the core stalled forever, with no fault because no access completed to fault on.

Cross-checked against three independent references that all wrap these apertures before enabling
caches on the RT106x:

- NuttX `arch/arm/src/imxrt/imxrt_mpuinit.c` (FlexSPI region typed for the populated size,
  remainder wrapped).
- NXP MCUXpresso SDK `boards/evkmimxrt1060/board.c`, `BOARD_ConfigMPU` (the reference MPU table:
  bounded XIP region, Device/no-access wrap).
- The i.MX RT1060 errata sheet, ERR011573.

## The fix (Option A -- shipped)

DECIDED: bound the real memory as Normal and wrap everything else in that external window as
Device + execute-never + no-access, so the M7 cannot speculate into an unbacked AHB slave.

DECIDED: do it through a new SHARED seam, not a chip one-off. A chip may declare
thread-INVARIANT MPU regions via `kickos_arm_mpu_fixed()`, programmed once into the LOW descriptor
slots `[0, k)`; per-thread grants then program into `[k, hw)`, so a grant sits ABOVE the fixed
background and correctly overrides it (PMSAv7: highest-numbered region wins). `k == 0` on every
other chip, so their emitted sequence is byte-identical to the pre-seam behavior.

DECIDED: the rows carry a raw PMSAv7 `base + RASR` pair. The portable R/W/X/DEV attr vocabulary
cannot express the AP and type values this needs (no-access, priv-RO, Device).

The imxrt fixed table, three rows:

| Base | Size | Type / AP |
|---|---|---|
| `0x6000_0000` | 512 MiB | Device + XN + no-access -- FlexSPI aperture wrap |
| `0x6000_0000` | 8 MiB | Normal WB cacheable, priv-RO + X -- populated-flash overlay |
| `0x8000_0000` | 512 MiB | Device + XN + no-access -- SEMC aperture wrap |

The 8 MiB Normal overlay sits in a higher slot than the 512 MiB wrap, so the real image is
executable and readable while the unbacked remainder is Device (never speculated) and no-access
(never even architecturally reachable).

ORDERING IS LOAD-BEARING: the fixed regions must be live BEFORE the cache is enabled, because the
cache is what arms the prefetch the wrap defends against. `arch_init` calls
`kickos_arm_mpu_fixed_init()` then `kickos_armv7m_icache_enable()`, in that order, before the
scheduler starts. The L1 I-cache being enabled is the configuration the fix was proven with.

**Corrections applied during the fable review of the initial POC:**

- **F1 -- the wrap is AP=no-access, not RW.** REJECTED: leaving the wrap readable, as the POC did.
  A readable unbacked aperture is an unprivileged-triggerable denial-of-service vector, since any
  thread could aim a speculative stall at it.
- **F2 -- fixed regions in the LOW slots, not high.** REJECTED: the inverted order, in which the
  wrap would have shadowed a legitimate per-thread grant.
- **F3 -- no fleet-wide per-thread code region for privileged threads.** The populated-flash
  overlay is a FIXED region programmed once; privileged threads keep running on the background map
  for RAM and peripherals.

## Silicon result

`teensy41-st -DKICKOS_HAVE_MPU=1` (banner `mpu enforce`): **43/43**, was a hang at test 6. Soak
clean. No-MPU Teensy (unchanged) and the four other enforcing backends stand.

## Residuals & follow-ups

- **D-cache: RESOLVED.** `kickos_armv7m_dcache_enable` is silicon-validated and now the imxrt
  default (`KICKOS_IMXRT_DCACHE ON`). Safe on this single-core, DMA-less port; the coherency
  obligation arrives with DMA (a non-cacheable pool or per-buffer clean/invalidate) and is carried
  in TODO.md.
- **Option B (post-M6 fleet-wide hardening).** The shipped fix keeps `PRIVDEFENA`, so privileged
  code still runs on a permissive background. The stronger posture (drop `PRIVDEFENA`, program an
  explicit whole-map, confine the kernel to its own regions) is recorded post-M6. TODO.md.
- **The MPU-disabled per-switch window (ACCEPTED).** `kickos_arm_mpu_program` writes
  `MPU_CTRL = 0` to reprogram, reverting to the Normal-typed default map for those instructions. It
  is bracketed by `cpsid i` in the commit and runs from already-cached fetches, so no new
  speculation into the unbacked window is armed within it. Accepted residual.
- **HFNMIENA bypass (ACCEPTED).** `MPU_CTRL` is enabled without `HFNMIENA`, so the MPU, and with it
  the anti-speculation wrap, is bypassed inside HardFault and NMI handlers. The fault path is short
  and runs cached. Accepted residual.
