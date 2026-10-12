<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# How to read this book, and the toolchain

> This Part 0 chapter says who the Book is for, how it fits with the two other
> KickOS documents, and in what order a newcomer can read them. It then follows
> a program from source text to a running image: the general pipeline, what a
> cross toolchain is, and how KickOS configures, builds and runs an image. It
> ends with a map of the source tree.

*Further reading: Tanenbaum, Modern Operating Systems, ch.1 (section 1.8, "The
world according to C": header files, the preprocessor, and how a large program
is compiled and linked). For the linker in depth: Levine, Linkers and Loaders.*

## Who the Book is for

The Book assumes a reader who can read C and a little C++, and who has compiled
a program and seen it run. It does not assume an operating-systems course. The
first chapters of Part 0 build the hardware picture (0.1, 0.2) and the idea of a
kernel (0.3, 0.4); this chapter supplies the build picture; 0.6 explains what
sits under `#include`. Parts 1 and later then take KickOS apart one design
decision at a time.

Part 0 follows one program throughout, **the f411 example**: an application
whose `main` calls `printf("hello\n")`, running on the `f411disco` board (an
STM32F411, Cortex-M4F). Its text leaves through the USART2 data register.
Chapter 0.1 locates that register in the memory map, 0.2 the interrupts and the
syscall trap on the way, 0.3 why `printf` needs the kernel at all, 0.4 who owns
the UART, and 0.6 what `printf` pulls in from the C library. This chapter builds
the example and puts it on the board.

Each chapter follows the same spine: the problem, the ways one could solve it,
what KickOS chose and why, and where that choice lives in the code. A reader who
only wants the rule can skip to the last step and follow its link.

## Three documents, three questions

KickOS keeps three bodies of documentation. Each answers one question, and each
links to the others instead of repeating them.

| Document | Answers | Where |
|---|---|---|
| **The Book** | *Why* is it built this way? The concepts, the design space, the choice. | [`./`](README.md) |
| **The Journey** | *Where, and in what order*, does it happen in the code? A walk through the source, stop by stop, with gdb recipes. | [`../journey/`](../journey/README.md) |
| **The Reference** | *Exactly what* is the contract? Interfaces, invariants, porting rules, boards. | [`../reference/`](../reference/README.md) |

The Reference is kept in step with the code, and when the two disagree, the code
wins. The Book can stay timeless because it leaves the exact numbers and rules to
the Reference. The Journey sits between them: it names a file and a function,
says what happens there, and links up to the Book for the reason and across to
the Reference for the rule.

## A reading path for a newcomer

1. **Part 0 in order:** [the machine](the-machine-underneath.md) (0.1),
   [interrupts and traps](interrupts-and-traps.md) (0.2),
   [what a kernel is](what-is-an-os-and-a-kernel.md) (0.3),
   [monolithic versus microkernel](monolithic-vs-microkernel.md) (0.4), this
   chapter (0.5), and
   [the C library and C++ runtime](whats-under-include-libc-and-the-cxx-runtime.md)
   (0.6).
2. **Build and run once.** Follow [Journey 00, Setup](../journey/00-setup.md) to
   build the `qemu` preset and attach gdb. Seeing the banner and the first
   breakpoint makes everything after it concrete.
3. **Walk power-on.** [Journey 01](../journey/01-power-on.md) goes from reset to
   the first unprivileged instruction of `main`.
4. **Alternate Journey and Book.** Read a Journey chapter, then the Book chapter
   that explains the design it walked:

| Journey chapter | Book chapter beside it |
|---|---|
| [02, the first syscall](../journey/02-first-syscall.md) | 3.9, [The syscall path](the-syscall-path-trap-dispatch-return.md) |
| [03, sleep](../journey/03-sleep.md) | 2.1, [The time base](the-time-base-a-monotonic-clock-you-can-trust.md); 2.2, [The blocking substrate](the-blocking-substrate-one-wait-wake-primitive.md) |
| [04, a mutex](../journey/04-mutex.md) | 2.3, [Priority inheritance](priority-inheritance-lending-urgency.md) |
| [05, call and reply](../journey/05-call-and-reply.md) | 8.3, [Endpoints](endpoints-synchronous-ipc-by-rendezvous.md); 8.5, [Call and reply](synchronous-call-and-reply.md) |
| [06, shared memory](../journey/06-shared-memory.md) | 7.5, [Protection follows the CPU](protection-follows-the-cpu-not-the-schedulers-intent.md) |
| [07, an interrupt to a driver](../journey/07-interrupt-to-a-driver.md) | 3.8, [A masked interrupt is latched](a-masked-interrupt-is-latched-not-lost.md) |
| [08, spawn and capabilities](../journey/08-spawn-and-caps.md) | 8.0, [Authority](authority-from-possession-or-from-identity.md); 8.1, [Handles](handles-and-the-resolve-chokepoint.md) |
| [09, fault and kill](../journey/09-fault-and-kill.md) | 7.4, [Privilege is three axes](privilege-is-three-axes-not-one-bit.md) |

5. **Branch out.** The rest of the Book can be read in any order; each chapter
   names what it builds on. A porter reads Chapter 6 and
   [`../reference/porting.md`](../reference/porting.md) next.

## From source text to a running image

A C or C++ program becomes a running image in a fixed sequence of steps. A
desktop hides most of them behind one compiler command and a program loader. A
microcontroller hides almost none, because no loader exists: the hardware starts
executing at a fixed address, and whatever the build placed there is what runs.

**Preprocess.** The preprocessor expands `#include`, `#define` and `#if`. The
output is one long translation unit with no directives left. Configuration
reaches C mostly through this step: a generated header full of `#define`s
decides which code is compiled at all.

**Compile.** The compiler turns one translation unit into assembly for one
instruction set. It knows the target core through flags such as `-mcpu`, and it
follows the platform's **ABI**, described at the end of this section.

**Assemble.** The assembler turns assembly into an **object file**: machine code
and data grouped into named **sections**, plus a symbol table and a list of
**relocations**, the places where an address is still unknown. A few files are
written in assembly directly; Chapter 3.5 explains why a context switch must be
one of them.

The common sections:

| Section | Holds | Lives in |
|---|---|---|
| `.text` | machine code | flash |
| `.rodata` | constants, string literals | flash |
| `.data` | initialised globals | RAM, with its initial bytes stored in flash |
| `.bss` | zero-initialised globals | RAM, no bytes stored |

**Link.** The linker combines every object and library into one program. It
resolves each symbol reference to one definition, decides the final address of
every section, and patches the relocations. On a desktop the linker's default
layout is fine because a loader maps the program into virtual memory. On an MCU
the layout must match the chip's memory map (Chapter 0.1), so a **linker
script** states it: a `MEMORY` block naming the regions and a `SECTIONS` block
saying which input sections go into which region, in what order. For the f411
example the regions are 512 KiB of flash at `0x08000000` and 128 KiB of SRAM at
`0x20000000`.

`.data` is the interesting case. Its bytes must be in flash, because RAM is lost
at power-off, yet the code expects to find them at a RAM address. The linker
script gives it two addresses: a **load address** (LMA) in flash and a **run
address** (VMA) in RAM. Before `main` runs, startup code copies `.data` from the
first to the second and zeroes `.bss`. That copy is the C runtime's first job on
any bare-metal system.

**The ELF file.** The linker writes an **ELF** file: the sections, their
addresses, the symbol table and, with `-g`, the debug information. A debugger
reads the symbols from it; an emulator can load it directly. A link **map**,
written beside it on request, lists where every section and symbol landed.

**objcopy.** Flash programmers often want raw bytes rather than ELF. `objcopy -O
binary` writes the image as a plain `.bin` with no addresses, so the tool must
be told where it starts. `objcopy -O ihex` writes Intel HEX, which carries the
addresses inside. Some boot ROMs want their own format again, built from the ELF
by a vendor tool.

**Flash or load.** Finally the image reaches the target: written into flash by
a debug probe or a bootloader, then started by a reset; or loaded into an
emulator, which plays the part of the board.

### What an ABI is

The link step only works because every object it combines follows the same
**ABI** (application binary interface): the set of rules compiled code agrees
on so that separately compiled pieces can call each other and share data. The
source says a function takes two `int`s; the ABI says where they are when the
call happens. Each platform publishes one: **AAPCS** (the Procedure
Call Standard for the Arm Architecture) on Arm, the **RISC-V psABI** on RISC-V,
the **System V** ABI on x86_64. It has three layers.

- **The calling convention.** Which registers carry the arguments and the
  result, which registers a called function may overwrite (caller-saved) and
  which it must restore before returning (callee-saved), and how the stack is
  aligned at a call. On Arm the first four argument words go in `r0`-`r3`, the
  result comes back in `r0` (`r0:r1` for 64 bits), `r4`-`r11` are
  callee-saved, and the stack is 8-byte aligned at a call.
- **Data layout.** The size and alignment of each type, and the padding the
  compiler inserts in a struct so that every member is aligned.
  `alignof(uint64_t)` is 8 on Arm but 4 on RX, so one struct can have two
  layouts on two targets.
- **The object format.** How sections, symbols and relocations are encoded,
  ELF for the f411 example.

The linker joins calls but never translates between conventions, so objects
that disagree must not be linked into one image. The compiler flags choose the
ABI: `-mcpu` picks the instruction set and, on Arm, `-mfloat-abi` picks how
floating-point values travel. `soft` uses no FPU instructions; `softfp` uses the
FPU but still passes `float` and `double` in core registers; `hard` passes them
in FPU registers. A `hard` object and a `soft` or `softfp` object disagree about
where a `float` argument is, and the linker refuses to combine them. The f411
example is built with `-mcpu=cortex-m4 -mfpu=fpv4-sp-d16 -mfloat-abi=softfp`
(`arch/arm/chip/stm32f411/cpu.cmake`), kernel and application alike, and links
the newlib multilib built with the same flags. Its `printf("hello\n")` passes
the string's address in `r0`, where newlib's separately compiled `printf`
expects it.

KickOS adds its own ABI across the user/kernel boundary. An application traps
into the kernel rather than calling it, so KickOS states the rules there, in
`user/include/kickos/sys/abi.h`: the syscall numbers (`enum kos_syscall_nr`),
which register carries the number and which carry the arguments, and the structures both sides read, such as `struct kos_recv_info`
and `struct kos_thread_params`. In the f411 example, `_write` ends in
`kos_kconsole_write`, which calls
`arch_syscall(KOS_SYS_KCONSOLE_WRITE, buf, len, 0, 0)`. That is an ordinary
AAPCS call, so the number lands in `r0` and the buffer and length in `r1` and
`r2`. The stub moves the fifth word from the stack into `r12` and executes
`svc #0`; the core saves `r0`-`r3` and `r12` in its exception frame, and the
kernel reads the request from there. `abi.h` pins the shared structures' sizes
and offsets with static assertions, so both sides read the same bytes.
Chapter 3.9 explains the register convention
([*a register ABI*](the-syscall-path-trap-dispatch-return.md#the-number-and-the-arguments-a-register-abi)),
and Chapter 7.1,
[*Alignment across the syscall boundary*](alignment-across-the-syscall-boundary.md),
what goes wrong when the kernel trusts a user pointer's alignment.

## What a cross toolchain is

The machine that builds is the **host**; the machine that runs the image is the
**target**. A **cross toolchain** is a compiler, assembler, linker and binary
utilities that run on the host and produce code for the target. Each one is
named by a **target triple** such as `arm-none-eabi`: the architecture, the
vendor (none), and the ABI or system. `none` in the system position means
bare metal: no operating system underneath.

A toolchain is more than the compiler. It ships the C library and the C++
runtime the program links against (Chapter 0.6), and the low-level support
library `libgcc`. One target family covers many cores and float ABIs, so the
toolchain carries one build of each library per combination, a **multilib**,
and the compiler picks the right one from the flags.

There are three common ways to obtain one:

- **A vendor toolchain** for each architecture. Easy to install, but each family
  then has its own GCC and C library release, chosen by its vendor.
- **A distribution package.** Easy as well, but its version is the distribution's
  choice and differs between machines.
- **Build it from pinned sources.** Slow the first time, but every family uses
  the same compiler, binutils and C library release, and two machines build the
  same bits.

KickOS chose the third. One pinned release of GCC, binutils, newlib and
libstdc++ serves every family, and each libstdc++ is compiled against the newlib
it links with, which is what makes full C++ work on every target. Its families,
multilibs and reasons live in
[`../design-m10-toolchain.md`](../design-m10-toolchain.md).

A user gets it with one script, which restores each family from the project's
release when one exists for the host and builds it otherwise:

```sh
tools/kickos-toolchain.sh kickos-toolchain            # every family
tools/kickos-toolchain.sh kickos-toolchain arm-none-eabi  # one family
. kickos-toolchain/kickos-toolchain.sh
```

Sourcing the second file sets `KICKOS_TOOLCHAIN`, the one variable the CMake
toolchain files read (`cmake/toolchain-arm-none-eabi.cmake` and its siblings,
through `kickos_toolchain_package` in `cmake/toolchain-common.cmake`). A
toolchain file looks for programs in that folder and nowhere else, so a stray
compiler on `PATH` cannot slip into a build, and configure stops when the
variable is missing. The exact contract is
[Reference: cross toolchains](../reference/porting.md#cross-toolchains-before-you-configure-anything).

## Configuring a build: one fact, one home

Before anything is compiled, a build must decide many things: which chip, which
core flags, how large each kernel pool is, whether memory protection is
enforced, which tasks start at boot. One could scatter these through
`#ifdef`s in headers, keep a hand-written makefile per board, or describe the
hardware in a device tree as Linux and Zephyr do. KickOS separates them by
*who owns the fact*, and gives each kind exactly one file.

| Fact | Owner | File |
|---|---|---|
| which board, which variant | the person building | a CMake **preset** |
| which arch and chip the board uses | KickOS, once per board | `boards/<board>/board.cmake` |
| kernel knobs: pool sizes, stacks, protection posture | the kernel's integrator | `boards/<board>/configs/<variant>/defconfig` (Kconfig) |
| what the part *is*: memory map, interrupt count, devices | KickOS, once per chip | `platform/<chip>/chip.yaml` |
| what the board *decides*: console, pins, parts fitted | KickOS, once per board | `platform/<chip>/<board>.yaml` |
| what *runs*: tasks, grants, heap | the application author | a composition, `boards/<board>/composition.yaml` by default |

**Presets.** A preset names a board and a variant and nothing else. It lives in
`cmake/presets/*.json` (one file per CPU family, gathered by
`CMakePresets.json`), and each preset builds in its own `build/<preset>/`
directory. `f411disco` is the `base` variant of the `f411disco` board;
`f411disco-flat` is the same board with enforcement turned off.

**Kconfig.** The kernel's knobs are declared once in `Kconfig`, `arch/Kconfig`
and `boards/Kconfig`, each with a range and a default; this is the mechanism
Linux uses. A board's defconfig states only what differs. At configure time
`tools/kconfig/genconfig.py` resolves it into `build/<preset>/generated/`: a
`.config`, a `board_config.h` that C code includes, and a CMake fragment. A
value outside its declared range is refused, not quietly replaced. After the
first configure the live `.config` in the build directory is what counts.

**The three YAML files.** The chip file and the board file are the hardware
description. A host tool, `tools/compose` (run through uv), reads them and
writes the chip headers into the build tree, including the `MEMORY` regions the
chip's linker script includes, so the memory map is stated once. The
**composition** says what the system runs: for a plain application, one task
named `main`. The same tool checks it against the chip, the board and the
kernel's configuration, refuses it with a message naming the broken rule, or
emits a constant table that the init walks at boot. No YAML parser runs on the
target.

The rules behind all of this, including how a `-D` on the CMake line becomes a
request against the Kconfig declarations, are in
[Reference: configuration from Kconfig](../reference/porting.md#configuration-from-kconfig)
and [`../design-m10-composition.md`](../design-m10-composition.md).

## Building and flashing the f411 example

The prerequisites are listed in the project [`README.md`](../../README.md)
("Building"): CMake and Ninja, Python with `kconfiglib` (named through
`KICKOS_KCONFIG_PY`), uv, and the toolchain above, sourced in the shell.

The build has two halves. KickOS is built once for a board and installed as a
CMake **package**: its libraries, headers, the chip's linker script, and the
toolchain file that configured it. The application is then an ordinary CMake
project that links against that package.

```sh
cmake --preset f411disco                   # configure: Kconfig, chip headers, composition
cmake --build --preset f411disco           # build the kernel, runtime and in-tree images
cmake --install build/f411disco --prefix "$HOME/kickos-f411disco"
```

The f411 example is two files. `main.c`:

```c
#include <stdio.h>

int main(int argc, char** argv)
{
    (void)argc;
    (void)argv;
    printf("hello\n");
    return 0;
}
```

and `CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.25)
project(hello LANGUAGES C CXX ASM)
find_package(KickOS REQUIRED)
add_executable(hello main.c)
target_link_libraries(hello PRIVATE KickOS::kernel KickOS::system_default)
kickos_emit_image(hello)
```

`KickOS::kernel` brings the kernel, the user runtime, the link flags and the
linker script. `KickOS::system_default` is the board's default composition: one
task named `main`, which runs this `main` unprivileged; its return value ends the
system. `kickos_emit_image` adds the `objcopy` step. Configure it with the
package's own toolchain file, so the application is compiled with the same
compiler and flags as the kernel it links with:

```sh
cmake -S hello -B hello/build -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$HOME/kickos-f411disco/lib/cmake/KickOS/toolchain-arm-none-eabi.cmake" \
    -DCMAKE_PREFIX_PATH="$HOME/kickos-f411disco"
cmake --build hello/build
```

`hello/build/` now holds the whole pipeline's output: `hello` (the ELF, for the
debugger), `hello.bin` and `hello.hex` (for the flash tool), and `hello.map`.

**Flashing.** The `.bin` carries no address, so the flash tool is told where it
goes: `0x08000000`, the base of `main_flash` in `platform/stm32f411/chip.yaml`.
The chip's linker script, `arch/arm/chip/stm32f411/stm32f411.ld`, puts the
vector table first in that region, and the core reads it at reset. The board's
on-board ST-Link writes it with `st-flash`; [`../flashing.md`](../flashing.md)
gives the exact command and its reset caveats, and `tools/flash.sh f411disco
<app>` flashes the in-tree images of `build/f411disco/`. The board file
`platform/stm32f411/f411disco.yaml` routes the console to USART2 on `PA2`/`PA3`;
a USB-serial adapter on those pins shows `hello` at 115200 baud
([`../reference/boards.md`](../reference/boards.md) has the wiring).

### Without a board: the same example under QEMU

The `qemu` preset targets QEMU's `mps2-an386` machine, a Cortex-M4F with an MPU
like the F411, emulated on the host; it is the reference machine of the Journey.
A package holds exactly one board, so the example gets a second package and a
second build directory:

```sh
cmake --preset qemu && cmake --build --preset qemu
cmake --install build/qemu --prefix "$HOME/kickos-qemu"
cmake -S hello -B hello/build-qemu -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$HOME/kickos-qemu/lib/cmake/KickOS/toolchain-arm-none-eabi.cmake" \
    -DCMAKE_PREFIX_PATH="$HOME/kickos-qemu"
cmake --build hello/build-qemu
qemu-system-arm -M mps2-an386 -nographic -semihosting -kernel hello/build-qemu/hello
```

`-kernel` loads the ELF straight into the emulated memory, so no `objcopy` step
is involved. This machine has no UART wired to the kernel console; `hello`
reaches the terminal through semihosting, a trap the emulator answers on the
host's behalf.

The in-tree images build the same way: every application under
`user/apps/common/` becomes its own image under
`build/qemu/user/apps/common/<app>/`, and `ctest --preset qemu` boots each one
and judges its output. [Journey 00](../journey/00-setup.md) runs them, starts
QEMU halted under a gdb server, and attaches gdb.

## The source tree

The tree follows the layering the Book teaches: a small kernel with no hardware
access of its own, a hardware seam below it, and unprivileged code above it.
The seam is split three ways so that each layer is written once: what a CPU
**family** shares, what one **instruction set** (ISA) needs, and what one
**chip** adds.

| Path | Holds |
|---|---|
| `kernel/` | the privileged kernel: scheduler, threads, tasks, synchronisation, time, interrupts, memory domains, capabilities, syscalls |
| `arch/include/kickos/arch/arch.h` | the porting interface: every hardware operation the kernel may call |
| `arch/<family>/<isa>/` | one ISA's assembly and core rules: context switch, syscall trap, critical section (`arch/arm/armv7m/`, `arch/riscv/rv32imac/`, ...) |
| `arch/<family>/chip/<chip>/` | one chip: vector table, `Reset_Handler`, linker script `<chip>.ld`, clocks, console, time base (`arch/arm/chip/mps2/`, ...) |
| `arch/<family>/common/`, `arch/common/` | code a family, or several families, share; `arch/sim/` is the host backend |
| `platform/<chip>/` | the chip file and its board files (YAML) |
| `boards/<board>/` | `board.cmake`, the variants' defconfigs, the default composition |
| `user/` | what every application links: the `kos_*` API headers, syscall stubs, the newlib glue, and the applications under `user/apps/` |
| `system/` | the unprivileged system layer: the composed init, user-space drivers per chip, the C++ terminate handler |
| `lib/`, `include/` | a small freestanding C library for the kernel; headers shared by every layer |
| `cmake/` | presets, toolchain files, image helpers |
| `tests/` | host unit tests, per-image integration scripts, static checks over sources and linked images |
| `tools/` | the composition tool, Kconfig resolution, toolchain provisioning, flashing scripts |
| `examples/` | applications built outside the tree against an installed KickOS |

[Journey 00](../journey/00-setup.md#a-map-of-the-tree) walks each directory and
says which Journey chapter visits it;
[Reference: repo layout](../reference/architecture.md#repo-layout) is the full
map.

## Where to go next

- The hardware the linker script describes: Chapter 0.1,
  [*The machine underneath*](the-machine-underneath.md), and its memory map.
- Why the f411 example's `printf` cannot write the USART2 register itself and
  must ask the kernel: Chapter 0.3,
  [*What is an OS, and what is a kernel?*](what-is-an-os-and-a-kernel.md); and who owns
  that UART: Chapter 0.4, [*Monolithic vs microkernel*](monolithic-vs-microkernel.md).
- What the C library and the C++ runtime bring into the link, and why KickOS
  builds them with its own toolchain: Chapter 0.6,
  [*What's under #include*](whats-under-include-libc-and-the-cxx-runtime.md).
- How an enforcing linker script splits kernel and application memory, and how
  to read one: Chapter 7.3,
  [*Where your RAM goes*](where-your-ram-goes-full-cxx-memory-floor-and-the-linker-split.md).
- Hands-on: [Journey 00, Setup](../journey/00-setup.md), then
  [Journey 01, Power-on](../journey/01-power-on.md), which starts at the vector
  table the linker script placed.
- The exact build contract: [Reference: build system](../reference/architecture.md#build-system)
  and [`../reference/porting.md`](../reference/porting.md) ("Adding a board/chip",
  "Configuration from Kconfig").
- Silicon: [`../flashing.md`](../flashing.md).
