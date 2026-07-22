<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->

# Teensy 4.1 (i.MX RT1062, Cortex-M7) MPU-enforce hang -- root-caused + fixed

Terse, invariant-first. Records a first-silicon defect on the first-ever M7
under MPU enforcement, its root cause (Cortex-M7 speculative access to Normal
memory, NXP ERR011573 / Arm 1013783-B), and the shipped fix (a chip fixed-MPU
region seam that wraps the unbacked external-memory apertures as Device, plus
the L1 I-cache, ordered cache-after-MPU). Result: selftest under enforcement
went from a deterministic hang to 43/43 + soak.

## Symptom (confirmed on silicon)

Build `teensy41-st -DKICKOS_HAVE_MPU=1` (banner `mpu enforce`, plan `1..43`)
hangs **deterministically** at test 6 `rr_interleave`. A diagnostic build with
per-marker `kos_kconsole_write` prints:

    ok 5 - cpu_clock_set
    # t6 A enter
    # t6 B clk-measured
    # t6 C spawned, join
    <hang>

i.e. `t_rr` reaches `wait_n(2)` and the first round-robin worker's first marker
`# rrw enter` never prints. The freshly-switched worker's **first instruction
never retires**: a frozen PC with NO fault reported, until an unrelated IRQ
preempts it. The switch physically completed (the PendSV epilogue ran); the
worker's own first thread-mode fetch is what stalled.

## Investigation arc (what was ruled out)

The failure intersects exactly **{ MPU enforce } x { KOS_POLICY_RR } x { M7 }**
(no-MPU RR passes; MPU priority-preempt, test 3, passes; MPU + RR hangs), which
initially pointed at the switch/commit path. Ruled out, with silicon evidence:

- **The clock / idle / watchdog / slice-timer.** No-MPU `teensy41-st` passes
  and runs test 6; `cpu_clock_*` (tests 4-5) pass under MPU. `t_rr`'s quantum is
  `>= 1 ms`, orders of magnitude over a full MPU commit, so no slice storm.
- **The MPU-commit seam.** Silicon-proven + soaked on all four other enforcing
  backends (K64F SYSMPU, XMC/RX/C6); a single PendSV epilogue
  (`arch/arm/armv7m/switch.S`) commits identically for RR and non-RR switches.
- **The granted region set.** A privileged RR worker's whole-arena grant is
  non-pow2, so `kickos_arm_mpu_program` fail-closes and drops it
  (`arch/arm/common/arch_arm_common.cc:228`) -- the worker runs on the
  PRIVDEFENA background exactly as root/idle do. No region-descriptor fault.
- **A clean, reported fault.** `kickos_armv7m_fault_report`
  (`arch/arm/armv7m/arch_armv7m.cc`) forces the sync console and prints an
  `=== MPU FAULT === CFSR=...` banner. The console shows nothing after `# t6 C`,
  so no fault was being taken at all -- not a fault escalating to a silent
  double-fault, and not a clean MemManage.

The tell that redirected the investigation: the PC was **frozen on the worker's
first fetch with no fault**, and an injected IRQ un-stuck it. A stall with no
fault is not a protection violation -- it is an access that never completes.

## Root cause -- Cortex-M7 speculative access to unbacked Normal memory

The Cortex-M7 is an in-order core with an L1 cache and a prefetch unit. By
design it issues **speculative** accesses (instruction prefetch, and data
prefetch) to any address the memory map types as **Normal** -- speculation into
Normal memory is architecturally permitted and expected, because Normal memory
has no side effects on read. It must **not** speculate into **Device** or
**Strongly-ordered** memory, where a read can have side effects. This is the
documented M7 behavior: **NXP ERR011573** and **Arm erratum 1013783-B**.

The map on this port made an unbacked window Normal:

- Code is XIP from FlexSPI at `0x6000_0000`, but the Teensy's flash populates
  only **8 MiB** (`sflashA1Size`, `chip_imxrt1062.cc:166`; `LENGTH(FLASH)`).
- The dropped whole-arena grant leaves a privileged thread on the **PRIVDEFENA
  background**, i.e. the ARMv7-M default system map, which types the entire
  `0x6000_0000-0x9FFF_FFFF` (1 GiB FlexSPI + SEMC) region as **Normal**.

So when the worker began executing near the top of the populated image, the M7
speculatively prefetched **past the real 8 MiB** into unbacked FlexSPI address
space. That AHB slave has nothing mapped there and never asserts a response; the
M7 retires in order, so the *current* (already-fetched, valid) instruction could
not retire behind the outstanding speculative access, and the core stalled
forever -- **no fault, because no access completed to fault on**. An IRQ, taken
on a different (backed) path, broke the stall. This is exactly the failure mode
ERR011573 warns about.

Cross-checked against three independent references that all wrap these apertures
before enabling caches on the RT106x:

- NuttX `arch/arm/src/imxrt/imxrt_mpuinit.c` (FlexSPI region typed for the
  populated size, remainder wrapped).
- NXP MCUXpresso SDK `boards/evkmimxrt1060/board.c`, `BOARD_ConfigMPU` (the
  reference MPU table: bounded XIP region, Device/no-access wrap).
- The i.MX RT1060 errata sheet, ERR011573.

## The fix (Option A -- shipped)

Bound the real memory as Normal and wrap everything else in that external window
as Device + execute-never + no-access, so the M7 cannot speculate into an
unbacked AHB slave. Done through a new shared seam, not a chip one-off.

**The fixed-region seam** (`arch/arm/common/mpu.h`,
`arch/arm/common/arch_arm_common.cc`). A chip may declare **thread-invariant**
MPU regions via the weak hook `kickos_arm_mpu_fixed()`; `kickos_arm_mpu_fixed_init()`
programs them **once** into the **LOW** descriptor slots `[0, k)` and caches
`k`. Per-thread grants then program into `[k, hw)`
(`arch_arm_common.cc:217`), so a grant sits **above** the fixed background and
correctly overrides it (PMSAv7: highest-numbered region wins). The rows carry a
raw PMSAv7 `base + RASR` pair, so a chip can encode AP/type values (no-access,
priv-RO, Device) that the portable R/W/X/DEV attr vocabulary cannot express.
`k == 0` for every other chip, so their emitted sequence is byte-identical to
the pre-seam behavior.

**The imxrt fixed table** (`chip_imxrt1062.cc:519-537`), three rows:

| Base | Size | Type / AP |
|---|---|---|
| `0x6000_0000` | 512 MiB | Device + XN + no-access -- FlexSPI aperture wrap |
| `0x6000_0000` | 8 MiB | Normal WB cacheable, priv-RO + X -- populated-flash overlay |
| `0x8000_0000` | 512 MiB | Device + XN + no-access -- SEMC aperture wrap |

The 8 MiB Normal overlay sits in a higher slot than the 512 MiB wrap, so the
real image is executable/readable while the unbacked remainder is Device (never
speculated) and no-access (never even architecturally reachable).

**The L1 I-cache** (`arch/arm/armv7m/cache.cc`, `kickos_armv7m_icache_enable`).
Enabled as part of the fix; this is the configuration the fix was proven with.

**Ordering is load-bearing** (`chip_imxrt1062.cc:483-487`): the fixed regions
must be **live before the cache is enabled**, because the cache is what arms the
prefetch/speculation the wrap defends against. `arch_init` calls
`kickos_arm_mpu_fixed_init()` then `kickos_armv7m_icache_enable()`, in that
order, before the scheduler starts.

**Corrections applied during the fable review of the initial POC:**

- **F1 -- the wrap is AP=no-access, not RW.** The POC left the wrap readable,
  which turns it into an unprivileged-triggerable DoS (any thread could aim a
  speculative-stall at it). `AP_NONE` (`chip_imxrt1062.cc:524`).
- **F2 -- fixed regions in the LOW slots, not high.** So per-thread grants
  (high slots) override the fixed background, not the reverse. Had it been
  inverted, the wrap would have shadowed a legitimate grant.
- **F3 -- no fleet-wide per-thread code region for privileged threads.** The
  populated-flash overlay is a *fixed* region, programmed once; privileged
  threads keep running on the background map for RAM/peripherals.

## Silicon result

`teensy41-st -DKICKOS_HAVE_MPU=1` (banner `mpu enforce`): **43/43**, was a hang
at test 6. Soak clean. No-MPU Teensy (unchanged) and the four other enforcing
backends stand.

## Residuals & follow-ups

- **D-cache (pre-M4).** `kickos_armv7m_dcache_enable` exists but is opt-in via
  `-DKICKOS_IMXRT_DCACHE=1` (`chip_imxrt1062.cc:488`). Safe today (single-core,
  no DMA); the only coherency obligation arrives with M4-era DMA. Default it on
  after silicon validation. TODO.md.
- **Option B (post-M6 fleet-wide hardening).** The shipped fix keeps
  `PRIVDEFENA` (privileged code still runs on a permissive background). The
  stronger posture -- drop `PRIVDEFENA`, program an explicit whole-map, and
  confine the kernel to its own regions -- is recorded post-M6. TODO.md.
- **The MPU-disabled per-switch window (accepted).** `kickos_arm_mpu_program`
  writes `MPU_CTRL = 0` to reprogram (`arch_arm_common.cc:207`), which reverts
  to the Normal-typed default map for those instructions. It is bracketed by
  `cpsid i` in the commit (`arch_arm_common.cc:335`) and runs from already-cached
  fetches, so no new speculation into the unbacked window is armed within it.
  Accepted residual.
- **HFNMIENA bypass (accepted).** `MPU_CTRL` is enabled without `HFNMIENA`
  (`arch_arm_common.cc:240,282`), so the MPU -- and thus the anti-speculation
  wrap -- is bypassed inside HardFault/NMI handlers. The fault path is short and
  runs cached; accepted residual.
