<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->

# Deferred MPU commit -- enforcement-soundness seam

> **Status: LANDED** -- the seam is fleet-wide and every enforcing backend uses it
> (`arch_mpu_apply` stashes at the switch decision, `kickos_arch_mpu_commit` programs from the
> switch epilogue after the physical swap). The contract is `reference/invariants.md`
> (`mpu-apply-on-every-switch-in`, `arch-switch-may-defer`) and the teaching is Book ch.7.5.
> See `design/README.md` for the marker taxonomy.

Decision record for the seam that fixes an enforcement-soundness race between the eager
`arch_mpu_apply()` and the deferred (pended) context switch every KickOS arch uses. Per-target
mechanism: `reference/porting.md`.

## The race

`switch_to()` (`kernel/sched/sched.cc`) used to reprogram the per-task MPU on the way out:

    kernel().current = next;
    ...
    arch_mpu_apply(next->regions, next->region_count);  // eager
    arch_switch(&prev->ctx, &next->ctx);                // PENDS the switch

On every arch the switch is deferred. `arch_switch` pends PendSV (ARM) or a software interrupt (RX,
RISC-V) and returns; the physical register/PSP swap happens later, when the caller drops the
critical section and the pended exception fires. So between the apply and the swap the OUTGOING
thread keeps running under the INCOMING thread's region set, and it faults on its own stack the
instant it touches it if `next`'s regions do not cover `prev`'s stack.

REPRO, on RP2040 silicon (armv6-m) under the mutex-chain churn of selftest test 14: `current` and
the MPU had already moved to chain-head A while chain-tail C was the thread physically running
inside `mtx_spin`, giving a MemManage or HardFault. The region math was correct, and the MPU was
catching a real would-be-silent cross-stack scribble. The defect was the TIMING of the apply.
Green at 42/42 after the fix.

Latent on every deferred-switch enforcing backend (armv7-m PMSAv7, K64F SYSMPU, RX MPU, rv32imac
PMP), unobserved outside armv6-m only because their timing had not yet lined the window up under
test. The fault is timing-dependent, so the evidence a backend owes is an EXTENDED chain-repro
under enforcement. A short green selftest run is not sufficient.

## The seam

Split the one eager call into **stash** (eager, harmless) plus **commit** (deferred, atomic with
the physical swap). The decisions inside that split:

- **The stash is a COPY, not a pointer.** The commit must never chase a TCB whose region set
  changed after the stash.
- **`arch_mpu_apply` has one plain shared definition and is not customisable; only
  `kickos_arch_mpu_commit` is per-target.** A chip whose MPU is not PMSAv7 defines the commit
  itself, in an always-anchored member reading the same stash, which keeps the fallback member
  unextracted. One definition per link is what avoids a duplicate-symbol collision across
  backends.
- **The ARM commit brackets its disable/reprogram/re-enable with `cpsid i` (PRIMASK).** The eager
  apply inherited the caller's BASEPRI/PRIMASK lock; the commit instead runs in the lowest-priority
  switch exception, where a device IRQ could otherwise preempt a half-programmed descriptor set.
  `cpsid` is valid asm on v6-M and v7-M alike, so the PMSAv7 commit body stays shared.
- **Register discipline at the epilogue call.** Callee regs r4-r11 are already restored and survive
  the `bl` (AAPCS); r0-r3/r12 are reloaded from the HW frame on exception return; `lr`/EXC_RETURN
  must be preserved across the `bl` (v7-M pushes it, v6-M rebuilds it from a literal immediately
  after).
- **RX and rv32imac keep a LOCAL stash.** Each is its own arch lib with no ARM-common link, so
  there is no cross-arch symbol sharing to collide and no accessor is needed.
- **The hook goes only on the paths that PHYSICALLY swap.** RX calls it from `kickos_rx_restore`,
  which the syscall and timer-ISR returns never reach (they `rte` on their own). rv32imac calls it
  from `.Lswitch` and `arch_start`, NOT from the shared `.Lrestore`, which timer/ssoft/ext/syscall
  returns reach without swapping.
- **No interrupt-enable toggle is added inside a trap.** The RX SWINT handler already runs with
  `PSW.I=0` and the rv32imac trap with `MIE=0`, which IS the bracket. RX adds an
  `arch_irq_save`/`arch_irq_restore` IPL bracket as nested-safe insurance and to match the seam
  contract. REJECTED on rv32imac: a `mstatus.MIE` toggle, because enabling interrupts mid-trap is
  itself a bug.

Net effect: `current`, the physically-running thread and the MPU all become `next` together at the
switch exception. The outgoing thread runs to the swap under its own regions.

K64F is a Cortex-M4 (deferred PendSV) sharing `armv7m/switch.S`, so the epilogue hook converted it
automatically. It GETS the deferred commit, closing the same race there; it is not merely left
unaffected. Its eager strong `arch_mpu_apply` was dropped in favour of the shared stash plus a
strong SYSMPU `kickos_arch_mpu_commit`.
