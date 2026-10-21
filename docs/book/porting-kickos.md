<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# Porting KickOS: a new board, a new chip, a new ISA

> Part 4 toured the CPU families and Part 5 the chips. This chapter turns the tour around:
> what it takes to add one. It explains the three depths of a port, what each one
> touches and in what order, the seam that keeps the work bounded, where each kind of
> fact is written down, and how a port is shown to work. The exact contract, every seam
> member and every edit point, lives in [`../reference/porting.md`](../reference/porting.md);
> this chapter does not repeat it.
>
> *Further reading: Tanenbaum, Modern Operating Systems, ch.12 (operating system design,
> and its advice on hiding the hardware behind a machine-dependent layer) and ch.11
> (the hardware abstraction layer of Windows, one industrial form of that layer).*

## Why porting is a design question

A kernel is written once and runs on many machines. Each machine differs from the
others in some ways: how it saves a register file, how it enters a trap, which
timer it has, where its UART sits, which pins a board maker wired to it. A kernel
that ignores this runs on one machine. A kernel that handles every difference
wherever it appears becomes a pile of special cases, and no one can say what it
does on a target nobody tested.

So every portable kernel answers two questions. Where is the line between the part
that changes per machine and the part that does not? And how is a new machine
described to the build? The answers decide how much work a port is, and whether a
port that builds actually behaves like every other port.

KickOS has a sharp answer to the first question, because of its uniform-fleet
thesis (Chapter 1, *[Overview and principles](overview-and-principles.md)*): the same
semantics hold on every target, and a divergence is a bug. That only works if the
part that changes per target is small and has a clear contract.

## Three depths of port

"Porting" covers three jobs of very different size. Name the depth first, because
it decides what you touch.

| Depth | You have | You add | Typical size |
|---|---|---|---|
| **Board** | a supported chip | a description of how this board wires the chip | a few short files, no kernel or chip code |
| **Chip** | a supported ISA | a chip file, a chip backend, then a first board | a reference manual's worth of facts, one chip source, a linker script |
| **ISA** | nothing for this CPU family | a toolchain, an arch backend, then a chip and a board | the context switch, the trap entry, and their contract |

The depths nest. A new ISA needs a first chip, and a new chip needs a first board,
because only a board is something you can build and flash. Each depth reuses
everything above it unchanged: the scheduler, IPC, capabilities and the syscall
table are the same C++ whether the target is a Cortex-M4F or a RISC-V core.

### A new board on a supported chip

Take the f411 example. The `f411disco` board is an STM32F411 with its console on
USART2 (pins PA2 and PA3), its diagnostic LED on PD12 and an 8 MHz crystal. Now
imagine a second board on the same chip: a board of your own, call it `f411lab`
(it is hypothetical; no such board is in the tree). Its console is on the same
USART2 pins, its LED is on PB0, and its crystal runs at 16 MHz.

None of that is a fact about the STM32F411. It is a fact about how one PCB wires
it. So the port touches only board-level files, in this order:

1. **Name the board.** A `boards/Kconfig` stanza that selects the chip
   (`CHIP_STM32F411`), and a `boards/<board>/board.cmake` that names the arch and the
   chip for the build.
2. **Describe the wiring.** A board file, `platform/stm32f411/<board>.yaml`: the
   console device and its pins, the LED and its active level, any parts on a bus.
   The configure checks every pin against the chip file. PB0 is not yet in
   `platform/stm32f411/chip.yaml`, so the board file is refused until the chip file
   gains that pin's row. That row describes the chip, not the board, so it is the
   only change outside the board's own files.
3. **State what the file has no field for.** The crystal goes in the board's
   `board_wiring.h` (for the blackpill, `boards/blackpill/include/kickos/board_wiring.h`). The chip code derives its PLL input divider
   from that one number, so a whole-megahertz crystal needs no chip change.
4. **Configure the kernel.** One `defconfig` per variant under
   `boards/<board>/configs/`, stating only what differs from the declared defaults.
5. **Compose the default system.** `boards/<board>/composition.yaml`: the task that
   runs `main`, its priority and the heap.
6. **Add a preset**, so the board builds by name.

The f411 example's `printf("hello\n")` then reaches USART2 on the new board with
no change in the application or the kernel. The tree already holds this case
once: `blackpill` is a second board on `stm32f411`, and its board file and
defconfig differ from `f411disco`'s only in wiring facts and the board's name.

The depth has an edge, and the chip code states it. The STM32F411 backend drives
USART2 as its console, and it checks at compile time that the board's console is
that device. A board whose console is on USART1 is no longer only a board port:
the chip backend has to learn a second USART first. When a board fact needs new
code, that work is chip work.

### A new chip on a supported ISA

A chip port describes a part the kernel has never seen, on a CPU family it already
knows. The work moves to the reference manual. The order follows what you need to
see before anything else works:

1. **The chip file**, `platform/<chip>/chip.yaml`: the memory map, the device
   windows and their interrupt lines, the pins and their mux functions, the
   protection unit. For the f411 that is where USART2's window at `0x40004400`, its
   line 38, and TIM2 at `0x40000000` on line 28 are written, each with the manual
   page it came from.
2. **The capability declaration**, a `CHIP_*` stanza in `arch/Kconfig` that selects
   the arch and, if the part has one, `HAS_MPU`.
3. **The CPU baseline**, `arch/<family>/chip/<chip>/cpu.cmake`: the core and its FPU.
4. **The chip backend**, `arch/<family>/chip/<chip>/`: reset and the vector table,
   the linker script, the clock tree, the console, the time base, then the optional
   seams (pin mux, peripheral enable, the diagnostic LED) and the protection unit.
5. **A first board**, as above.

Most of a chip is a description, and the code is a short list of things only code
can say: register layouts, clock and init sequences, the mux. On the f411 the
time base is TIM2, a 32-bit counter the chip extends to 64 bits in software;
Chapter 2.1 explains why such a clock needs care. When two parts of one vendor
family share most of their bodies, a family directory such as
`arch/arm/chip/stm32f1f3/` holds the shared unit, and each chip keeps what really
differs.

### A new ISA

An ISA port is the deep one, and it is where the seam (next section) matters most.
The order:

1. **A toolchain**: a cross compiler and a CMake toolchain file for the family.
2. **An arch declaration** in `arch/Kconfig`, with the capabilities the arch has.
3. **The arch backend**, `arch/<family>/<arch>/`: the saved context, the context
   switch, the trap entry and the syscall path, the critical section, the panic
   stack. Chapter 3.5 states what every backend owes here. A backend also owes user
   code a compare-and-swap, and a promise that one in flight fails across any return to
   user mode. A core with no such instruction uses a restartable sequence that the
   switch rewinds ([Chapter 2.4](an-atomic-buys-definedness-not-atomicity.md)).
4. **An emulated chip first.** A QEMU machine with a documented timer and a
   semihosting console lets you prove the switch and the trap before any real
   peripheral is in the way.
5. **Then a silicon chip**, layered on an arch that already works.

*[Porting an RTOS to a new ISA](porting-a-new-isa-riscv.md)* walks this whole
depth on RISC-V, from the trap vector to the first silicon chip.

## The seam: a few functions with fixed meanings

**How one could** draw the line between portable and machine-specific code:

- **Conditional code everywhere.** `#ifdef` blocks inside the scheduler and the
  drivers. It is quick for a second target and unreadable by the fifth, and no one
  can list what a new target owes.
- **A hardware abstraction layer.** A layer under the kernel that hides the
  machine, as Windows' HAL does. It works, but a broad layer can grow into a second
  operating system.
- **A narrow arch interface with defined semantics.** A short list of functions
  every target implements, each with a meaning stated once. Linux's `arch/`
  tree, NuttX's `up_*` functions and seL4's architecture split each take this
  approach in their own way.

**What KickOS chose** is the third, as small as it can be.
`arch/include/kickos/arch/arch.h` lists every operation the kernel may ask of the
hardware: `arch_switch`, `arch_irq_save` and `arch_irq_restore`, `arch_clock_now`
and `arch_timer_arm`, `arch_mpu_apply`, and the rest. Each one names a *concept*,
not a mechanism. `arch_switch` means "make this thread run next". It does not say
"pend PendSV" or "write the CLINT". The kernel in `kernel/` names no ISA, no
register and no descriptor format. Above the seam, everything is shared. Below it,
each target answers the same short list of questions.

The seam has two halves, which match the depths above. The **arch backend** is what
one ISA needs on any chip: the switch, the trap, the critical section. The **chip
backend** is what one part needs: reset, clocks, console, timer, linker script.
`armv7m` is one arch backend under `stm32f411`, `xmc4800`, `mk64f` and other chips;
each of those chips is one chip backend under its boards.

Three properties keep the seam honest:

- **A mandatory member has no fallback.** A chip that forgets `arch_clock_now` fails
  to link, rather than running with a clock that does not tick.
- **An optional member has an explicit fallback.** A chip with no diagnostic LED
  simply does not define `arch_diag_led_set`, and a separate `_default.cc` translation
  unit in `arch/common/` answers instead. These are not weak symbols, and the
  Reference explains the one placement rule this imposes on a backend's definition.
- **The seam grows for a capability, never for a processor.** A new ISA must fit the
  existing signatures. A member is added only when a new class of hardware arrives
  that no existing member can express, such as address translation.

## Where a fact belongs

The second question is how a new machine is described to the build. Every port is
mostly *facts*: a memory map, an interrupt number, a pin, a pool size, a crystal.
The rule is one fact, one home, and each kind of fact has a home chosen for the
checks it gets there.

**How one could:** put board facts in `#ifdef` blocks in the source; keep them as
build-system variables; or describe the hardware in a separate data file that the
build reads, as Linux and Zephyr do with a device tree beside Kconfig.

**What KickOS chose** is the last approach, with three homes:

- **Kconfig, for what configures the kernel and the arch.** Pool sizes, stack sizes,
  the memory-protection posture, and the arch and chip capability selection.
  Kconfig gives each knob a type, a range and its dependencies, and the configure
  *refuses* a value the declarations do not permit, instead of quietly substituting
  a default. A chip stanza in `arch/Kconfig` is pure `select`: it says what the
  silicon has, and sets no knob.
- **The yaml files, for everything else.** The **chip file** describes the part, and
  configure generates the kernel's chip headers, tables and link values from it, so
  the vector table and the kernel's interrupt table are sized from one line count,
  and a device window is written once for both the kernel and the composition. The **board file** describes the wiring. The **composition** describes
  userspace: tasks, their authority, the heap, where `stdout` goes. Each is checked
  against the others at configure: a board pin the chip does not list, or a task
  granted a device the kernel owns, is refused before anything compiles.
- **CMake, last.** Only what the build itself needs before anything else can be
  read: which arch and chip a board is, the compiler's CPU flags, the toolchain, the
  preset. A value set only in CMake is invisible to Kconfig's checks and to the
  generated `.config` that records a build's configuration, so a fact goes there only
  when it can go nowhere else.

What none of the three can hold stays in code: register layouts, clock sequences,
the mux, and the few wiring facts the board file has no field for, such as the
f411's crystal. The test for a new fact is simple. If it describes the part, it goes
in the chip file. If it describes the PCB, it goes in the board file. If it shapes
the kernel build, it goes in Kconfig. If it describes the application's tasks, it
goes in the composition.

## How a port is proven

A port that builds is not a port that works. Under the uniform-fleet thesis, "works"
has a precise meaning: the target shows the same behaviour as every other target.
So the proof is the same test suite, not a test written for the new board.

- **The selftest** (`user/apps/common/selftest/`) is that suite: many test cases
  covering scheduling, IPC, capabilities, faults and memory protection. It runs as
  an ordinary task of its own composition and reports in TAP, the same cases on
  every target. A case a target cannot run is a skip declared by name for that
  target, and the gate fails on any skip it was not told to expect.
- **Proving the negative.** A passing suite shows what the target allows. It does not
  show that the target refuses anything. An enforcing port also runs `mpu_fault`, in
  which an unprivileged thread writes another domain's memory and the hardware must
  deny it. A protection unit that is configured but never faults passes the suite and
  fails this check.
- **The emulator gates.** The host `sim` runs the kernel and its userspace as one
  Linux process. The QEMU machines run the real arch backends. Both run in CI on
  every change, so an emulated chip is the cheapest place to prove a new arch, and
  the place that keeps proving it.
- **The static gates** under `tests/static/` read the tree and the linked images on
  every board. Some of them guard the porting rules themselves. For example,
  `check_seam_defaults.sh` refuses a seam definition placed where its fallback would
  silently win, and `check_chip_kconfig.sh` holds the chip file's protection unit and
  the Kconfig `select` to the same answer.
- **The silicon gates.** A real chip is proven on a real board: the selftest, the
  denial, and the board's own apps, captured from its console and judged by script.
  No emulator models every peripheral, and some families have no emulator at all.
  For such a target, a change to the arch seam is unproven until it runs on silicon.

The order of proof follows the order of the port: `hello` first, because a banner
proves reset, the C runtime, the first thread and the console; then the selftest
without protection; then with protection; then the denial. Which targets carry which
proof is a status, and status lives in the Reference, not here.

## The reading map

- **[Porting an RTOS to a new ISA: a RISC-V worked
  example](porting-a-new-isa-riscv.md)**: what a new CPU forces you to decide, from
  the trap vector and the deferred switch to the emulator-first bring-up.
- **Chapter 5, *[What a chip adds to its core](what-a-chip-adds.md)***: what a chip contributes
  beyond its ISA (clocks, console, timer, memory map and quirks), so you know what a
  chip port has to write.
- **Chapter 4, *[One kernel, many ISAs](one-kernel-many-isas.md)***: how each CPU
  family meets the same contract, so you can see your new ISA's answers next to the
  existing ones.
- Related deep dives a porter will need: Chapter 3.5, *[Context switching and the
  silicon contract](context-switching-and-the-silicon-contract.md)*, for what an arch
  backend owes; Chapter 2.1, *[The time base](the-time-base-a-monotonic-clock-you-can-trust.md)*,
  for vetting a chip's clock; Chapter 3.7, *[Peripheral isolation and the hardware
  ceiling](peripheral-isolation-and-the-hardware-ceiling.md)*, for what a chip's
  protection unit can and cannot isolate.

## Where to go next

- The code a port fills in, stop by stop: [Journey 01, Power-on](../journey/01-power-on.md)
  names the f411 chip file next to each QEMU stop, and Journey chapters
  [12](../journey/12-armv6m.md) to [17](../journey/17-xtensa.md) show what changes at
  each stop on every other CPU family, with [14](../journey/14-riscv.md) as the
  companion to the worked ISA port.
- Building a preset and running an image: [Journey 00, Setup](../journey/00-setup.md).
- The exact contract: [`../reference/porting.md`](../reference/porting.md) holds the seam
  members, the board and chip edit points, the Kconfig rules and the porter's
  checklist. [`../reference/boards.md`](../reference/boards.md) holds each board's
  facts, its caveats and what is proven on it.
