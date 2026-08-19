<!--
SPDX-License-Identifier: CECILL-C
Copyright (c) 2026 Philippe Leduc
-->

# KickOS

A capability-based microkernel RTOS, written from scratch: no vendor HAL, no vendor SDK, no
CMSIS pack, no third-party kernel code. The privileged kernel holds threads, memory domains,
IPC, capabilities and IRQ routing. Console, device drivers and buses are unprivileged userspace
servers reached over IPC.

The tree carries 20 board targets -- 14 MCU boards, five QEMU machines, and a host "sim" that
runs the same kernel and the same userspace as one Linux process -- across five instruction
sets: armv7m, armv6m, RXv3, RV32IMAC and Xtensa LX6.

## What is different about it

- **The root thread is unprivileged on every board.** Not a posture and not a build option:
  there is no configuration in which application code starts privileged. Everything a task can
  reach, it was handed.
- **Memory confinement uses whatever the silicon actually has** -- ARM PMSAv6/v7/v8, NXP
  SYSMPU, the Renesas RX-MPU, RISC-V PMP, and `mprotect` on the host. One arch-independent
  memory-domain model over a `{base, size, attr}` region contract, with ten chip region
  backends and the host one behind it. Where a chip has no unit that faults, the enforcing
  posture is not offered at all, rather than configured into a silent no-op.
- **Objects are reached by capability, never by a global id.** A per-task typed handle table, a
  single resolve chokepoint, refcounted destroy-on-last-close, and rights that can only narrow
  when a handle is delegated to a child.
- **The scheduler is tickless and event-driven.** It switches on any event -- yield, block,
  semaphore post, device IRQ -- and a periodic tick is available but never the only trigger.
- **The host sim is the real kernel**, not a mock: the same scheduler, the same syscall path
  across the same SVC boundary, the same capability tables, with `mprotect` standing in for the
  MPU. It is the authoritative test gate, and it needs no board and no emulator.
- **A port is a seam, not a fork.** A new CPU means implementing `arch.h` plus a chip backend
  (reset and vector table, clock tree, console, linker script); the kernel is not restructured
  for it. The contract is [`docs/reference/porting.md`](docs/reference/porting.md).
- **Consuming it is plain CMake.** `find_package(KickOS)`, `add_executable`, link `kickos`, and
  write an ordinary `main`. Two standalone downstream projects, one hosted and one bare-metal,
  are in [`examples/oot-app/`](examples/oot-app/) and
  [`examples/oot-mcu-app/`](examples/oot-mcu-app/).

Design ideas are studied from other RTOSes -- NuttX, Argon, RIOT, ChibiOS, uC/OS-III, RTEMS,
ThreadX, RT-Thread, and seL4 and Zircon for the microkernel paradigm itself -- and never
copied. Register definitions are hand-written from the reference manuals.

## Does it run on your board

Preset names are board names. What each board has actually been proven to do on silicon, its
console pins, LED and flash recipe, are all in
[`docs/reference/boards.md`](docs/reference/boards.md).

| Board (preset) | Part / core | Memory confinement |
|---|---|---|
| **armv7m** | | |
| `frdmk64f` | MK64FN1M0 / Cortex-M4F | SYSMPU |
| `xmc4800-relax` | XMC4800 / Cortex-M4F | PMSAv7 |
| `f411disco` | STM32F411 / Cortex-M4F | PMSAv7 |
| `blackpill` | STM32F411 / Cortex-M4F | PMSAv7 |
| `teensy41` | i.MX RT1062 / Cortex-M7 | PMSAv7 |
| `pizero2350` | RP2350 / Cortex-M33 | PMSAv8 |
| `f302nucleo` | STM32F302R8 / Cortex-M4 | none (this line has no MPU) |
| `bluepill-c8` | STM32F103C8 / Cortex-M3 | none |
| `due` | AT91SAM3X8E / Cortex-M3 | none |
| `qemu`, `qemu-m7`, `qemu-m3` | QEMU MPS2 an386 / an500 / an385 | PMSAv7 |
| `qemu-m33` | QEMU MPS2 an505 / Cortex-M33 | PMSAv8 |
| **armv6m** | | |
| `picopi` | RP2040 / Cortex-M0+ | PMSAv6 |
| `microbit` | nRF51822 / Cortex-M0 | none |
| **RV32IMAC** | | |
| `esp32c6-wroom` | ESP32-C6 | PMP (NAPOT) |
| `qemu-riscv` | QEMU `virt` | PMP (NAPOT) |
| **RXv3** | | |
| `rx72m` | Renesas RX72M | RX-MPU |
| **Xtensa LX6** | | |
| `esp32-wroom` | ESP32-D0WD | none (no per-task unit) |
| **host** | | |
| `sim` | Linux process | `mprotect` |

Fourteen of those boards enforce with a hardware unit. The rest still run their root thread
unprivileged where the core has a privilege ring at all; on the two parts with neither a ring
nor a unit (nRF51, LX6) the authority word is software and still refuses.

## Building

CMake 3.24 or newer, Ninja, a host C++ compiler for the sim, and a cross toolchain per target
family. `kconfiglib` is needed to configure any build.

```sh
# Host sim: build it and run the full test suite.
cmake --preset sim && cmake --build --preset sim && ctest --preset sim --output-on-failure

# An MCU target, e.g. the Raspberry Pi Pico; flash the resulting image.
cmake --preset picopi && cmake --build --preset picopi
```

Emulator run gates: `ctest --preset qemu` (Cortex-M4), `qemu-m7`, `qemu-m3`, `qemu-m33`,
`microbit` (Cortex-M0), `qemu-riscv` (RV32IMAC). Flashing a real board is per board:
[`docs/flashing.md`](docs/flashing.md) for the tool backends,
[`docs/reference/boards.md`](docs/reference/boards.md) for the wiring.

### Presets: a board and a variant, nothing else

```sh
cmake --preset frdmk64f        # the board's base variant
cmake --preset frdmk64f-st     # + the self-test (TAP) suite
cmake --preset frdmk64f-flat   # the non-enforcing posture, on a board that can enforce
```

52 presets over 20 boards, defined in [`cmake/presets/`](cmake/presets/) and each building into
its own directory under `build/`. The memory posture is part of the variant, so there is no
`-D` for it: the 14 enforcing boards state enforcement in their base defconfig and carry a
`<board>-flat` preset beside it, which is what the ring-only gates build. `sim` has no flat
variant, because host `mprotect` is the only posture it has. Boards whose base variant is
already a test image (`sim`, the four `qemu` machines, `qemu-riscv`, `microbit`) have no `-st`.

### Configuration

Configuration is Kconfig; CMake keeps the build graph.
`boards/<board>/configs/<variant>/defconfig` is the saved, reviewable starting point kept in
git, and a preset seeds the live `.config` from it once.

```sh
ninja -C build/frdmk64f menuconfig      # edit the live configuration
ninja -C build/frdmk64f                 # build what it now says
ninja -C build/frdmk64f savedefconfig   # write it back to the defconfig, minimally
ninja -C build/frdmk64f defconfig       # reload from the defconfig, discarding edits
rm -rf build/frdmk64f                   # distclean: nothing was written in-tree
```

`.config` is authoritative once it exists, exactly as `CMakeCache.txt` is once a preset has
seeded it. So editing a defconfig reaches a new build directory, or an existing one after
`ninja defconfig`. Nothing generated lands in the source tree: `.config`, the C header the
compile reads and the CMake fragment all live under the build directory's `generated/`.

A new posture is therefore a new defconfig under `boards/<board>/configs/`, not a flag. The
enforcing posture is offered only where a wild cross-domain access actually faults, which needs
both the chip's linker script to carve the protected window and its arch to ship a real region
backend; a chip declares that pair by selecting `HAS_MPU`.

`kconfiglib` is build-time only -- one pure-Python file, ISC-licensed, never in a shipped
artefact. Put it in a venv of its own and name that interpreter absolutely through
`KICKOS_KCONFIG_PY`, so the venv's `bin/` never lands on `PATH` and shadows the interpreter the
flash tools run on. `cmake` prints the exact recipe if it cannot import it. A packaged
`kconfig-mconf` drives `menuconfig` too, though not the generator.

### Cross toolchains

A cross build finds its compiler through a per-family hint -- `KICKOS_ARM_TOOLCHAIN_BIN`,
`KICKOS_RISCV_TOOLCHAIN_BIN`, `KICKOS_RX_TOOLCHAIN_BIN`, `KICKOS_XTENSA_BIN` -- seeded from the
environment, overridable with `-D`, and falling back to `PATH` when left empty. The ARM and
RISC-V toolchain files then verify what they resolved: a compiler without newlib and
`libstdc++` for the board's own multilib is refused at configure time, naming what is missing,
rather than failing dozens of build steps later on a missing standard header.

## What CI gates

The five instruction sets are not covered equally, and the asymmetry is structural. The header
of [`.github/workflows/ci.yml`](.github/workflows/ci.yml) states each job's reasoning.

| Target | Gate | Confinement exercised |
|---|---|---|
| host `sim` | the full `ctest` suite -- the authoritative gate -- plus a UBSan build | `mprotect`, at runtime |
| armv7m / armv8-m | four QEMU MPS2 run gates (an386, an500, an385, an505), each in both postures | PMSAv7 and PMSAv8, at runtime |
| armv6m | QEMU run gate (micro:bit) | none: the nRF51 has no unit |
| rv32imac | QEMU `virt` run gate, both postures | PMP, at runtime |
| Xtensa LX6 | build gate: no upstream QEMU ESP32 machine model | none: the LX6 has no per-task unit |
| Renesas RX | none | -- |
| the remaining ARM boards | build sweep, plus the one gate that needs neither silicon nor an emulator | link surface only |

The backends QEMU carries no model for -- SYSMPU, the Cortex-M7 anti-speculation wrap, PMSAv6
-- get their link surface built in CI and their traps proven on silicon. RX has no gate at all:
the RX72M needs `-misa=v3` and `-mdfpu`, which exist only in the registration-gated Renesas
GNURX build (upstream `rx-elf` GCC rejects both), so it cannot be built on a hosted runner and
stays bench-validated. Warnings are errors in every job.

## Documentation

[`docs/README.md`](docs/README.md) is the map. Two tiers:

- **The Book** ([`docs/book/README.md`](docs/book/README.md)) -- how and why, plus a
  teach-how-a-microkernel-works text. Durable across refactors.
- **The Reference** ([`docs/reference/README.md`](docs/reference/README.md)) -- the exact
  contract, code-synced: [`architecture.md`](docs/reference/architecture.md) for the kernel
  design, [`porting.md`](docs/reference/porting.md) for the arch/chip seam,
  [`boards.md`](docs/reference/boards.md) for per-board wiring and validation status,
  [`invariants.md`](docs/reference/invariants.md),
  [`ipc-call-reply.md`](docs/reference/ipc-call-reply.md) and
  [`bus-service.md`](docs/reference/bus-service.md) for the IPC and bus contracts.

[`STATE.md`](STATE.md) is where the project stands right now; [`roadmap.md`](roadmap.md) is the
milestone-level plan.

## License

CeCILL-C V1.0 (LGPL-compatible, file-level copyleft). See [`LICENSE`](LICENSE). Every source
file carries an `SPDX-License-Identifier: CECILL-C` header. The clean-room discipline behind
the studied-never-copied rule is stated in
[`docs/reference/architecture.md`](docs/reference/architecture.md).
