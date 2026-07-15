<!--
SPDX-License-Identifier: CECILL-C
Copyright (c) 2026 Philippe Leduc
-->

# KickOS

A small **microkernel** RTOS with a clear userspace/kernel separation, MPU-first
per-task isolation, an event-driven **tickless** scheduler, and a **first-class x86
host "sim"** that runs the real kernel + userspace as one Linux process.

Documentation map: [`docs/README.md`](docs/README.md). Full design:
[`docs/reference/architecture.md`](docs/reference/architecture.md).

## Status

Milestone **M1 is complete**: the same kernel + userspace runs on **10 boards across 5 ISAs**
-- armv7m (Cortex-M3/M4/M4F), armv6m (Cortex-M0+), Renesas **RXv3**, Xtensa **LX6**, and
**RV32IMAC** -- plus the host **sim** and three emulator gates (QEMU `mps2-an386`, micro:bit /
nRF51, QEMU riscv `virt`). Every board boots, has a console, runs the selftest, panics visibly,
and runs at its true (or safely-degraded) clock. This is all on **privilege + SVC**; hardware MPU
enforcement is the next milestone.

Silicon boards include the XMC4800, STM32F411 (f411disco / blackpill), STM32F302, STM32F103
(bluepill), RP2040 (Pico), Renesas RX72M, ESP32-WROOM, and ESP32-C6. See
[`M1_state.md`](M1_state.md) for the validated per-board matrix, [`roadmap.md`](roadmap.md) for
the milestone plan (M2 = MPU enforcement, M3 = capabilities, M4 = SMP), and
[`docs/reference/porting.md`](docs/reference/porting.md) for how to add a target.

## Building

```sh
# Host sim (runs the full test suite in CI):
cmake --preset sim && cmake --build --preset sim && ctest --preset sim --output-on-failure

# An MCU target (e.g. the Raspberry Pi Pico); flash the resulting image:
cmake --preset picopi && cmake --build --preset picopi
```

Runnable emulator gates: `ctest --preset qemu` (Cortex-M4), `ctest --preset microbit`
(Cortex-M0), `ctest --preset qemu-riscv` (RV32IMAC). MCU presets include `frdmk64f`,
`f411disco`, `blackpill`, `bluepill`, `f302nucleo`, `due`, `xmc4800-relax`, `esp32-wroom`,
`esp32c6-wroom`, and `rx72m` (each with a `-st` selftest variant). Flashing is per-board --
see [`docs/flashing.md`](docs/flashing.md) and [`docs/reference/boards.md`](docs/reference/boards.md).

## License

CeCILL-C V1.0 (LGPL-compatible, file-level copyleft). See [`LICENSE`](LICENSE).
Every source file carries an `SPDX-License-Identifier: CECILL-C` header.

Design ideas are studied from other RTOSes (NuttX, Argon, RIOT, ChibiOS,
uC/OS-III, RTEMS, ThreadX, RT-Thread) but never copied -- see the *Licensing &
clean-room discipline* section of the architecture doc.
