<!-- SPDX-License-Identifier: CECILL-C -->
# KickOS -- current state

One screen, and the only file that changes every milestone: read it to re-ground, then go
straight to the record you need. No history and no task lists -- granular items live in
`TODO.md`, the docs map in `docs/README.md`.

## Where we are

On `master`. M4.5.2 is merged; nothing is in flight.

M4.5.2 **stage 2 of the unprivileged root is COMPLETE**: five boards run root with
`privileged=false` plus a `CAP_AUTHORITY`, silicon-witnessed across all **four** enforcement
backends -- `xmc4800-relax` and `f411disco` (PMSAv7), `esp32c6-wroom` (RISC-V PMP),
`pizero2350` (PMSAv8), `rx72m` (RXv3 MPU). `frdmk64f` waits for stage 3.

## What is next (locked order)

1. **4.5.3 -- stage 3**: `arch_periph_enable` (K64F `SIM_SCGC*` + `AIPS0_PACRN`), then flip
   `frdmk64f`.
2. **4.5.4 -- stage 4**: `kos_cap_narrow`, and narrow root's authority cap before
   `kickos_app_main`.
3. **4.5.5**: MPU region-encoding classes, plus a fleet re-witness pass.
4. Delete `KICKOS_ROOT_PRIVILEGED` outright.
5. **M4.6**: consoles / UART.

## Build posture

The whole fleet including the sim builds `MinSizeRel` (`-Os`, with `-g` re-added in
`CMakeLists.txt`). It shipped `-O0` until this milestone, roughly 2x the footprint, so
**every silicon witness taken before it needs re-running** -- the fleet pass under 4.5.5.
Two exceptions: the `bluepill-c8-st` and `f302nucleo-st` images were already `-Os` under the
superseded two-board holding block, `.text` byte-identical. The `f302nucleo` captures are the
only ones taken on optimised code. `-Os` is a preset default, so it is invisible through
`find_package(KickOS)`: an out-of-tree consumer picks its own build type, and the floors in
`reference/porting.md` assume `-Os`.

The single commit to revert when bisecting a footprint or timing regression at the bench is
`build: optimise the fleet (MinSizeRel)`; it is deliberately alone.

## Gates (ctest, all green)

sim 12/12, sim-telem 14/14, qemu 10/10, qemu-m33 9/9, qemu-m7 9/9, qemu-m3 8/8,
qemu-riscv 7/7, microbit 5/5.

## Board matrix

Three classes, which fix what a board can witness:

- **enforcing** -- a privilege ring AND an MPU/PMP backend (`arch/*/chip/<chip>/mpu.cmake`);
  the only class that can witness memory confinement or the root flip.
- **ring-only** -- a real privilege ring, no MPU backend: memory confinement is
  unwitnessable, so the stage-2 gate cannot be met.
- **no-ring** -- no privilege axis in the hardware; `privileged` is a no-op there.

| Board | Chip / core | Class | The fact that decides it |
|---|---|---|---|
| `xmc4800-relax` | XMC4800 / M4F | enforcing | PMSAv7; FLIPPED, the enforcement flagship |
| `f411disco` | STM32F411 / M4F | enforcing | PMSAv7; FLIPPED |
| `esp32c6-wroom` | ESP32-C6 / RV32IMAC | enforcing | RISC-V PMP (NAPOT); FLIPPED |
| `pizero2350` | RP2350 / M33 | enforcing | PMSAv8; FLIPPED |
| `rx72m` | RX72M / RXv3 | enforcing | RX MPU; FLIPPED; no CI gate (vendor toolchain) |
| `frdmk64f` | MK64FN1M0 / M4F | enforcing | SYSMPU; NOT flipped -- its service list writes MMIO from root (stage 3) |
| `blackpill` | STM32F411 / M4F | enforcing | shares the `stm32f411` backend; witnessed on `f411disco`, not re-run here |
| `teensy41` | i.MX RT1062 / M7 | enforcing | PMSAv7 + the M7 anti-speculation fix (ERR011573) |
| `picopi` | RP2040 / M0+ | enforcing | PMSAv6; the M0+ does implement `CONTROL.nPRIV` -- the fleet's only armv6m enforcement proof |
| `qemu` | mps2-an386 / M4 | enforcing | PMSAv7 run gate |
| `qemu-m3` | mps2-an385 / M3 | enforcing | PMSAv7, soft-float (no FP switch path) |
| `qemu-m7` | mps2-an500 / M7 | enforcing | PMSAv7 with 16 regions |
| `qemu-m33` | mps2-an505 / M33 | enforcing | PMSAv8 + full-C++ `cxxtest` |
| `qemu-riscv` | QEMU virt / RV32IMAC | enforcing | PMP run gate |
| `f302nucleo` | STM32F302R8 / M4 | ring-only | the `x8` line has no MPU (the xB/xC line does); the only physically present no-MPU ARM board |
| `bluepill-c8` | STM32F103C8 / M3 | ring-only | `stm32f103` has no MPU; no physical unit -- build-only |
| `due` | SAM3X8E / M3 | ring-only | unit RETIRED (peripheral-I/O fault); the chip HAS an MPU on silicon, the backend was never written |
| `microbit` | nRF51822 / M0 | no-ring | ARMv6-M's privilege extension is optional and the M0 omits it, so `msr control` is discarded and an "unprivileged" thread runs privileged |
| `esp32-wroom` | ESP32-D0WD / Xtensa LX6 | no-ring | no privilege split on the core; MPU/privilege are no-ops |
| `sim` | host process | no-ring | host `mprotect` gives memory enforcement, but `privileged` is ignored |

## Open blockers

- **SPI-service silicon halt**: `k64dspi` and `xmcssc` halt right after console-up.
  Pre-existing, M4.6-era.
- **The XMC privileged-write seam**: `xmc_spi0_start`'s `FDR`/`BRG`/`CCR` stores are silently
  dropped for an unprivileged window holder (measured). The seam is designed and unbuilt --
  `docs/design-unprivileged-root.md`, *The privileged-write seam family*.
- **IRQ non-reclaim**: `irq_detach` has exactly one caller, nothing in thread teardown releases
  a binding, and `irq_register` is ungated -- so any thread can squat any line, permanently.
- **Five in-tree apps grant a DEV window a live board-service driver already holds**, which the
  one-holder-per-window check now refuses. Silicon-only: no in-env gate covers any of them.

## Where to go next

- `docs/README.md` -- the docs map (Book vs Reference, conventions).
- `TODO.md` -- the granular, actionable items.
- `docs/reference/` -- the exact contract; the code wins, drift is a bug.
- `CONTEXT.local.md` -- local rig ops. Gitignored: it exists only in the main checkout.
