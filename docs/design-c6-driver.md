<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# Design brief: ESP32-C6 unprivileged GPIO driver -- the canonical PMP + APM per-thread peripheral-isolation reference

> **Status: LANDED** -- kept as the DECISION record. The register-level facts it used to carry --
> the HP_APM and HP_TEE register table, the two-gate PMP-then-APM model, the "an APM denial does
> not trap" semantics, and the whole-port W1TS/W1TC granularity caveat -- now live in
> `reference/architecture.md` (Memory domains, the peripheral-MMIO matrix), which is code-synced.
> The APM open no longer lives in this app either: `arch_init` (`chip_esp32c6.cc`) programs the
> REE0 background permit at boot for every board, so it covers apps other than this demo.

IMPLEMENTED + PROVEN on silicon (2026-07-17): `user/apps/esp32c6-wroom/c6blink` blinks GPIO10
through the granted PMP window (APM opened) then an ungranted `GPIO_ENABLE` poke PMP-faults
(mcause=7). The earlier "boot-loop" was an elf2image RAM-only-header flag error, not a code bug.

> **That 2026-07-17 result predates a defect found 2026-07-30 and is retrospectively a pass by
> LUCK, not a clean witness.** `reference/boards.md` (the `esp32c6-wroom` matrix row and *M4.5.6*)
> carries the retrospective and the post-fix re-witness that closes it. Read the verdicts below as
> confirmed by the LATER capture, not by the original one.

The RISC-V analog of the F411 canonical PMSA proof (`design-spi-driver-stm32f411.md`): prove a
granted per-thread peripheral window WORKS and an ungranted poke FAULTS, per thread -- but on the
C6 that per-thread PMP line sits on top of a coarse, one-time **APM** background permit the fleet
had never driven. Builds on the landed MMIO-grant seam (`design-task9-mmio-driver.md`), the k64drv
privileged-shim-to-unprivileged-driver structure, and the rv32imac PMP backend plus U-mode fault
routing.

## Decisions

1. **A plain GPIO toggle, not a timer-IRQ driver, for the first cut.** The simplest peripheral
   that exercises the two-gate model with the fewest moving parts: no IRQ, one small MMIO window,
   an externally observable output. It isolates exactly what is being proven, with no IRQ plumbing
   confounding the result. The timer path is the natural follow-on but is NOT a pure userspace
   addition on the C6 (see open question 3), so it would have broken the "no kernel change"
   property that made `k64drv` and `f411spi` clean.

2. **Not the onboard WS2812 (LED2, GPIO8).** RMT-driven, a strapping pin, and already a privileged
   kernel diag (`arch_diag_led_*`). Use a plain non-strapping header GPIO driven push-pull --
   **GPIO10** (strapping: 8/9/15; USB-JTAG: 12/13; console UART: 16/17).

3. **The granted window is the pin BANK, and everything that ROUTES stays out of it.** What makes
   the window a capability rather than a share of the block is the exclusions, not the inclusions:
   the matrix out-sel and the per-pin config/interrupt registers are outside it, and IO MUX, PCR
   and the APM/TEE registers are never granted at all -- so the holder drives and reads its bank
   but cannot ungate a peripheral, re-mux a pad, or route a peripheral signal onto one. The exact
   extent and its honest granularity caveat are in `reference/architecture.md`.

   **This brief scoped a tighter window than shipped, and the CODE is the contract**: it proposed
   8 bytes at `0x6009_1008` covering only `W1TS`/`W1TC`, with direction programmed by the shim
   beforehand. The shipped `c6blink` grants 64 bytes at the block base and lets the driver set its
   own direction via `ENABLE_W1TS`. The tighter window was rejected in practice for the reason this
   brief itself listed as its cost -- it re-touches the grant whenever the register set moves --
   and both windows encode as one PMP NAPOT entry, so the tightening bought no enforcement.

4. **Both mux stages go through the mediated `arch_pinmux_set` seam, not raw MMIO from root.** The
   kernel refuses a kernel-owned pin on BOTH stages, which is what leaves no raw MMIO in the app's
   `main` at all. `arch_init` already opened the REE0 permit, so the shim's whole job is the pin.

5. **The APM open is a ONE-TIME, TEE-mode, per-security-mode act, and it belongs in `arch_init`.**
   It is not per-thread and not per-driver: all REE0 U-threads share it, so putting it in a driver
   would have made every future driver re-do it. Its extent is wider than this brief scoped --
   regions 1..3 cover the complement of the HP-bus Rule 7 reserved blocks -- which is the
   "broad background permit" option below, narrowed by the reserved set rather than by a
   hand-picked block.

6. **Region 0 is LEFT at its reset values as the deny catch-all.** Overlap semantics are a permit
   UNION, so a later permit region beats it on the overlap while the catch-all keeps everything
   else APM-closed to U-mode. Not writing region 0 is the decision: it is already exactly right.

7. **The isolation proof rides the PMP fault, never APM.** An APM denial does not trap, so a test
   scoped at APM would report "no fault" and be indistinguishable from broken enforcement. The
   negative test therefore pokes a register INSIDE the APM-open extent but OUTSIDE the PMP window
   -- `GPIO_ENABLE` in the same GPIO block -- which is the sharpest available discriminator: same
   block, same background permit, and it still faults, *because* PMP draws the per-thread line.
   A second axis (a different peripheral entirely, e.g. IO MUX) also faults but conflates the two
   gates.

8. **Announce-before-poke** (the k64drv idiom), and the terminal negative test is the LAST thing
   the app does.

## Scoping alternatives considered for the APM permit

- **Tightest**: a region matching the PMP window exactly. Minimal, but it re-touches APM whenever
  the window moves.
- **Block-level**: one region over the GPIO Matrix slot. What this brief originally recommended;
  it keeps the APM belt-and-suspenders over the escalation surfaces (IO MUX, PCR).
- **Broadest**: all of HP_PERI, REE0 R/W-NX. Cleanest conceptually (APM = coarse background, PMP =
  per-thread) and future drivers need no APM edit, but it removes the second lock over the
  escalation surfaces, leaving only PMP between a U-thread and them.

What shipped is the broad form MINUS the Rule 7 reserved blocks, which keeps the double lock over
exactly the surfaces that matter (INTMTX, and the contiguous PCR..HP_APM span) while needing no
per-driver APM edit. Defense note either way: even the broad open cannot self-escalate, because APM
config writes require TEE mode, so a REE thread with a hypothetical PMP window over the APM
registers still could not reprogram them -- and the grant path refuses that window anyway.

## What it proves (and the fleet contrast)

Per-thread peripheral isolation on the C6 = **a PMP per-thread NAPOT window (granted blink works;
an ungranted same-block poke faults `mcause` 5/7) layered over a one-time APM REE0 background
open**. Distinguished from the F411 PMSA proof only by that extra open, since PMSA has no
second bus-side gate.

Contrast with K64F, and it is the structural point: there the AIPS bridge is the SOLE, coarse,
per-4-KiB-slot, all-user gate and the SYSMPU window is INERT for peripherals, so no per-thread
peripheral line exists at all (`k64drv`). The C6 delivers the line K64F structurally cannot, at the
cost of the background permit K64F does not need.

Region budget: the unprivileged driver spends app code (RX) + app data (RW) + private stack (RW) +
the GPIO window (RW-NX) = **4 of 8** PMP entries. Comfortable. APM cost: three regions of 16, with
region 0 still the deny catch-all.

## Open questions / risks

1. **Whole-port W1TS/W1TC granularity** -- the PMP line is register-bank-level, not single-pin (a
   register limit, not a PMP limit). Documented in `reference/architecture.md`; acceptable for the
   isolation proof, and a real caveat for anyone reasoning about the C6 boundary.
2. **APM M-path and REE0 mapping** were confirmed by elimination rather than by reading a register
   back: the HP CPU is HP_APM master **M0** (16.3.2 states M1 carries all masters except the HP and
   LP CPUs, and the 16.5 worked example plus the default posture both use M0), and
   `TEE_M0_MODE_CTRL` reset 0 maps U-mode to REE0. Both hold on silicon, since `c6blink` runs at
   all; a direct read of `HP_APM_M0_EXCEPTION_*` under a controlled denial would confirm them
   positively and has not been taken.
3. **A timer-IRQ follow-on needs an arch/chip change**, which is why GPIO was the first cut. The C6
   real-device IRQ dispatch is single-source: the one real device wired through the interrupt matrix
   is UART0 TX -> CPU int 30 -> `.Lextdev`, HARD-CODED to the console line, and the tier-1 userspace
   IRQ path otherwise reaches `kickos_isr_irq` only via the software-inject doorbell (test
   scaffolding, not a device). A systimer or TIMG userspace driver requires generalizing
   `kickos_rv_ext_dispatch_dev` to PLIC-claim-demux a logical line (or a second device CPU int plus
   a `switch.S` vector) so `kos_irq_claim`/`kos_irq_wait`/`kos_irq_ack` reaches a REAL device. Then the
   W1C-before-re-arm storm rule applies as usual.
