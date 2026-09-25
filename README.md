<!--
SPDX-License-Identifier: CECILL-C
Copyright (c) 2026 Philippe Leduc
-->

# KickOS

KickOS is a capability-based microkernel RTOS written in C and C++. The kernel manages
threads, memory domains, IPC, capabilities and interrupts. Device and bus drivers run as
userspace servers and communicate through IPC. Console handover is supported on boards
with a userspace console driver; the kernel retains startup and panic output.

The kernel and hardware backends are written without vendor HALs, SDKs, CMSIS packs or
third-party kernel code. Register definitions come from hardware manuals.

## Features

- **Unprivileged applications.** The root application starts without kernel privilege.
  Capabilities and delegated authority control access to kernel services. Hardware privilege
  and memory isolation depend on the target; software checks cannot replace a missing MPU.
- **Memory isolation.** MPU and PMP targets use protected memory regions. ARM64 and RV64
  use per-process page tables, with ASIDs where supported. Linux sim uses `mprotect` over
  its user-memory arena. Targets without enforcement still check capability permissions.
- **Typed capabilities.** Tasks hold their own handle tables. Delegation can reduce rights;
  reference counts control object lifetime.
- **Event-driven scheduling.** Blocking, yielding, IPC and interrupts trigger scheduling.
  The clock is tickless by default; a periodic tick is also available.
- **Configurable resource pools.** Thread, task and memory-domain limits are independent.
  Boards set their budgets through Kconfig.
- **Host testing.** Linux sim runs the kernel and userspace in one process. It uses the same
  scheduler, syscall dispatcher and capability code, with emulated privilege transitions.
- **Portable backends.** Architecture and chip code implement the interface described in
  the [porting guide](docs/reference/porting.md).
- **CMake integration.** Applications can use `find_package(KickOS)`, link `kickos` and
  provide an ordinary `main`. See the [host](examples/oot-app/) and
  [MCU](examples/oot-mcu-app/) examples.

## Supported targets

The tree defines 24 targets: 14 MCU boards, eight QEMU targets, the i.MX8MP EVK target and
Linux sim. Build support, emulator tests and hardware validation are separate. Check the
[board reference](docs/reference/boards.md) for validation status, memory requirements,
console pins and wiring.

| Preset | Part or machine | Memory isolation |
|---|---|---|
| **Cortex-M3/M4/M7/M33 (`armv7m` backend)** | | |
| `frdmk64f` | MK64FN1M0 / Cortex-M4F | SYSMPU |
| `xmc4800-relax` | XMC4800 / Cortex-M4F | PMSAv7 |
| `f411disco` | STM32F411 / Cortex-M4F | PMSAv7 |
| `blackpill` | STM32F411 / Cortex-M4F | PMSAv7 |
| `teensy41` | i.MX RT1062 / Cortex-M7 | PMSAv7 |
| `pizero2350` | RP2350 / Cortex-M33 | PMSAv8 |
| `f302nucleo` | STM32F302R8 / Cortex-M4 | None |
| `bluepill-c8` | STM32F103C8 / Cortex-M3 | None |
| `due` | AT91SAM3X8E / Cortex-M3 | None |
| `qemu`, `qemu-m7`, `qemu-m3` | QEMU MPS2 an386 / an500 / an385 | PMSAv7 |
| `qemu-m33` | QEMU MPS2 an505 / Cortex-M33 | PMSAv8 |
| **Cortex-M0/M0+ (`armv6m` backend)** | | |
| `picopi` | RP2040 / Cortex-M0+ | PMSAv6 |
| `microbit` | nRF51822 / Cortex-M0 | None |
| **ARMv8-A** | | |
| `qemu-arm64` | QEMU `virt` / Cortex-A53 | MMU, VMSAv8 page tables |
| `imx8mp-evk` | NXP i.MX 8M Plus / Cortex-A53 | MMU, VMSAv8 page tables |
| **RV32IMAC** | | |
| `esp32c6-wroom` | ESP32-C6 | PMP (NAPOT) |
| `qemu-riscv` | QEMU `virt` | PMP (NAPOT) |
| **RV64IMAC** | | |
| `qemu-riscv64` | QEMU `virt` | MMU, Sv39 or Sv48 page tables |
| **RXv3** | | |
| `rx72m` | Renesas RX72M | RX-MPU |
| **Xtensa LX6** | | |
| `esp32-wroom` | ESP32 | None |
| **x86-64** | | |
| `qemu-x86_64` | QEMU `q35` / UEFI | None; shared firmware map |
| **Host** | | |
| `sim` | Linux process | `mprotect` over the user arena |

The `imx8mp-evk` target has local QEMU tests but no CI job or hardware validation. The
`microbit` QEMU tests use 32 KiB of RAM; they do not represent a physical micro:bit v1.

For a new MCU target, the board reference gives a 64 KiB flash / 16 KiB RAM minimum and a
128 KiB / 32 KiB recommended configuration. Actual requirements depend on thread count,
stacks, capability tables and enabled features. Small targets split the self-test across
multiple images.

## Building

Requirements:

- CMake 3.25 or newer for the supplied version-6 presets, plus Ninja.
- A host C/C++ compiler for sim, or the target's toolchain.
- Python 3 with `kconfiglib` for configuration.
- GoogleTest with a CMake package for the host unit tests.
- QEMU for emulator tests; x86-64 also needs OVMF firmware and `mtools`.

Keep the Kconfig interpreter separate from flashing tools:

```sh
python3 -m venv "$HOME/.venvs/kickos-kconfig"
"$HOME/.venvs/kickos-kconfig/bin/pip" install kconfiglib==14.1.0
export KICKOS_KCONFIG_PY="$HOME/.venvs/kickos-kconfig/bin/python"
```

Build sim and run its tests:

```sh
cmake --preset sim -DKICKOS_BUILD_UNIT_TESTS=ON
cmake --build --preset sim
ctest --preset sim --output-on-failure
```

Set `CMAKE_PREFIX_PATH` if GoogleTest is installed outside the compiler's search paths.
The [CI setup](.github/actions/gtest/action.yml) shows how to install it with Conan.
Without GoogleTest, sim can build with `KICKOS_BUILD_UNIT_TESTS=OFF`, but the host unit
suite is omitted.

Build and test an emulator target:

```sh
cmake --preset qemu
cmake --build --preset qemu
ctest --preset qemu --output-on-failure
```

For a physical board, select its preset, build, then use the
[flashing guide](docs/flashing.md):

```sh
cmake --preset picopi
cmake --build --preset picopi
```

### Presets

Presets select a board and configuration variant. Each uses a separate directory under
`build/`. Run `cmake --list-presets` for the full list, defined in
[`cmake/presets/`](cmake/presets/).

```sh
cmake --preset frdmk64f        # base configuration
cmake --preset frdmk64f-st     # self-test configuration
cmake --preset frdmk64f-flat   # MPU enforcement disabled
```

Available variants depend on the board. They include self-tests, benchmarks, telemetry,
shared-kernel multicore builds, isolated cores and AMP partitions with separate kernels.
MPU boards generally provide a `-flat` variant. Sim always uses `mprotect`; MMU targets
select address-space support through their chip configuration.

### Configuration

Kconfig controls kernel settings. A preset loads its initial values from
`boards/<board>/configs/<variant>/defconfig`. Later builds use the saved
`build/<preset>/generated/.config`.

```sh
ninja -C build/frdmk64f menuconfig     # edit the live configuration
ninja -C build/frdmk64f                # apply changes and rebuild
ninja -C build/frdmk64f savedefconfig  # save changes to the source defconfig
ninja -C build/frdmk64f defconfig      # discard local settings and reload defconfig
```

Editing a source defconfig does not update an existing build until it is reloaded. Generated
configuration files stay in the build directory. Add a defconfig and preset for a reusable
variant. Memory enforcement requires a working chip backend and the corresponding linker
layout.

### Toolchains

Cross toolchains accept these directory hints from the environment or CMake `-D` options,
and otherwise search `PATH`:

| Target family | Toolchain directory variable |
|---|---|
| Cortex-M | `KICKOS_ARM_TOOLCHAIN_BIN` |
| ARM64 | `KICKOS_AARCH64_TOOLCHAIN_BIN` |
| RISC-V | `KICKOS_RISCV_TOOLCHAIN_BIN` |
| RX | `KICKOS_RX_TOOLCHAIN_BIN` |
| Xtensa | `KICKOS_XTENSA_BIN` |

The x86-64 UEFI build uses host GCC and GNU binutils with the `i386pep` linker emulation.
See [CI toolchain setup](.github/actions/) for the versions and packages used in CI.

An ARM64 or RV64IMAC image also needs the dynamic-reent newlib, provisioned once per multilib
through Conan:

```sh
conan export conan/newlib
conan install conan/board -o "&:multilib=aarch64" --build=missing --output-folder=/tmp/kn-aarch64
source /tmp/kn-aarch64/kickos-newlib-aarch64.sh
```

Use `multilib=rv64imac_lp64` for RV64IMAC. The generated script exports `KICKOS_NEWLIB_AARCH64` or
`KICKOS_NEWLIB_RV64IMAC_LP64`, which the corresponding cross toolchain file reads; see the
[board reference](docs/reference/boards.md) for the full variable table.

## CI coverage

[The CI workflow](.github/workflows/ci.yml) defines the exact builds and test selections.
Warnings are errors. Coverage includes:

| Target | Checks |
|---|---|
| Linux sim | Source checks, kernel and unit tests, plus a UBSan build |
| Cortex-M3/M4/M7/M33 | Four QEMU MPS2 machines, with and without MPU enforcement |
| Cortex-M0 | QEMU `microbit`, without memory enforcement |
| ARM64 | QEMU `virt`: single core, four cores, core isolation, GICv3 and AMP; single-core and multicore benchmark checks |
| RV32IMAC | QEMU `virt` with and without PMP; ESP32-C6 builds |
| RV64IMAC | QEMU `virt`: Sv39, Sv48 and four harts; single-core and multicore benchmark checks |
| x86-64 | QEMU `q35` with UEFI, including benchmark checks |
| Xtensa LX6 | ESP32 builds and host checks, including SMP and benchmark configurations |
| Other Cortex-M boards | Build and static checks, including MPU and RP2350 AMP configurations |
| RXv3 | No CI job; validation uses the RX72M hardware |

ARM64 AMP runtime tests cover shared-image and two-image configurations. Three-image AMP
partitions are built and checked for link-layout agreement; CI does not boot them. RP2350
AMP checks also run without booting hardware.

Local emulator tests may report a skip when tools are missing; CTest can still exit
successfully. CI checks the required tools, and the x86-64 job also rejects skipped tests.
Emulator results do not replace hardware validation. See the
[board reference](docs/reference/boards.md) for recorded hardware results and limitations.

## Documentation

Start with the [documentation index](docs/README.md).

- [The Book](docs/book/README.md): concepts and design explanations.
- [The Reference](docs/reference/README.md): interfaces, invariants, porting, boards and drivers.
- [Project state](STATE.md): implementation status and current limitations.
- [Roadmap](roadmap.md): planned milestones.

## License

CeCILL-C V1.0. See [LICENSE](LICENSE). Source files carry
`SPDX-License-Identifier: CECILL-C` headers. The
[architecture reference](docs/reference/architecture.md) describes the clean-room policy.

A cross-built image also embeds newlib (BSD-family notices; the dynamic-reent package above ships
its own copy as `licenses/COPYING.NEWLIB`, and every other cross toolchain bundles its own) and the
GCC runtime libraries it links against (GPL-3 with the GCC Runtime Library Exception). Whoever
distributes such an image carries those notices with it. This states facts about what an image
embeds; it is not legal advice.
