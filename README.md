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

Milestone **M0** — the x86 sim: a single native Linux ELF that boots the real
kernel, schedules threads, and runs an unprivileged user app across the syscall
boundary. No hardware; runnable in CI.

## Building the sim

```sh
cmake --preset sim
cmake --build --preset sim
ctest --preset sim --output-on-failure
```

## License

CeCILL-C V1.0 (LGPL-compatible, file-level copyleft). See [`LICENSE`](LICENSE).
Every source file carries an `SPDX-License-Identifier: CECILL-C` header.

Design ideas are studied from other RTOSes (NuttX, Argon, RIOT, ChibiOS,
µC/OS-III, RTEMS, ThreadX, RT-Thread) but never copied — see the *Licensing &
clean-room discipline* section of the architecture doc.
