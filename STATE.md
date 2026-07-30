<!-- SPDX-License-Identifier: CECILL-C -->
# KickOS -- current state

One screen, and the only file that changes every milestone: read it to re-ground, then go
straight to the record you need. No history and no task lists -- granular items live in
`TODO.md`, the docs map in `docs/README.md`.

## Where we are

On branch `M4.5.4-unpriv-root-stage4`, off `master`. **Silicon-witnessed on two of the six flipped
boards** (2026-07-30): `xmc4800-relax` (PMSAv7, 65 cases / 64 `ok` / 1 skip) and
`frdmk64f` (SYSMPU, full service list, 65 / 63 / 2), each with `ok 46 - authority_cap` and TAP
routed through its userspace console driver. Read those two rows precisely: `selftest` declares five
of the six bits, so root gave up only `AUTH_PSTATE` there. The capture where root actually drops
`AUTH_PINMUX` and `AUTH_CONSOLE` is XMC `consoledemo`, which declares no mask and still prints
through the driver it handed off. `docs/reference/boards.md`, *Stage 4*. The other four flipped
boards rest on emulation, and the three apps that declare a wider mask (`c6blink`, `rxdrv`,
`f411spi`) are **still unrun** -- they are the direct witness that a per-app declaration carries a
board's own pin muxing on hardware.

**Stage 4 of the unprivileged root is COMPLETE in-tree**: root now hands the app only the authority
the app declared. The authority set is re-cut into **six** bits -- `AUTH_MEMORY`, `AUTH_PINMUX`,
`AUTH_PSTATE`, `AUTH_IRQ`, `AUTH_SYSTEM`, `AUTH_CONSOLE`; `AUTH_DEVICE` and `AUTH_CLOCK` are gone --
funded by moving the authority word out of `CapEntry.rights` into the poolless `CapEntry.obj`, which
took an explicit type refusal at the delegation site first. `kos_cap_narrow`
(`KOS_SYS_CAP_NARROW = 40`) is ungated and can only clear bits; the default init calls it after the
pin map and the service list, with the mask from a per-app `KICKOS_APP_AUTHORITY` whose default is
`AUTH_MEMORY | AUTH_SYSTEM`. **It bites only where root is unprivileged** --
`cap_check_authority` short-circuits on `Thread::privileged` -- so on a privileged-root board it is
inert and answers `-KOS_EBADF`. `stress`'s three privileged spawns turned out to be the same
leftover `selftest`'s were, and flipping them fixed a real `sim_stress` failure under
`KICKOS_ROOT_PRIVILEGED=OFF`.

**Stage 3 is COMPLETE**: `arch_periph_enable` mediates the clock-ungate
plus the bus-side unprotect for one register block, keyed on that block's exact base and gated on
**possession** of a live DEV window, not on an authority bit. Six boards run root with
`privileged=false` plus a `CAP_AUTHORITY`, silicon-witnessed across all **five** enforcement
backends -- `xmc4800-relax` and `f411disco` (PMSAv7), `esp32c6-wroom` (PMP), `pizero2350` (PMSAv8),
`rx72m` (RXv3 MPU), `frdmk64f` (SYSMPU). `frdmk64f` is the first flipped on its **full** service
list (`k64uart` + `k64dspi`) and writes no MMIO from root; `xmc4800-relax` stays console-only,
because `xmcssc` needs the USIC `FDR`/`BRG`/`CCR` seam that stage 3 does not cover.

## What is next (locked order)

1. **Finish witnessing 4.5.4**, then merge it. Two enforcement backends are done; what is owed is
   `c6blink` / `rxdrv` / `f411spi`, the apps whose own `KICKOS_APP_AUTHORITY` mask is what makes them
   work, plus their never-run `kos_periph_enable` possession probes.
2. **4.5.5**: MPU region-encoding classes, plus a fleet re-witness pass.
3. Delete `KICKOS_ROOT_PRIVILEGED` outright.
4. **M4.6**: consoles / UART.
5. **M4.6.1**: USB CDC console on `picopi`, `pizero2350` and `teensy41`, the boards whose console
   needs an external adapter today. One shared CDC class over two controller backends (the RP
   DPRAM block, the RT1062 ChipIdea OTG). After M4.6 because it needs the IRQ-driven driver work
   and the IRQ-reclaim fix.

## Build posture

The whole fleet including the sim builds `MinSizeRel` (`-Os`, with `-g` re-added in
`CMakeLists.txt`). It shipped `-O0` until M4.5.2, roughly 2x the footprint, so **every silicon
witness taken before it needs re-running** -- the fleet pass under 4.5.5, and not a formality: on
the K64F `-Os` dropped a PIT clock-gate-race write that `-O0` had masked.
Two exceptions: the `bluepill-c8-st` and `f302nucleo-st` images were already `-Os` under the
superseded two-board holding block, `.text` byte-identical. The `f302nucleo` and `frdmk64f` captures
are the only ones taken on optimised code. `-Os` is a preset default, so it is invisible through
`find_package(KickOS)`: an out-of-tree consumer picks its own build type, and the floors in
`reference/porting.md` assume `-Os`.

The single commit to revert when bisecting a footprint or timing regression at the bench is
`build: optimise the fleet (MinSizeRel)`; it is deliberately alone.

## Gates (ctest, all green)

sim 12/12, sim-telem 14/14, qemu 10/10, qemu-m33 9/9, qemu-m7 9/9, qemu-m3 8/8,
qemu-riscv 7/7, microbit 5/5. The selftest is 65 tests, 0 skipped.

Off-preset, run by hand for stage 4: `qemu` at `-DKICKOS_ROOT_PRIVILEGED=OFF` is 10/10. `sim` at
`OFF` is 12/13 -- the one failure is the gate's own limitation, not the kernel's: `ctest` treats any
`# skipped:` as failure while `mpu_privileged_guard` correctly skips in that posture, and the sim
registration has no `EXPECT_SKIPS` list like `check_qemu_selftest.sh`. Filed in `TODO.md` with the
CI items.

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
| `frdmk64f` | MK64FN1M0 / M4F | enforcing | SYSMPU; FLIPPED, the only board flipped on its full service list |
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

- **A published console makes an app's own diagnostics invisible**: `console_emit` drops every
  `kos::print` line once the console is `USER_OWNED`, so app output survives only on RTT
  (`KICKOS_CONSOLE=both`, which no board preset carries). Neither SPI service halts -- `k64dspi`
  and `xmcssc` are both witnessed doing their work on silicon (`docs/reference/boards.md`); the
  handover ordering that would restore visibility is a `TODO.md` item, M4.6-era.
- **The XMC privileged-write seam**: `xmc_spi0_start`'s `FDR`/`BRG`/`CCR` stores are silently
  dropped for an unprivileged window holder (measured). `arch_periph_enable` covers the
  clock-gate/bus-protect member of the seam family only, so this one is still unbuilt and
  `xmc4800-relax` runs console-only under the flip --
  `docs/design-unprivileged-root.md`, *The privileged-write seam family*.
- **IRQ non-reclaim**: `irq_detach` has exactly one caller, nothing in thread teardown releases
  a binding, and `irq_register` is ungated -- so any thread can squat any line, permanently.
- **Five in-tree apps grant a DEV window a live board-service driver already holds**, which the
  one-holder-per-window check now refuses. Silicon-only: no in-env gate covers any of them.
- **A missed `KICKOS_APP_AUTHORITY` declaration surfaces only at runtime, on a flipped board.** The
  kernel cannot know what an app will call, so there is no configure-time equivalent of the
  root-MMIO `FATAL_ERROR`. The failure is a checked `-KOS_EPERM` rather than a fault, but on a board
  with a published console the diagnostic is invisible (first blocker above). The nastiest shape is
  an app that ignores a failed `pinmux_set` and then drives an unmuxed pin.
- **`f302nucleo-st` has not linked in Debug for some time** -- `.text` overflows FLASH by 4,548 B on
  `master`, pre-dating stage 4, and no gate builds that board in Debug so nothing caught it. Ample
  room at `-Os` (12.7 KiB free).

## Where to go next

- `docs/README.md` -- the docs map (Book vs Reference, conventions).
- `TODO.md` -- the granular, actionable items.
- `docs/reference/` -- the exact contract; the code wins, drift is a bug.
- `CONTEXT.local.md` -- local rig ops. Gitignored: it exists only in the main checkout.
