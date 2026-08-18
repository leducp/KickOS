<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# RP2350 (Cortex-M33) MPU: the ARMv8-M PMSAv8 backend

> **Status: LANDED** -- the PMSAv8 backend shipped (`e2179da`) and is silicon-validated on the
> RP2350: the enforcement selftest passes, `mpu_fault` gets a clean cross-domain MemManage
> denial, and bench + soak run without a fault. Kept as the DECISION record. The register-level
> facts it used to carry -- the v7-M/v8-M address-reuse table, the `AP[2:1]` encoding, the MAIR
> slots and the encodability shape -- now live in `reference/porting.md` (*MPU descriptor
> encodings, per backend*), which is code-synced; the runtime contract is
> `reference/architecture.md` (Memory domains) and `reference/invariants.md`
> (`mpu-apply-on-every-switch-in`). The four review advisories A-D remain open in `../TODO.md`.

This scoped the memory-protection backend for the RP2350's Cortex-M33, written after inspecting
the shared ARM PMSA backend to answer one question: does the already-landed v7-M `arch_mpu_apply`
work on the M33, or does the M33 need a distinct ARMv8-M backend?

## Verdict

**The RP2350 M33 needs its own ARMv8-M PMSAv8 backend. The shared v7-M PMSA one does NOT work on
it and must never be pointed at the M33's MPU.**

The M33 implements PMSAv8 only; there is no v7-M-compatible MPU mode on an ARMv8-M core. The v7-M
backend does not merely "under-program" the M33 -- it writes v7-M-shaped values into a register
pair whose meaning changed, at the SAME core addresses, so the result is enabled regions with
privileged-only permissions and nonsense limits pointing into an unprogrammed MAIR. It fails
closed on the first unprivileged access: a thread is denied its own stack. That is a semantically
different unit, not a trivial-tweak case. The encoding evidence is in `reference/porting.md`.

The `KICKOS_CHIP_ENFORCES_MPU` fail-loud floor in the top `CMakeLists.txt` is what protected the
tree in the meantime: with no `mpu.cmake`, `KICKOS_HAVE_MPU=1` on `pizero2350` was REJECTED at
configure time rather than silently linking the wrong backend. The port claimed no enforcement and
was honest about it; this document is what lifted that.

## Decisions

1. **The seam needs NO change.** `struct arch_mpu_region { base; size; attr; }` is provably
   sufficient for PMSAv8 (base -> RBAR, base+size -> RLAR's inclusive limit, `attr` ->
   `AP[2:1]`/`XN`/`AttrIndx`), so no per-arch field leaks into it. That was the question worth
   settling before any code: had it needed a field, every other backend would have paid.

2. **Strong override, REUSE the armv7m arch. No separate `armv8m` arch directory.** ARMv8-M
   Mainline is a superset of v7-M for everything the arch layer touches -- BASEPRI critical
   section, DWT, SysTick, NVIC, PendSV/SVCall -- and the port already ran the console path on the
   reused backend. Only the MPU differs, so a whole arch directory would duplicate a large,
   identical backend to swap one file. Rejected.

3. **A SEPARATE TU selected by presence-in-link, not an `#ifdef` fork inside one function.** The
   PMSAv8 definitions live in `arch/arm/common/arch_arm_pmsav8.cc` (+ `regs_v8m.h`), which is NOT
   in the always-compiled `kickos_arch_armv7m` source list; a chip opts in through its own
   `mpu.cmake` (the `KICKOS_ARM_PMSAV8_SOURCE` seam), which `target_sources` it into the chip
   library so the v7-M fallback TUs are never extracted. A v7-M chip's `mpu.cmake` never adds it
   and its fallback stands. The alternative -- one `arch_mpu_apply` forked by `#if` on a compile
   define -- is smaller-diff and was considered viable, but it couples two register layouts in one
   TU and risks building the wrong branch. This also follows the K64F precedent, where
   `chip_mk64f.cc` defines the SYSMPU backend in its always-anchored member.

4. **Program the MPU generically enough for EVERY ARMv8-M part, not just this chip.** PMSAv8 is
   shared by every ARMv8-M chip, so the backend is `arch/arm/common/`, not
   `arch/arm/chip/rp2350/`. nRF5340, STM32U5 and STM32H5 M33 parts inherit it by adding an
   `mpu.cmake` line and nothing else.

5. **Read `MPU_TYPE.DREGION`; do not hard-code the region count.** The M33 on RP2350 implements 8,
   the same budget as the rest of the fleet (code RX + data RW-NX + per-thread stack + optional
   MMIO + guard fits), but the count is an implementation choice per part.

6. **Do not carve the enforcement `.appdata` window ahead of the backend.** It would be dead
   weight until the backend existed, and the verdict above is that the backend is real work rather
   than a scaffolding pass. When it landed, the layout was SIMPLER than the v6/v7 pow2 machinery:
   an app data/bss range is two 32-byte-aligned symbols fed to RBAR/RLAR, with no `ALIGN(pow2)` on
   the block and no size-class padding.

7. **No fault-path change.** PMSAv8 violations raise MemManage with MMFSR/MMFAR exactly as v7-M,
   and the shared `kickos_armv7m_fault_report` already labels a set MMFSR byte and prints MMFAR.
   The `mpu_fault` selftest (ungranted write -> MemManage) was the silicon acceptance gate, and it
   passed.

### Addendum (as-implemented): the override target is `kickos_arch_mpu_commit`

This document named `arch_mpu_apply` as the strong-override symbol throughout, written before the
deferred-MPU-commit seam landed fleet-wide (`design-mpu-commit-deferred.md`). As implemented:

- `arch_mpu_apply` is STASH-ONLY and shared unchanged from `arch_arm_common.cc`, as a plain
  non-overridable definition. The PMSAv8 backend does NOT redefine it.
- **`kickos_arch_mpu_commit`** is what the backend defines: it reads the shared stash via
  `kickos_arm_mpu_pending()` and programs the running thread's regions in the switch epilogue.
  Its member is anchored by `chip_rp2350.cc`'s `kickos_arm_pmsav8_init` call, so the fallback TU
  (`arch/arm/common/kickos_arch_mpu_commit_default.cc`) is never extracted.
- `arch_mpu_region_encodable` and `arch_mpu_min_region` displace the v7-M fallback TUs as
  described; those names are unchanged.
- The one-time MAIR programming plus enable live in `kickos_arm_pmsav8_init`.

So decision 2 and decision 3 hold verbatim; only the overridden symbol name differs. Read
"strong `arch_mpu_apply`" above as `kickos_arch_mpu_commit`.

### Resolved (M4.5.5): the granular-shaping follow-on

The shared `arch_ram_region_size()` originally pow2-shaped RAM regions whenever
`arch_mpu_min_region() != 0`. That was **correct** on PMSAv8 -- a pow2 range is a valid arbitrary
range -- just not minimal, so the allocator wasted the same padding the v7-M chips do. Relaxing it
was deliberately staged as a follow-on rather than bundled into first enforcement, on the
reasoning that correctness comes first and padding can be reclaimed afterwards.

The staging judgement held. `arch_mpu_region_pow2()` now splits the enforcing case into
pow2-required (PMSAv7 RASR, PMP NAPOT) and granular-at-N (PMSAv8, SYSMPU, RX), and
`arch_ram_region_size`/`_align` round to the granule in the second -- a seam change rather than a
per-chip patch, which is what the staging bought. On RP2350 the boot stacks now align to 32 instead
of to 2048/8192. Note the mode is POSTURE-dependent: `arch_arm_pmsav8.cc` is linked only at
`KICKOS_HAVE_MPU=1`, so a non-enforcement build of the same chip still takes the v7-M pow2 fallback.

## SMP interaction (M6 endgame -- noted, NOT designed here)

The RP2350 is the M6 SMP target: dual M33 with real LDREX/STREX exclusives, and the MPU is
**per-core, banked** -- each M33 has its own register file at the same core-local addresses. Two
guardrails so the single-core backend does not embed an assumption M6 would have to unwind:

- **Keep the commit operating on "the current core's MPU" only.** It writes core-local registers
  with no core index, which is already SMP-correct provided the switch path that calls it runs on
  the core the thread lands on. Do NOT add a global or shared MPU cache: the K64F-style
  "skip if the region set is unchanged" static cache would be per-core state under SMP and must
  become core-local, or be omitted. Flagged for M6, not added now.
- **The one-time MAIR setup must run once PER CORE**, not once globally -- fold it into the
  per-core bring-up, never a boot-once path.

## Confidence to verify against silicon first

Recorded because the ARM PMSA had two K64F silicon surprises, and all three came out right:
`AP[2:1]` "any vs privileged-only" bit sense; RLAR limit inclusivity at the 32-byte granule (an
off-by-one page); and MAIR slot values actually yielding cacheable Normal for XIP flash and SRAM.
