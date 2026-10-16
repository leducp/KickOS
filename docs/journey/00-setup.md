<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# 0. Setup: build, run, debug, and a map of the tree

This chapter gets you ready to follow the code. You will build the `qemu` images, run one in
QEMU, stop it at its first instruction with gdb, and find your way around the source tree.
For an explanation of compiling, linking and loading, see Part 0 of the Book
([Book: the toolchain](../book/the-toolchain-from-source-to-a-running-image.md),
[Book: the C library and the C++ runtime](../book/whats-under-include-libc-and-the-cxx-runtime.md),
[Book: the machine underneath](../book/the-machine-underneath.md)).

## What you need

The project [`README.md`](../../README.md) ("Building") is the authority for the list. For the
`qemu` walk you need:

- **CMake** 3.25 or newer, and **Ninja**.
- **Python 3 with `kconfiglib`**, for the configuration step. The README keeps it in its own
  virtual environment and points the build at it with `KICKOS_KCONFIG_PY`.
- **uv**. The configure step runs the composition tool (`tools/compose`) through it. The
  `qemu` board has a default composition (`boards/qemu/composition.yaml`), so it needs uv.
- **The KickOS toolchain.** Every board builds with KickOS's own GCC, binutils and newlib,
  built from pinned sources. `tools/kickos-toolchain.sh <folder> [family,...]` restores it
  from a release or builds it. The reference walk needs only the `arm-none-eabi` family;
  chapters 10 to 17 name the presets for the other architectures. Then source
  `<folder>/kickos-toolchain.sh` to set `KICKOS_TOOLCHAIN`. Configuration stops if that
  variable is missing.
- **QEMU** with `qemu-system-arm`.
- **A gdb that understands Arm.** On a Debian-like host that is `gdb-multiarch`. The KickOS
  toolchain ships no gdb.

```sh
python3 -m venv "$HOME/.venvs/kickos-kconfig"
"$HOME/.venvs/kickos-kconfig/bin/pip" install kconfiglib==14.1.0
export KICKOS_KCONFIG_PY="$HOME/.venvs/kickos-kconfig/bin/python"

tools/kickos-toolchain.sh kickos-toolchain arm-none-eabi
. kickos-toolchain/kickos-toolchain.sh
```

## Build the `qemu` preset

A **preset** names one board and one configuration variant. The presets live in
`cmake/presets/*.json`, one file per CPU family; `cmake --list-presets` prints them all. The
`qemu` preset is in `cmake/presets/arm.json`: it sets `KICKOS_BOARD=qemu` and uses
`cmake/toolchain-arm-none-eabi.cmake`.

```sh
cmake --preset qemu
cmake --build --preset qemu
```

The build directory is `build/qemu`. The configure step does three things worth knowing:

1. **Kconfig.** It reads `boards/qemu/configs/base/defconfig` and resolves it against the
   `Kconfig` tree into `build/qemu/generated/.config` and
   `build/qemu/generated/include/kickos/board_config.h`. Every `KICKOS_*` constant you meet
   in the code (stack sizes, pool sizes, `KICKOS_MEMORY_ENFORCED`) comes from there. Once a
   build exists, the live `.config` is what counts; `ninja -C build/qemu menuconfig` edits
   it.
2. **The chip description.** It reads `platform/mps2/chip.yaml` and `platform/mps2/qemu.yaml`
   and writes the chip headers under `build/qemu/generated/chip/` (memory layout, interrupt
   count, the tables the linker script includes).
3. **The composition.** It reads `boards/qemu/composition.yaml`, which says what tasks the
   system starts (for a plain app, one task called `main`), and emits a table the init
   walks at boot. You will meet that table at the end of [01](01-power-on.md).

Every application under `user/apps/common/` becomes an **image**: one ELF file that holds the
kernel, the runtime and that one application. The `hello` image is
`build/qemu/user/apps/common/hello/hello`.

## Run an image

The test suite runs every image and judges its output:

```sh
ctest --preset qemu --output-on-failure
```

To see exactly how one image is launched, ask CTest to print the command instead of running
it:

```sh
ctest --test-dir build/qemu -N -V -R '^qemu_hello$'
```

It prints a call to `tests/integration/check_qemu_hello.sh` with `QEMU_MACHINE=mps2-an386`.
That script starts the emulator with the same arguments you can type yourself:

```sh
qemu-system-arm -M mps2-an386 -nographic -semihosting \
    -kernel build/qemu/user/apps/common/hello/hello
```

`-kernel` loads the ELF into the emulated flash and RAM. `-semihosting` lets the program ask
the host for services through a special trap: on this machine the kernel console, the clock
and the final exit code all go through it. `-nographic` puts the console on your terminal.
Press `Ctrl-A` then `X` to quit QEMU.

What `hello` prints (the build lines are trimmed):

```
  ==============================================
   KickOS 0.5.1  -  microkernel RTOS
  ==============================================
   board   qemu
   arch    armv7m
   cpu     cortex-m4
   mpu     enforce
   sched   tickless
   ...
   heap    56 KiB available
   kstack  1008 B x 17 = 17136 B

hello from KickOS userspace!
two threads play ping-pong; press Ctrl+C to stop.

  ping 1
  pong 1
  ping 2
  pong 2
```

The banner comes from the kernel (`kbanner` in `kernel/init/kmain.cc`). The lines after it
come from the application, running unprivileged. `hello` never ends; the two threads keep
playing.

The `selftest` image is the kernel's own test suite. It prints TAP lines and then ends the
emulator with its exit status:

```sh
qemu-system-arm -M mps2-an386 -nographic -semihosting \
    -kernel build/qemu/user/apps/common/selftest/selftest
```

```
1..126
# tap route: kernel debug console (stdout not published)
...
ok 1 - svc_roundtrip
ok 2 - fifo_order
ok 3 - preempt_on_ready
...
# all tests passed (0 skipped, 0 vacuous, 2 partial)
```

## Run an image under gdb

Add two options to the QEMU command. `-gdb tcp::1234` opens a gdb server on port 1234. `-S`
holds the core before its first instruction until gdb says `continue`.

```sh
qemu-system-arm -M mps2-an386 -nographic -semihosting \
    -kernel build/qemu/user/apps/common/hello/hello -gdb tcp::1234 -S
```

In a second terminal, load the same ELF into gdb (it reads the symbols from it) and attach:

```sh
gdb-multiarch build/qemu/user/apps/common/hello/hello -ex 'target remote :1234'
```

gdb shows where the core is held:

```
kickos_armv7m_enable_fpu () at .../arch/arm/common/regs.h:130
130	    kickos::arm::reg32(kickos::arm::SCB_CPACR) |= kickos::arm::CPACR_CP10_CP11_FULL;
```

The core has not run an instruction, but gdb already shows it inside `Reset_Handler` (the
FPU enable is its first line, inlined) with a stack pointer. A Cortex-M core loads both from the first two words of its vector table when it
leaves reset. [01, Stop 1](01-power-on.md) starts there.

To rerun a recipe without typing it, put the commands in a file and run gdb in batch mode:

```sh
cat > walk.gdb <<'EOF'
break kickos::kmain
continue
bt
EOF
gdb-multiarch build/qemu/user/apps/common/hello/hello -batch \
    -ex 'target remote :1234' -x walk.gdb
```

Restart QEMU for each run: an image runs once from reset. Kill the emulator when you are done.

## A map of the tree

The tree follows the layering the Book teaches: a small kernel with no hardware access of its
own, a hardware seam below it, and unprivileged code above it. Two directories run privileged,
`kernel/` and `arch/`. Everything else, `user/`, `system/` and the applications, runs
unprivileged and reaches the kernel only through the syscall door.

Only the directories the journey visits are described at length. The
[Reference: repo layout](../reference/architecture.md#repo-layout) is the full map.

### `kernel/` - the privileged kernel

The kernel itself, in freestanding C++ (C++ without the standard library's runtime). It
touches no hardware register directly: everything that touches a register goes through the
arch seam. Its subdirectories group by the four jobs a kernel owns
([Book: what a kernel must own](../book/what-is-an-os-and-a-kernel.md#what-a-kernel-must-own)),
and the syscall door sits beside them.

| Directory | Job | What it holds | Journey |
|---|---|---|---|
| `kernel/sched/` | Scheduling | The scheduler (`sched.cc`): ready queues, picking the next thread, the switch booking; and the FIFO/round-robin policy | 01, 03, 04 |
| `kernel/thread/` | Scheduling | Creating a thread (`thread_create`), parking and unparking it, its thread-local storage and its C library state | 01, 08 |
| `kernel/time/` | Scheduling | The time base, the sleep queue, the timer re-arm; CPU clock selection | 03 |
| `kernel/domain/` | Memory protection | Memory domains: the set of memory regions a task may touch | 01, 06 |
| `kernel/mem/` | Memory protection | Who owns which arena block, and, on MMU targets, frames, address spaces and user stacks | 06, 10 |
| `kernel/task/` | Memory protection | Tasks: the group of threads that share one memory domain | 08 |
| `kernel/grant/` | Memory protection | Device windows and the kernel-reserved blocks a grant may not cover | 06, 07 |
| `kernel/sync/` | IPC and synchronization | Semaphores, mutexes with priority inheritance, the wait queue; the kernel lock on multicore | 03, 04, 11 |
| `kernel/irq/` | Interrupts | Interrupts delivered as events to user space, and which core takes a line | 07 |
| `kernel/notify/` | Interrupts | Notification objects (a word of pending bits) | 07 |
| `kernel/syscall/` | The door; IPC | `syscall_dispatch` (`syscall.cc`), which routes each syscall number; the capability table (`cap.cc`), each thread's list of the kernel objects it may use, which every object-naming syscall checks; one file per syscall family, among them the message endpoints (`syscall_ipc.cc`) | 02, 05, 08 |
| `kernel/init/` | Boot | `kmain` (`kmain.cc`: the boot sequence and the banner), the kernel console and its TX ring, the fault and panic reporter, the diagnostic LED | 01, 09 |
| `kernel/klib/` | - | `kprintf` formatting and string helpers for the kernel | - |
| `kernel/ktrace/`, `kernel/bench/` | - | Telemetry records and benchmark counters | - |
| `kernel/amp/` | - | Asymmetric multiprocessing: several kernels sharing one chip | 11 |
| `kernel/include/kickos/` | - | The kernel's own headers: `thread.h`, `sched.h`, `cap.h`, `instance.h` (the `Kernel` object) and the rest | all |

### `arch/` - the hardware seam

`arch/include/kickos/arch/arch.h` is the porting interface: every function the kernel may call
on the hardware (`arch_init`, `arch_context_init`, `arch_switch`, `arch_timer_arm`,
`arch_mpu_apply`, ...). Below it, code is split in three layers, so that each is written once:

- **Family** (`arch/arm/`, `arch/riscv/`, `arch/arm64/`, `arch/rx/`, `arch/xtensa/`,
  `arch/x86/`, plus `arch/sim/` for the host): what all chips of one family share. For
  Cortex-M that is `arch/arm/common/` (the vector block, the NVIC, SysTick, the PMSA MPU
  writer).
- **ISA** (`arch/arm/armv7m/`, `arch/arm/armv6m/`, `arch/riscv/rv32imac/`, ...): the context
  switch and the syscall trap in assembly (`switch.S`), the critical section, the first
  frame of a thread.
- **Chip** (`arch/arm/chip/mps2/`, `arch/arm/chip/stm32f411/`, ...): the vector table
  (`startup.S`), the linker script (`<chip>.ld`), `Reset_Handler`, clocks, the console and
  the time base (`chip_<chip>.cc`).

`arch/common/` holds pieces several families share: the copy and zero loop of the C runtime
(`startup_ranges.cc`), the boot tail (`crt_tail.h`), the bump allocator over RAM
(`arch_ram_common.cc`), the deferred MPU stash, the semihosting console. Visited in 01, 02
and every architecture chapter.

### `user/` - the runtime every application links

| Directory | What it holds |
|---|---|
| `user/include/kickos/` | The application API: `sys.h` (the C `kos_*` calls), `kos.h` (the C++ `kos::` wrappers), `app.h` (the `main` contract) |
| `user/src/` | The syscall stubs (`syscall_stubs.cc`), the user side of the door: each `kos_*` call packs its arguments and calls `arch_syscall`, which executes the trap; the root thread's entry (`root_entry.cc`), the task entry (`task_trampoline.cc`), and the glue newlib needs: `_sbrk`, the libc lock, the per-thread `_reent`, the other stubs |
| `user/lib/` | User-side libraries, such as the SPI proxy |
| `user/apps/common/` | Applications built for every board: `hello`, `hello_c`, `selftest`, `stress`, the fault probes, and many small witnesses |
| `user/apps/<board>/` | Demos for one board |

### `system/` - the system layer

The unprivileged layer between the kernel and the application. `system/init/compose/` is the
**init**: the code the first user thread runs to read the composition table and start each
task. `system/driver/<chip>/` holds the user-space drivers, `system/cxx/` the C++ terminate
and exception report. Visited at the end of 01 and in 07 and 08.

### Other directories

- **`lib/`** - a small freestanding C library for the kernel (`string.cc`, `fmt.cc`), the
  console TX ring header, the SEGGER RTT backend.
- **`include/kickos/`** - headers shared by every layer (units, link-symbol helpers,
  telemetry records).
- **`boards/<board>/`** - one directory per board: `board.cmake` (which arch and chip), the
  `configs/<variant>/defconfig` files, and the default `composition.yaml`. `boards/Kconfig`
  lists the boards.
- **`platform/<chip>/`** - the chip and board descriptions in YAML (memory map, interrupt
  count, devices, console) that the configure step turns into headers.
- **`Kconfig`, `arch/Kconfig`, `boards/Kconfig`** - the configuration symbol tree. The
  defconfigs set values; `tools/kconfig/genconfig.py` resolves them.
- **`cmake/`** - presets, toolchain files, and the helpers that build images
  (`kickos.cmake`).
- **`tests/`** - `unit/` (host unit tests of kernel pieces), `integration/` (one script per
  image that boots it and judges its output), `static/` (checks over the sources and the
  linked images), `tap/` (the TAP harness `selftest` uses), `drivers/`, `composition/`.
- **`tools/`** - the composition tool (`compose/`), Kconfig resolution (`kconfig/`), the
  toolchain provisioning script, flashing scripts for real boards, the telemetry decoder
  (`kicktrace.py`), benchmark and AMP helpers.
- **`examples/`** - applications built outside the tree against an installed KickOS.
- **`docs/`** - the Book, the Reference, design records, and this journey.

## Where to next

- [1. Power-on](01-power-on.md): from reset to the first instruction of `main`.
- [Reference: build system](../reference/architecture.md#build-system) and
  [Reference: configuration from Kconfig](../reference/porting.md#configuration-from-kconfig).
- [`docs/flashing.md`](../flashing.md), when you move from QEMU to the `f411disco` board.
