<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->

# Deferred MPU commit -- enforcement-soundness seam

Terse, invariant-first. This records the fleet-wide seam that fixes an
enforcement-soundness race between the eager `arch_mpu_apply()` and the
deferred (pended) context switch every KickOS arch uses.

## The race

`switch_to()` (`kernel/sched/sched.cc`) reprograms the per-task MPU in one
place, on the way out:

    kernel().current = next;
    ...
    arch_mpu_apply(next->regions, next->region_count);  // eager
    arch_switch(&prev->ctx, &next->ctx);                // PENDS the switch

On every arch the switch is **deferred**: `arch_switch` pends PendSV (ARM) or a
software interrupt (RX / RISC-V) and *returns*. The physical register/PSP swap
happens later, when the caller drops the critical section and the pended
exception fires. So between `arch_mpu_apply` and the physical swap the
**outgoing** thread keeps running -- now under the **incoming** thread's region
set. If `next`'s regions do not cover `prev`'s stack, `prev` faults on its own
stack the instant it touches it.

Proven on RP2040 silicon (armv6-m) under mutex-chain churn (selftest test 14):
`current`/MPU had already been moved to chain-head A while chain-tail C was the
thread physically running inside `mtx_spin` -> MemManage/HardFault. The MPU was
correct; it caught a would-be-silent cross-stack scribble. The bug was the
*timing* of the apply, not the region math.

Latent on every deferred-switch enforcing backend (armv7-m PMSAv7, K64F SYSMPU,
RX MPU, rv32imac PMP); unobserved outside armv6-m only because their timing had
not yet lined the window up under test.

## The seam

Split the one eager call into **stash** (eager, harmless) + **commit**
(deferred, atomic with the physical swap):

1. **Shared stash.** `arch_mpu_apply(regions, n)` now only *records* the
   incoming set into a private, fixed-size copy (`g_pend_regions` / `g_pend_count`
   in `arch/arm/common/arch_arm_common.cc`). A copy, not a pointer -- the commit
   must never chase a TCB whose region set changed after the stash. `switch_to()`
   is unchanged; it still calls `arch_mpu_apply` under the kernel critical section.

2. **Per-backend commit.** `kickos_arch_mpu_commit(void)` programs the hardware
   from the stash. It is the single symbol each arch's switch epilogue calls by a
   fixed name. It resolves per link with weak/strong linkage -- exactly one
   definition wins, so there is no duplicate-symbol collision across backends:
   - `arch_arm_common.cc` provides the **weak** default: for `KICKOS_HAVE_MPU`
     it is the **PMSAv7** commit (`kickos_arm_mpu_program` from the stash), and
     for a no-MPU board it is an **empty** commit.
   - A chip whose MPU is not PMSAv7 provides a **strong** override that reads the
     *same* stash (`kickos_arm_mpu_pending`) and programs its own hardware. K64F
     (SYSMPU) does this; it therefore does **not** override `arch_mpu_apply`, so
     `arch_mpu_apply` has exactly one (weak, shared) definition per link.
   - The commit brackets its disable/reprogram/re-enable with `cpsid i`
     (PRIMASK). Eager apply ran under the caller's BASEPRI/PRIMASK lock; the
     commit runs in the lowest-priority switch exception, where a device IRQ
     could otherwise preempt a half-programmed descriptor set. `cpsid` is valid
     asm on both v6-M and v7-M, so the PMSAv7 commit body is shared.

3. **Per-arch epilogue hook.** Each deferred arch's switch assembly calls
   `bl kickos_arch_mpu_commit` **after** the physical register/PSP swap, before
   the exception return. Register discipline: callee regs (r4-r11) are already
   restored and survive the call (AAPCS); r0-r3/r12 are reloaded from the HW
   frame on exception return; `lr`/EXC_RETURN must be preserved across the `bl`
   (v7-M pushes it; v6-M rebuilds it from a literal immediately after).

Net effect: `current`, the physically-running thread, and the MPU all become
`next` together at the switch exception. The outgoing thread runs to the swap
under its own regions.

## Rollout

| Arch / backend | Boards | apply (stash) | commit | epilogue hook | Status |
|---|---|---|---|---|---|
| armv6-m PMSAv7 | RP2040 (picopi), microbit | shared weak | shared weak PMSAv7 | `armv6m/switch.S` | DONE -- silicon-validated (RP2040 test 14, 42/42) |
| armv7-m PMSAv7 | XMC4800, F411 | shared weak | shared weak PMSAv7 | `armv7m/switch.S` | REFERENCE here -- build-only, silicon-pending |
| armv7-m SYSMPU | FRDM-K64F | shared weak | strong (chip) | `armv7m/switch.S` | build-only, silicon-pending |
| armv7-m no-MPU | qemu (mps2) | shared weak no-op | shared weak empty | `armv7m/switch.S` | build-only |
| armv8-m PMSAv8 | RP2350 (pizero2350) | shared weak | strong (RLAR/MAIR, `arch_arm_pmsav8.cc`) | `armv7m/switch.S` reuse | DONE -- silicon-validated (RP2350: selftest 43/43 enforce, clean cross-domain `mpu_fault`, bench + soak; authority: `docs/design-rp2350-mpu-armv8m.md`) |
| RX MPU | RX72M | local stash | `kickos_arch_mpu_commit` (RSPAGEn/REPAGEn) | `rxv3/switch.S` (`kickos_rx_restore`) | DONE (build-only) -- silicon-pending |
| rv32imac PMP | qemu-virt, ESP32-C6 | local stash | `kickos_arch_mpu_commit` (pmpaddr/pmpcfg CSRs) | `rv32imac/switch.S` (`.Lswitch` + `arch_start`) | DONE (build-only) -- silicon-pending |

K64F note: it is a Cortex-M4 (deferred PendSV) and shares `armv7m/switch.S`, so
adding the epilogue hook there converts K64F automatically -- it **gets** the
deferred commit (closing the same race on K64F), it is not merely left
unaffected. Its eager strong `arch_mpu_apply` was dropped in favour of the
shared stash + a strong SYSMPU `kickos_arch_mpu_commit`.

RX and rv32imac now follow the identical seam. Each is its own arch lib (no
ARM-common link, so no cross-arch symbol sharing and no collision), so the stash
is a **local** `g_pend_regions` in the arch `.cc` and the commit reads it
directly -- no accessor is needed. Per-arch specifics:
- **RX72M**: `arch_mpu_apply` stashes; `kickos_arch_mpu_commit` runs the existing
  RSPAGEn/REPAGEn programming (one-time `MPBAC=0` background, the same-set skip
  cache, the UM sec.17.4.3 readback barrier), bound to the stash. Called from
  `kickos_rx_restore` (the shared switcher/`arch_start` epilogue -- NOT reached by
  the syscall or timer-ISR returns, which `rte` on their own) after the physical
  USP/PSW swap. The SWINT handler runs with `PSW.I=0`, so the register writes are
  already atomic; an `arch_irq_save`/`arch_irq_restore` (IPL) bracket is added as
  nested-safe insurance and to match the seam contract.
- **rv32imac (ESP32-C6, qemu-virt)**: `arch_mpu_apply` stashes; `kickos_arch_mpu_
  commit` writes the 8 `pmpaddr`/`pmpcfg` CSRs (NAPOT) from the stash. Called from
  `.Lswitch` and `arch_start` -- the only paths that physically swap -- after the
  swap, NOT from the shared `.Lrestore` (reached by timer/ssoft/ext/syscall
  returns that do no swap). It runs in the M-mode trap with `MIE=0`, which IS the
  bracket; no `mstatus.MIE` toggle is added (enabling interrupts mid-trap would be
  a bug).

## Build verification (this change)

All eight links compile clean (`exit 0`) and `nm` shows exactly one definition of
each symbol -- no duplicate `arch_mpu_apply` / `kickos_arch_mpu_commit`. (RX uses
the leading-underscore psABI, hence `_arch_mpu_apply` / `_kickos_arch_mpu_commit`.)

| Board (+HAVE_MPU) | `arch_mpu_apply` | `kickos_arch_mpu_commit` |
|---|---|---|
| xmc4800-relax-st (armv7m PMSAv7) | W (stash) | W (PMSAv7) |
| f411disco-st (armv7m PMSAv7) | W (stash) | W (PMSAv7) |
| frdmk64f-st (armv7m SYSMPU) | W (stash) | **T (SYSMPU strong)** |
| picopi-st (armv6m PMSAv7) | W (stash) | W (PMSAv7) |
| qemu (armv7m no-MPU) | W (no-op) | W (empty) |
| rx72m-st (RX MPU) | T (stash) | T (RSPAGEn/REPAGEn) |
| esp32c6-wroom-st (rv32imac PMP) | T (stash) | T (PMP CSRs) |
| qemu-riscv (rv32imac PMP) | T (stash) | T (PMP CSRs) |

## Gating -- BUILD-ONLY, silicon-re-validation required

This change is **build-only**. It is *not* flashed and *not* proven on silicon
for any board except the pre-existing armv6-m/RP2040 fix.

Before merge, each enforcing backend MUST be re-validated **on hardware under
enforcement** (`KICKOS_HAVE_MPU=1`):
- the selftest suite green under enforcement, AND
- a **chain-repro** (the mutex-chain churn that surfaced the race on RP2040)
  running clean for an extended pass -- the fault was timing-dependent, so a
  short run is not sufficient evidence.

Minimum silicon set, each under `KICKOS_HAVE_MPU=1` with selftest green + an
extended chain-repro: XMC4800 (armv7-m PMSAv7, the named validation target),
F411 (PMSAv7), FRDM-K64F (SYSMPU), RX72M (RX MPU), and ESP32-C6 (rv32imac PMP;
qemu-virt covers the PMP path in emulation but the C6 is the silicon witness).
RP2350 PMSAv8 is implemented (`arch/arm/common/arch_arm_pmsav8.cc`, overriding
`kickos_arch_mpu_commit`) and silicon-validated on the pizero2350 (see
`docs/design-rp2350-mpu-armv8m.md`).

Merge is **fable-review-gated**: the seam touches the switch assembly of every
enforcing arch (armv6-m/armv7-m/RX/rv32imac), so the design and each backend's
epilogue hook get a review before it lands.
