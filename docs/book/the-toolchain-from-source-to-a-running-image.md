<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# The toolchain: from source to a running image

> How does the C you write become a program running on a chip?
>
> You will learn:
>
> - how source text becomes bytes in flash
> - what runs between reset and the first line of your `main`
> - how to build KickOS and an application, and run it on a board or under QEMU
> - how the source tree is organised.

*Further reading: Tanenbaum, Modern Operating Systems, ch.1 (section 1.8, "The
world according to C": header files, the preprocessor, and how a large program
is compiled and linked). For the linker in depth: Levine, Linkers and Loaders.*

## From source text to a running image

A C or C++ program becomes a running image in a fixed sequence of steps. On a
desktop computer, most of these steps stay out of sight, for two separate reasons.

- **The program is built on the machine that runs it.** This is a host build, and
  the compiler's defaults already fit: it adds the startup code and the C library
  by itself, and lays out memory the way the operating system expects. Building
  for another machine is **cross-compiling**, and then the build must be told about
  that machine: which core, which C library, where its memory sits (see
  *[What a cross toolchain is](#what-a-cross-toolchain-is)*).
- **An operating system starts the program.** Its loader places the program in
  memory and runs it.

A microcontroller program has neither help. It is always cross-compiled, and
there is no loader: the hardware starts
executing at a fixed address, and whatever the build placed there is what runs.
Code that runs this way, with no operating system underneath it, runs on **bare
metal**. A kernel is itself such a program.

**Preprocess.** The preprocessor expands `#include`, `#define` and `#if`. The
output is one long translation unit with no directives left. Configuration
reaches C mostly through this step: a generated header full of `#define`s
decides which code is compiled at all.

**Compile.** The compiler turns one translation unit into assembly for one
instruction set. It knows the target core through flags such as `-mcpu`, and it
follows the platform's **application binary interface (ABI)**, described at the
end of this section.

**Assemble.** The assembler turns assembly into an **object file**: machine code
and data grouped into named **sections**, plus a symbol table and a list of
**relocations**, the places where an address is still unknown. A few files are
written in assembly directly. [Chapter 3.5](context-switching-and-the-silicon-contract.md) explains why a context switch must be
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
layout is fine, because a loader maps the program into virtual memory. On a
microcontroller the layout must match the chip's memory map ([Chapter 0.1](the-machine-underneath.md)), so a
**linker script** states it: a `MEMORY` block naming the regions, and a
`SECTIONS` block saying which input sections go into which region, in what
order. For the f411 example the regions are 512 KiB of flash at `0x08000000`
and 128 KiB of SRAM at `0x20000000`.

`.data` is the interesting case. Its bytes must be in flash, because RAM is lost
at power-off, yet the code expects to find them at a RAM address. So the linker
script gives `.data` two addresses: a **load address** in flash and a **run
address** in RAM. The image's own code copies the bytes across at reset, before
anything reads them ([From reset to main](#from-reset-to-main)).

The linker's output is usually an **Executable and Linkable Format (ELF)** file:
the sections, their addresses, the symbol table and, with `-g`, the debug
information. A debugger reads the symbols from it, and an **emulator**, a host
program that plays the part of the board, can load it directly. On request, the
linker also writes a link **map**, which lists where every section and symbol
landed.

**Generate the binary.** Each target needs the image in a format its flash tool
supports. The GNU `objcopy` tool generates most of them from the linker's output.
Common formats:

- **raw binary** (`.bin`): only the bytes, so the flash tool must be told the start
  address
- **Intel HEX** (`.hex`): the bytes with their addresses
- **a vendor format**: some chips run their maker's own start-up code first, called
  the **boot ROM**, and it may require a format of its own

Some flash tools accept the ELF file and do this step themselves.

**Flash or load.** The image is written into the target and started:

- a **debug probe**, a USB adapter that can write flash and halt the core, writes
  it into flash. The f411 example's board has one built in: ST-Link.
- a **bootloader**, a small program already in flash, receives the image and
  writes it.
- an **emulator** loads it into its emulated memory.

A reset then starts the image.

### What an ABI is

The link step only works because every object it combines follows the same
**ABI**: the set of rules compiled code agrees on, so that separately compiled
pieces can call each other and share data. The source says a function takes two
`int`s, and the ABI says where they are when the call happens. Each platform
publishes one. For instance, Arm has the **AAPCS** (Procedure Call Standard for
the Arm Architecture), RISC-V has the **RISC-V psABI** (processor-specific ABI),
and x86_64 has the **System V** ABI.

An ABI has three layers. Several of its rules are about alignment: a value is
N-aligned when its address is a multiple of N.

- **The calling convention.** Which registers carry the arguments and the
  result, which registers a called function may overwrite (caller-saved) and
  which it must restore before returning (callee-saved), and how the stack is
  aligned at a call. On Arm, for instance, the first four argument words go in
  `r0`-`r3`, the result comes back in `r0` (`r0:r1` for 64 bits), `r4`-`r11`
  are callee-saved, and the stack is 8-byte aligned at a call.
- **Data layout.** The size and alignment of each type, and the padding the
  compiler inserts in a struct so that every member is aligned.
  `alignof(uint64_t)` is 8 on Arm but 4 on Renesas RX, so one struct can have
  two layouts on two targets.
- **The object format.** How sections, symbols and relocations are encoded. The
  platform fixes it, so that objects from different compilers can be linked
  together: ELF on Linux and most embedded targets, PE/COFF on Windows, Mach-O on
  Apple systems. The f411 example uses ELF.

The linker joins calls but never translates between conventions, so objects
that disagree must not be linked into one image. The compiler flags choose the
ABI. `-mcpu` picks the core, and so its instruction set. On Arm, `-mfloat-abi`
also picks how floating-point values travel:

- `soft` uses no floating-point unit (FPU) instructions at all.
- `softfp` uses the FPU, but still passes `float` and `double` in core
  registers.
- `hard` passes them in FPU registers.

A `hard` object and a `soft` or `softfp` object disagree about where a `float`
argument is, so the linker refuses to combine them. The f411 example is built
with `-mcpu=cortex-m4 -mfpu=fpv4-sp-d16 -mfloat-abi=softfp`, kernel and
application alike. It links newlib, its C library ([Chapter 0.6](whats-under-include-libc-and-the-cxx-runtime.md)), in the build
made with those same flags (a multilib, described under
[What a cross toolchain is](#what-a-cross-toolchain-is)). So its
`printf("hello\n")` passes the string's address in `r0`, where newlib's
separately compiled `printf` expects it.

KickOS adds its own ABI at the boundary between user code and the kernel. An
application does not call the kernel, it traps into it, so KickOS must state the
rules there itself. It states them in one header that both sides include: the
syscall numbers, which register carries the number and which carry the
arguments, and the layout of the structures both sides read. Static assertions
in that header check the size and field offsets of each shared structure at
compile time, so both sides read the same bytes.

In the f411 example, `printf` hands its bytes to `_write`, the function newlib
calls to output them ([Chapter 0.6](whats-under-include-libc-and-the-cxx-runtime.md)). `_write` ends in a small syscall stub. The
stub is an ordinary function, called under the AAPCS, so the syscall number
lands in `r0`, and the buffer and its length in `r1` and `r2`. The stub then
executes `svc #0`. The core saves `r0`-`r3`, with a few other registers, in its
**exception frame**: the registers the core itself pushes on the stack when it
takes a trap. The kernel reads the request from there.
[Chapter 3.9](the-syscall-path-trap-dispatch-return.md) explains the register convention
([*a register ABI*](the-syscall-path-trap-dispatch-return.md#the-number-and-the-arguments-a-register-abi)),
and Chapter 7.1,
[*Alignment across the syscall boundary*](alignment-across-the-syscall-boundary.md),
what goes wrong when the kernel trusts a user pointer's alignment.

## What a cross toolchain is

The machine that builds is the **host**. The machine that runs the image is the
**target**. A **cross toolchain** is a compiler, assembler, linker and binary
utilities that run on the host and produce code for the target. Each one is
named by a **target triple** such as `arm-none-eabi`: the architecture (`arm`),
the operating system (`none`, which means bare metal), and the ABI (`eabi`, the
Embedded ABI).

A toolchain is more than the compiler. It ships the C library and the C++
runtime the program links against ([Chapter 0.6](whats-under-include-libc-and-the-cxx-runtime.md)), and the low-level support
library `libgcc`. One target family covers many cores and float ABIs, so the
toolchain carries one build of each library per combination, a **multilib**.
The compiler picks the right one from the flags.

There are three common ways to obtain a toolchain:

- **A vendor toolchain** for each architecture. Easy to install, but each family
  then has its own compiler and C library release, chosen by its vendor.
- **A distribution package.** Easy as well, but its version is the
  distribution's choice and differs between machines.
- **Build it from pinned sources.** Slow the first time, but every family uses
  the same compiler, binutils and C library release, and two machines build the
  same bits.

KickOS chose the third. One pinned release of GCC (the GNU Compiler
Collection), binutils, newlib and libstdc++ (the C++ standard library) serves
every family. Each libstdc++ is compiled against the newlib it links with, so
that full C++ works on every target.

A user gets the toolchain with one script. For each family, the script restores
it from the project's release when one exists for the host, and builds it
otherwise:

```sh
tools/kickos-toolchain.sh kickos-toolchain                # every family
tools/kickos-toolchain.sh kickos-toolchain arm-none-eabi  # one family
. kickos-toolchain/kickos-toolchain.sh
```

The last line sets `KICKOS_TOOLCHAIN`, the variable the KickOS CMake toolchain
files read to find the compiler. A toolchain file looks for programs in that
folder and nowhere else, so a stray compiler on `PATH` cannot slip into a build.
If the variable is missing, configure stops. The exact contract is in
[Reference: cross toolchains](../reference/porting.md#cross-toolchains-before-you-configure-anything).

## Configuring a build: one fact, one home

Before anything is compiled, a build must decide many things: which chip, which
core flags, how large each kernel pool is (a **pool** is a fixed array of
kernel objects reserved at build time), whether memory protection is enforced,
which tasks start at boot. One could scatter these through `#ifdef`s in
headers, keep a hand-written makefile per board, or describe the hardware in a
device tree as Linux and Zephyr do. KickOS separates them by *who owns the
fact*, and gives each kind exactly one file.

| Fact | Owner | Where it is written |
|---|---|---|
| which board, which variant | the person building | a CMake **preset** |
| which architecture and chip the board uses | KickOS, once per board | the board's CMake file |
| kernel knobs: pool sizes, stack sizes, whether memory protection is enforced | the kernel's integrator | the board's Kconfig **defconfig**, one per variant |
| what the chip *is*: memory map, interrupt count, devices | KickOS, once per chip | the chip's YAML description |
| what the board *decides*: console, pins, parts fitted | KickOS, once per board | the board's YAML description |
| what *runs*: tasks, their grants ([Chapter 0.4](monolithic-vs-microkernel.md)), the heap | the application author | a **composition**, by default the board's own |

**Presets.** A preset picks a board and a variant. Each preset builds in its own
directory, named after the preset, and `cmake --list-presets` prints them all.
For instance, `f411disco` is the `base` variant of the `f411disco` board, and
`f411disco-flat` is the same board with memory protection not enforced.

**Kconfig.** Kconfig is the configuration language of the Linux kernel, and
KickOS uses it for the kernel's knobs. Each knob is declared once, with a range
and a default. A board's defconfig states only the values that differ. At
configure time, a host script resolves it into three files in the build
directory: the full configuration, called `.config`, a header that C code
includes, and a fragment that CMake reads. A value outside its declared range is
refused, not quietly replaced. After the first configure, the `.config` in the
build directory is what counts.

**The three YAML files.** YAML is a plain-text format for structured data. The
chip file and the board file describe the hardware. A host tool, the
composition tool, reads them and writes the chip headers into the build
directory. Those headers include the memory regions that the chip's linker
script uses, so the memory map is written in one place only. The tool runs
through uv, a program that runs Python tools with the exact package versions
they pin.

The **composition** says what the system runs. For a plain application, that
is one task named `main`. The composition tool checks the composition against
the chip, the board and the kernel's configuration. If a rule is broken, it
refuses the composition with a message that names the rule. Otherwise it writes
a constant table into the image. At boot, the **init**, the first unprivileged
program, walks that table to start every other task. No YAML parser runs on the
target.

The exact rules, including how a `-D` on the CMake command line becomes a
request against the Kconfig declarations, are in
[Reference: configuration from Kconfig](../reference/porting.md#configuration-from-kconfig).
Chapter 8, [*Capabilities and the object model*](capabilities.md#the-composition-initial-capabilities-declared-once),
explains what a composition grants.

## Building and flashing the f411 example

You need CMake and Ninja, Python with `kconfiglib` (the build finds it through
`KICKOS_KCONFIG_PY`), uv, and the toolchain above, sourced in the shell.
[Journey 00](../journey/00-setup.md#what-you-need) lists them with the commands
that set them up.

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
task named `main`, which runs this `main` unprivileged. Its return value ends
the system. `kickos_emit_image` adds the binary generation and the link map.
Configure the application with the package's own toolchain file, so that it is
compiled with the same compiler and flags as the kernel it links with:

```sh
cmake -S hello -B hello/build -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$HOME/kickos-f411disco/lib/cmake/KickOS/toolchain-arm-none-eabi.cmake" \
    -DCMAKE_PREFIX_PATH="$HOME/kickos-f411disco"
cmake --build hello/build
```

The application's build directory now holds the whole pipeline's output:
`hello` (the ELF file, for the debugger), `hello.bin` and `hello.hex` (for the
flash tool), and `hello.map` (the link map).

**Flashing.** The `.bin` file carries no address, so the flash tool must be told
where it goes: `0x08000000`, the start of the chip's flash, where the chip's
linker script places the image. On this board, the built-in ST-Link probe writes
it with the `st-flash` tool. The [flashing guide](../flashing.md) gives the
exact command, its reset caveats, and the project's own flash script.

In this example, the board's description routes the kernel console to USART2,
on pins `PA2` and `PA3`. A USB-serial adapter on those pins shows `hello` at
115200 baud. [Reference: boards](../reference/boards.md) has the wiring.

### Without a board: the same example under QEMU

The `qemu` preset targets the `mps2-an386` machine of QEMU, an open-source
emulator. This machine is an emulated Cortex-M4F with an MPU, like the F411, and
it is the reference machine of the Journey. A package holds exactly one board,
so the example gets a second package and a second build directory:

```sh
cmake --preset qemu && cmake --build --preset qemu
cmake --install build/qemu --prefix "$HOME/kickos-qemu"
cmake -S hello -B hello/build-qemu -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$HOME/kickos-qemu/lib/cmake/KickOS/toolchain-arm-none-eabi.cmake" \
    -DCMAKE_PREFIX_PATH="$HOME/kickos-qemu"
cmake --build hello/build-qemu
qemu-system-arm -M mps2-an386 -nographic -semihosting -kernel hello/build-qemu/hello
```

`-kernel` loads the ELF file straight into the emulated memory, so no binary
generation is involved. This machine has no UART wired to the kernel console. Instead,
`hello` reaches the terminal through **semihosting**: the program executes a
special instruction, and the emulator answers it by writing to the host's
terminal.

The applications that ship with KickOS build the same way, each into its own
image, and `ctest --preset qemu` boots each one and checks its output.
[Journey 00](../journey/00-setup.md) runs them, starts QEMU halted under a gdb
server, and attaches gdb.

## From reset to main

The f411 example is now in flash. Between a reset and the first line of its
`main`, a fixed chain of steps runs, all of it code inside the image.

1. **The reset vector.** A Cortex-M core does not start by executing an
   address. It starts by reading one. The linker script places the **vector
   table** at the start of flash, `0x08000000`, which the STM32F411 also shows
   at address 0 when it boots from flash. The table's first word is the initial
   stack pointer, the top of the 128 KiB of SRAM. Its second word, the **reset
   vector**, is the address of the reset handler, `Reset_Handler`. The core
   loads both and jumps. The rest of the table holds the addresses of the trap
   and interrupt handlers of [Chapter 0.3](interrupts-and-traps.md).
2. **The C runtime.** The reset handler first switches the floating-point unit
   on, because the compiler may use FPU instructions in any function, and they
   fault while the FPU is off. Then it copies `.data` from its load address in
   flash to its run address in RAM, and zeroes `.bss`. To do this, it walks a
   copy table and a zero table that the linker script writes. Until then, no
   code may read a global variable.
3. **Constructors.** C++ runs a **constructor** to give some globals their
   first value. The reset handler runs the kernel's list of them, which in a
   KickOS image is empty: the kernel's globals need no code to initialise, and
   the linker refuses an image where the list is not empty. The constructors of
   the application and of the C and C++ libraries go to a second list, which
   runs later and unprivileged (step 6). [Chapter 0.6](whats-under-include-libc-and-the-cxx-runtime.md) explains why the order
   matters.
4. **The chip and the kernel.** The chip's setup code brings the chip up: first
   the clock to its final speed, then TIM2 counting as the time base, then
   USART2 programmed for the console. The order matters, because the baud rate
   depends on the final clock. Then the kernel's own `main` runs, privileged, on
   the boot stack. It prints its banner, sets up its pools and builds the first
   two threads: **idle**, the thread that runs when nothing else is ready, and
   **root**, the first unprivileged thread.
5. **The first thread.** A new thread has never run, so it has no saved context
   to switch back to. The kernel writes on root's stack the frame that a context
   switch ([Chapter 0.2](what-is-an-os-and-a-kernel.md)) would have saved, and the scheduler starts by
   "switching back" to root. That first switch also turns the memory protection
   on, so root runs unprivileged and confined from its very first instruction.
6. **The init and `main`.** Root first runs the constructors set aside in step 3.
   Then it becomes the init: it walks the table that the composition tool built
   from the board's default composition, creates the `main` task and its thread
   through syscalls, and blocks. The new thread enters a small runtime function,
   which calls the application's `main` and passes its return value to `exit`,
   and `exit` ends the system. The build renames your `main` with a `-D`
   compiler flag, so that this runtime function can call it, while your source
   still says `main`.

[Journey 01, Power-on](../journey/01-power-on.md) walks each step in a debugger
on the `qemu` preset, with the code, and says where `f411disco` differs.

## The source tree

The tree is organised by role. Its main top-level directories hold:

- the privileged kernel
- the hardware layer below it, split into what a CPU family shares, what one
  instruction set needs and what one chip adds, so that each part is written
  once
- the boards, one directory per board with its build configurations
- the chip and board descriptions in YAML, which the configure step turns into
  headers
- the runtime that every application links
- the unprivileged system services
- the tests, at every level
- the tools that the build and the flashing use

The [Journey's map of the tree](../journey/00-setup.md#a-map-of-the-tree) names
each directory and says which Journey chapter visits it.

## Where to go next

- The hardware the linker script describes: Chapter 0.1,
  [*The machine underneath*](the-machine-underneath.md), and its memory map.
- Why the f411 example's `printf` cannot write the USART2 register itself and
  must ask the kernel: Chapter 0.2,
  [*What is an OS, and what is a kernel?*](what-is-an-os-and-a-kernel.md).
- Who owns that UART: Chapter 0.4,
  [*Monolithic vs microkernel*](monolithic-vs-microkernel.md).
- What the C library and the C++ runtime bring into the link, and why KickOS
  builds them with its own toolchain: Chapter 0.6,
  [*What's under #include*](whats-under-include-libc-and-the-cxx-runtime.md).
- How an enforcing linker script splits kernel and application memory, and how
  to read one: Chapter 7.3,
  [*Where your RAM goes*](where-your-ram-goes-full-cxx-memory-floor-and-the-linker-split.md).
- Hands-on: [Journey 00, Setup](../journey/00-setup.md), then
  [Journey 01, Power-on](../journey/01-power-on.md), which starts at the vector
  table the linker script placed.
- The exact build contract: [Reference: build system](../reference/architecture.md#build-system),
  [Reference: adding a board/chip](../reference/porting.md#adding-a-boardchip-the-seven-edit-points)
  and [Reference: configuration from Kconfig](../reference/porting.md#configuration-from-kconfig).
- Flashing a real board: the [flashing guide](../flashing.md).
