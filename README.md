<!--
SPDX-License-Identifier: CECILL-C
Copyright (c) 2026 Philippe Leduc
-->

# KickOS

A small **microkernel** RTOS with a clear userspace/kernel separation, MPU-first
per-task isolation, an event-driven **tickless** scheduler, and a **first-class x86
host "sim"** that runs the real kernel + userspace as one Linux process.

See [`docs/architecture.md`](docs/architecture.md) for the full design.

## Status

Milestone **M1** — privilege + SVC on real MCUs (no hardware MPU yet; that is
M2). On top of the M0 x86 sim, the same kernel + userspace now cross-compiles for
**two ARM arch backends** (armv7m Cortex-M3/M4/M4F, armv6m Cortex-M0/M0+) across
**nine chip backends**: `mps2` (QEMU) and `nrf51` (QEMU micro:bit) are
QEMU-validated; `rp2040` (Raspberry Pi Pico) is **hardware-validated**; `mk64f`,
`stm32f411`, `stm32f103`, `stm32f302`, `sam3x8e` (Arduino Due) and `xmc4800` are
build-verified (flash to run). Two non-ARM ports -- **ESP32/Xtensa LX6** and
**Renesas RX72M/RXv3** -- are implemented and build-verified (HW validation pending).

See [`docs/porting.md`](docs/porting.md) for the per-target status and how to add
a board.

## Building

```sh
# Host sim (runs the full test suite in CI):
cmake --preset sim && cmake --build --preset sim && ctest --preset sim --output-on-failure

# An MCU target (e.g. the Raspberry Pi Pico); flash the resulting image:
cmake --preset picopi && cmake --build --preset picopi
```

Runnable QEMU gates: `ctest --preset qemu` (Cortex-M4) and `ctest --preset
microbit` (Cortex-M0). Other MCU presets: `frdmk64f`, `f411disco`, `bluepill`,
`f302nucleo`, `due`, `xmc4800`.

## License

CeCILL-C V1.0 (LGPL-compatible, file-level copyleft). See [`LICENSE`](LICENSE).
Every source file carries an `SPDX-License-Identifier: CECILL-C` header.

Design ideas are studied from other RTOSes (NuttX, Argon, RIOT, ChibiOS,
µC/OS-III, RTEMS, ThreadX, RT-Thread) but never copied — see the *Licensing &
clean-room discipline* section of the architecture doc.
