<!--
SPDX-License-Identifier: CECILL-C
Copyright (c) 2026 Philippe Leduc
-->

# KickOS

A small **microkernel** RTOS with a clear userspace/kernel separation, MPU-first
per-task isolation, an event-driven **tickless** scheduler, and a **first-class x86
host "sim"** that runs the real kernel + userspace as one Linux process.

Current state: [`STATE.md`](STATE.md). Documentation map: [`docs/README.md`](docs/README.md).
Full design: [`docs/reference/architecture.md`](docs/reference/architecture.md).

## Status

Version **0.4.5** -- **M4, the driver era**. The same kernel + userspace runs on **13 MCU boards
across 5 ISAs** -- armv7m (Cortex-M3/M4/M4F/M7/M33), armv6m (Cortex-M0/M0+), Renesas **RXv3**,
Xtensa **LX6**, and **RV32IMAC** -- plus the host **sim** and three emulator gates (QEMU
`mps2-an386`, micro:bit / nRF51, QEMU riscv `virt`). Every board boots, has a console, runs the
selftest, panics visibly, and runs at its true (or safely-degraded) clock.

Shipped since M1:

- **M2 -- hardware MPU enforcement.** Per-task isolation is real on silicon: the cross-domain
  fault trap is proven on **SYSMPU** (K64F), **PMSAv7** (XMC4800, i.MX RT1062), **PMSAv6-M**
  (RP2040), **PMSAv8** (RP2350), **RISC-V PMP** (ESP32-C6, QEMU `virt`) and the **RX72M MPU** --
  an unprivileged cross-domain store faults and does not complete. The sim enforces via
  `mprotect`. Parts with no per-task unit (Xtensa LX6, nRF51, STM32F103) stay privilege + SVC.
- **M3 -- capabilities.** Per-task typed handle tables with a single `cap_resolve` chokepoint,
  refcounted destroy-on-last-close, authenticated-grant spawn delegation, and synchronous
  **call/reply** over endpoints (the reply capability) -- all validated under enforcement.

Silicon boards include the XMC4800, FRDM-K64F, STM32F411 (f411disco / blackpill), STM32F302,
RP2040 (Pico), RP2350 (pizero2350), i.MX RT1062 (Teensy 4.1), Renesas RX72M, ESP32-WROOM and
ESP32-C6. See [`docs/m2-readiness.md`](docs/m2-readiness.md) for the validated per-board
matrices, [`docs/reference/boards.md`](docs/reference/boards.md) for per-board wiring,
[`roadmap.md`](roadmap.md) for the milestone plan (**M4 = the driver era,
M5 = SMP**), and [`docs/reference/porting.md`](docs/reference/porting.md) for how to add a target.

### What CI actually gates, per ISA

The five ISAs are **not** covered equally, and the asymmetry is structural rather than an
oversight -- see the header of [`.github/workflows/ci.yml`](.github/workflows/ci.yml):

| ISA | CI gate | MPU enforcement in CI |
|---|---|---|
| host **sim** | full `ctest` (the authoritative gate) | **runtime** (`mprotect`) |
| **rv32imac** | QEMU `virt` run gate | **runtime** (PMP) |
| **armv7m** | four MPS2 QEMU run gates (an386/an505/an500/an385) + a board build sweep | **runtime** (PMSAv7 + PMSAv8) |
| **armv6m** | QEMU run gate (micro:bit) + `picopi` build | **build only** |
| **Xtensa LX6** | build only (no upstream QEMU ESP32 machine model) | -- (no per-task unit) |
| **Renesas RX** | **none** | -- |

ARM enforcement is a *run* gate since `mps2` grew an enforcement block: the four MPS2 images
run the full TAP suite as unprivileged threads and take a real MemManage denial, over both
PMSA revisions (v7 on the M4/M7/M3, v8 on the an505's M33). The backends QEMU carries no model
for -- SYSMPU, the M7 anti-speculation wrap, PMSAv6 -- stay silicon-validated; see
[`docs/reference/boards.md`](docs/reference/boards.md). **RX has no gate at all**: RX72M needs
`-misa=v3` and `-mdfpu`, which exist only in the registration-gated Renesas GNURX build (upstream
`rx-elf` GCC rejects both), so it cannot be built on a hosted runner and remains bench-validated.

## Building

Configuration is Kconfig; CMake keeps the build graph. The workflow is the usual Kconfig one,
spelled in CMake's verbs -- a preset selects the board's stored defconfig, the menu edits the
live configuration, and the build follows it:

```sh
cmake --preset frdmk64f-st -B build/k64   # seed .config from the board's defconfig
ninja -C build/k64 menuconfig             # edit the live configuration
ninja -C build/k64                        # build what .config now says
ninja -C build/k64 savedefconfig          # persist it back to the board's defconfig
ninja -C build/k64 defconfig              # or reload, discarding local edits
rm -rf build/k64                          # distclean: nothing is written in-tree
```

`boards/<board>/configs/<variant>/defconfig` is the saved, reviewable starting point, kept in
git. `<build>/generated/.config` is the live state the build is based on, and it is
authoritative once it exists -- exactly as a preset seeds `CMakeCache.txt` once and the cache
rules afterwards. So editing a defconfig reaches a NEW build directory, or an existing one
after `ninja defconfig`.

Nothing generated is written into the source tree: `.config`, the C header the compile reads
(`kickos/board_config.h`) and the CMake fragment all live under `<build>/generated/`.

`menuconfig` needs `kconfiglib` (build-time only, one pure-Python file, ISC, never in a
shipped artefact). Put it in a venv of its own and point `KICKOS_KCONFIG_PY` at that
interpreter by absolute path; `cmake` prints the exact recipe if it cannot import it. A
packaged `kconfig-mconf` parses this tree unmodified and works too.

```sh
# Host sim (runs the full test suite in CI):
cmake --preset sim && cmake --build --preset sim && ctest --preset sim --output-on-failure

# An MCU target (e.g. the Raspberry Pi Pico); flash the resulting image:
cmake --preset picopi && cmake --build --preset picopi
```

Runnable emulator gates: `ctest --preset qemu` (Cortex-M4), `ctest --preset microbit`
(Cortex-M0), `ctest --preset qemu-riscv` (RV32IMAC). The MCU presets are `frdmk64f`,
`teensy41`, `f411disco`, `blackpill`, `bluepill-c8`, `f302nucleo`, `picopi`, `pizero2350`,
`due`, `xmc4800-relax`, `esp32-wroom`, `esp32c6-wroom`, and `rx72m` -- each with a `-st`
selftest variant. Add `-DKICKOS_HAVE_MPU=1` on any board whose chip ships an enforcement block
(`arch/*/chip/<chip>/mpu.cmake`); on one that does not, asking for it is refused at configure
with the declaration's own explanation rather than becoming a silent no-op. That flag becomes a
defconfig line rather than a command-line argument when the posture becomes a variant. Flashing is per-board -- see [`docs/flashing.md`](docs/flashing.md) and
[`docs/reference/boards.md`](docs/reference/boards.md).

**Cross toolchains.** A cross build finds its compiler through a per-family hint --
`KICKOS_ARM_TOOLCHAIN_BIN`, `KICKOS_RISCV_TOOLCHAIN_BIN`, `KICKOS_RX_TOOLCHAIN_BIN`,
`KICKOS_XTENSA_BIN` -- seeded from the environment, overridable with `-D`, and falling back to
`PATH` when empty. Keep local pins in `.session/env.sh` (gitignored) and `source` it before
configuring. The ARM and RISC-V toolchain files then *verify* the compiler they resolved: one
without newlib + `libstdc++` for the board's own multilib is refused at configure time, naming
what was missing, rather than failing dozens of build steps later on `#include <exception>`.
See [`docs/reference/porting.md`](docs/reference/porting.md).

## License

CeCILL-C V1.0 (LGPL-compatible, file-level copyleft). See [`LICENSE`](LICENSE).
Every source file carries an `SPDX-License-Identifier: CECILL-C` header.

Design ideas are studied from other RTOSes (NuttX, Argon, RIOT, ChibiOS,
uC/OS-III, RTEMS, ThreadX, RT-Thread) but never copied -- see the *Licensing &
clean-room discipline* section of the architecture doc.
