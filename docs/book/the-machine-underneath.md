<!-- SPDX-License-Identifier: CECILL-C -->
# The machine underneath: CPU, memory, and peripherals

> Before any chapter can talk about saving a thread's state, protecting a
> region, or handing a driver its device, there has to be a shared picture of
> what the hardware *is*: a core that holds a little state and executes, a single
> flat range of addresses, and a set of memories and devices wired into that
> range. This is a front-of-book primer. It defines the vocabulary -- register,
> address space, peripheral, memory-mapped I/O, bus, memory type, memory map --
> that later chapters use at depth without pausing to introduce. It assumes only
> minimal C/C++ and the compile/link/flash basics; it assumes no prior hardware
> background. Everything here is framed for the microcontroller (MCU), because
> that is the machine KickOS runs on.

## What a CPU core is: a small pile of state that executes

Strip a processor core down to what software can observe and it is astonishingly
small: a handful of fast on-core storage slots called **registers**, plus the
logic that reads an instruction, does what it says, and moves to the next one.
Everything a running program *is*, at the instant you freeze it, lives in those
registers. The important ones, by role:

- **Program counter (PC).** The address of the instruction being executed (or the
  next one). The core fetches from PC, executes, advances PC. A branch or a call
  is just a write to PC.
- **Stack pointer (SP).** The address of the top of the current stack -- the
  region in memory where function calls push their return addresses, saved
  registers, and local variables. Every `{ }` scope in your C leans on it.
- **General-purpose registers (GPRs).** A dozen-ish scratch slots the compiler
  uses for the values a computation is working on right now: loop counters,
  pointers, intermediate arithmetic. Function arguments and return values are
  passed in some of these by convention (the ABI).
- **Status / flags register.** A few bits the last operation set -- was the result
  zero, did it carry, did it overflow -- which the next conditional branch reads.
  This is how `if (a > b)` becomes machine code.
- **Floating-point (FP) registers.** On cores with a hardware FP unit, a *separate*
  bank of slots holding floating-point values. Integer-only cores do not have
  these; there the compiler emulates floating point in software.
- **A privilege / mode bit.** One bit (or a small field) that says whether the
  core is running in a trusted **privileged** mode or a restricted
  **unprivileged** one. Privileged code may touch control registers and any
  memory; unprivileged code is fenced in. This single bit is the seed of the
  entire kernel-vs-userspace split.

That whole collection -- PC, SP, GPRs, flags, FP, mode -- is **the machine
state**, sometimes called the *context*. A CPU does not know what a "thread" is;
it only ever holds one machine state at a time and executes it. The trick an
operating system plays is to save that pile of state to memory, load a different
pile, and resume -- so that many threads appear to run on one core. Saving and
restoring exactly this state, on the very core whose state it is rewriting, is
what Chapter 3.5 (*Context switching and the silicon contract*) is about; this
section is the definition it builds on.

*Further reading: Tanenbaum, Modern Operating Systems, ch.1 (the processor, its
registers, and what a program's state is).*

## MCU vs application processor: one flat address space, usually no MMU

"Computer" spans a wide range, and the split that matters for an RTOS is between
an **application processor** and a **microcontroller**.

An application processor is the chip in a phone or laptop. It runs a big OS
(Linux, Windows), boots off external DRAM and flash, and -- decisively -- has a
**memory management unit (MMU)**: hardware that translates the *virtual*
addresses a program uses into the *physical* addresses on the bus. The MMU gives
each process its own private illusion of the whole address space, and lets the OS
page memory to and from disk. It is powerful and expensive, in transistors, in
boot complexity, and in the RAM its page tables consume.

A microcontroller is a different animal. It is a *single chip* that already
contains its core, its program memory (flash), its working memory (SRAM), and its
devices (timers, serial ports, GPIO). It typically has:

- **On-chip memory.** Flash and SRAM measured in kilobytes to a few megabytes,
  built into the same die -- not external DRAM sticks.
- **A single, flat, physical address space.** Every memory and every device lives
  at a fixed numeric address. There is no per-process remapping; address `0x2000_0000`
  means the same physical SRAM byte to every piece of code.
- **No MMU** (commonly). What it may have instead is a **memory protection unit
  (MPU)**: a much smaller unit that does *not* translate addresses but *can* mark
  a handful of address ranges as readable/writable/executable or off-limits to
  unprivileged code. Protection without translation.

KickOS targets the MCU end deliberately. There is one physical address space that
the kernel and every thread share, and isolation is drawn by an MPU where one
exists -- not by an MMU giving each thread a private virtual space. That is the
root of the project's "MPU-first, MMU-later" stance: the near-term isolation
story is built on the protection unit that MCUs actually have, and address
translation is a later, additive concern. Chapter 7 (*Memory protection*) is the
full treatment; what you need here is the shape -- **flat, shared, physical,
protected-in-regions rather than translated.**

*Further reading: Tanenbaum, Modern Operating Systems, ch.1 (kinds of systems)
and ch.3 (memory management -- what an MMU does, which is exactly what an MCU
does without).*

## The parts of a microcontroller: a block diagram in prose

Open the block diagram in any MCU datasheet and you will see the same cast, wired
the same way. Named once, so later chapters can name them without redrawing:

- **The core** -- the CPU described above, the only thing that *initiates*
  memory traffic in a simple single-master MCU: every access starts as a load, a
  store, or an instruction fetch from the core.
- **The interconnect (the bus fabric)** -- the wiring that carries an access from
  the core to whatever lives at the target address, and carries the response
  back. It is not a single wire but a small network with a fast backbone and
  slower branches (next two sections).
- **The memories** -- flash (where your program lives) and SRAM (where your data
  and stacks live), each occupying a fixed address range.
- **The peripherals** -- the on-chip devices: timers, UART/SPI/I2C serial
  controllers, GPIO pins, ADCs, DMA engines, the interrupt controller. Each also
  occupies a fixed address range.

The mental picture is a hub. The core sits on one side of the interconnect;
memories and peripherals hang off the other side, each answering to its own slice
of addresses. When the core issues an access to some address, the interconnect
routes it to the one memory or device that owns that address, that responder
acts, and the interconnect returns the result (or an error, or -- as Chapter 7.6
warns -- nothing at all if no responder is wired there). Two things follow from
this hub: devices are reached *by address* exactly like memory (next section), and
*where a guard sits on the hub* decides what it can police (the section after).

## Peripherals and memory-mapped I/O: a device register is an address

How does a program tell a timer to start, or read a byte that arrived on a serial
port? On an MCU the answer is uniform and simple: **you read and write addresses.**

Each peripheral exposes a bank of **registers** -- small fixed-width control and
data words -- and each register is assigned a fixed address in the same flat space
as memory. Writing the timer's "control" register at its address starts the
timer; reading the UART's "data" register at its address hands you the received
byte. There is no special "I/O instruction"; an ordinary load or store to the
right address *is* the I/O. This scheme is **memory-mapped I/O (MMIO)**, and the
address range where the peripherals live is the **peripheral aperture** (often up
around `0x4000_0000` on ARM MCUs). In C this looks like a write through a pointer
to a `volatile` struct laid over the register block.

Here is the twist that separates a device register from a memory cell, and it
matters enough that a whole later chapter turns on it: **a peripheral register can
have side effects, even on a read.** Reading a memory word is harmless and
repeatable -- read it twice, nothing changes. Reading a UART data register can
*pop* the received byte off a hardware queue; reading a status register can
*clear* the flags it just reported. The act of reading changed the device. Writes
are worse: a write is a command. This is why you cannot treat the peripheral
aperture like RAM, and why the hardware must be told, per address range, whether
it is looking at ordinary memory (safe to read speculatively, cache, reorder) or
at a device (touch only when and exactly as the program says). That distinction --
"Normal" memory versus "Device" memory -- is the entire subject of Chapter 7.6
(*The CPU reads ahead: memory types and speculative access*); this paragraph is
the reason it exists.

## Buses and the interconnect: where a guard can sit

The interconnect is worth one level of detail, because *its structure* decides
what an OS can and cannot protect. Two facts:

First, an access has two ends. There is the **CPU side** -- the access as it
leaves the core, before it reaches the fabric -- and the **bus-slave side** -- the
access as it arrives at a particular responder (a flash bank, an SRAM bank, a
peripheral). A guard placed on the CPU side sees *every* address the core emits,
including peripheral addresses. A guard placed out on the fabric, in front of one
slave, sees only the traffic that reaches *that* slave. Where the memory
protection unit sits along this path is the whole subject of Chapter 3.7
(*Peripheral isolation and the hardware ceiling*): a CPU-side unit can fence off
an individual device, a slave-side unit that guards only the memories is blind to
devices. Keep the CPU-side / slave-side distinction; that chapter cashes it in.

Second, the fabric is tiered, and the tiers have names you will meet repeatedly.
ARM's on-chip bus family is the common vocabulary across the fleet:

| Bus | Role |
|---|---|
| **AXI** | The high-performance backbone on larger cores -- wide, pipelined, for the memories and fast masters. |
| **AHB** | The main system bus on smaller MCUs -- connects the core, flash, SRAM, and fast peripherals. |
| **APB** | The low-speed **peripheral** bus -- most control registers (timers, UART, GPIO) hang off here. |

A **bridge** connects a fast tier to a slow one (an AHB-to-APB bridge lets the
fast backbone reach the slow peripheral bus). Bridges matter beyond speed: a
bridge is itself a place where access can be *gated* -- some MCUs enforce
"may unprivileged code touch this peripheral?" in the peripheral bridge rather
than in the core's MPU, which is exactly the K64F lesson Chapter 3.7 draws out.
The names AXI/AHB/APB and "bridge" are not trivia; they are how later chapters
say *where* on the hub a thing happens.

## Memory types: flash, SRAM, TCM, external -- and the memory map

Not all memory is the same, and the differences (speed, whether it survives power
loss, whether code can run from it) drive real design choices. The kinds an MCU
puts on the flat address space:

- **Flash.** Non-volatile (keeps its contents with the power off), so it holds the
  program image and read-only data. It is relatively slow to read and very slow to
  write/erase, and it is written in blocks, not bytes. Code normally *executes in
  place* directly from flash.
- **SRAM.** Volatile (contents lost at power-off), fast, byte-writable. It holds
  the data that changes at runtime: variables, heap, and thread stacks. This is
  where a running program does its work.
- **TCM (tightly-coupled memory), split into ITCM and DTCM.** Small SRAM banks
  wired *directly* to the core rather than out across the general fabric, so
  access is fast and jitter-free. **ITCM** is tuned for instructions (hot code),
  **DTCM** for data (hot stacks/buffers). Not every MCU has TCM; where it exists
  it is the memory you put latency-critical code and data in.
- **External memory.** Off-chip flash or RAM reached through a controller (QSPI
  flash, external SDRAM). It is larger but slower and sits behind a controller,
  and its window may be far bigger than the chip actually populates -- a gap that
  Chapter 7.6 shows can trap a speculating core.

The property axis to hold onto: **volatile vs non-volatile** (does it survive
power-off), **speed / wait states** (how many cycles a read costs), and
**execute-in-place** (can the core fetch instructions from it directly).

*Further reading: Hennessy and Patterson, Computer Organization and Design, ch.2
(the memory hierarchy -- why memories trade speed against size and cost).*

Put every memory and every peripheral aperture side by side, each at its fixed
address range, and you have the **memory map**: the single picture of what lives
where in the whole flat address space. Flash here, SRAM there, TCM in its own
window, the peripheral aperture up high, external memory beyond. The memory map
is the master reference for everything address-related in the system -- linker
scripts place code and data into its memory ranges, and the MPU draws its
protection regions over its ranges. Nearly every hardware-facing chapter reads
the memory map; the per-chip specifics live in `../reference/boards.md`, and the
region/domain contract the kernel enforces over it lives in
`../reference/architecture.md` ("Memory domains").

## How to read a memory map and a register table

The two skills the hardware chapters silently assume are reading a memory map and
reading a register table out of a reference manual or datasheet. Both are
mechanical once you know the shape.

**A memory map** is a table of address ranges: for each range, a *start (base)
address*, an *end or size*, and *what responds there* (a named memory or a named
peripheral). Reading it is answering "if the core emits this address, who
answers?" -- and, just as importantly, spotting the ranges where *nobody* answers
(reserved or unpopulated), which are the ones an access must never wander into.

**A register table** describes one peripheral's registers. To reach a specific
register you combine two numbers: the peripheral's **base address** (from the
memory map) plus the register's **offset** (from the register table). Base +
offset is the register's absolute address -- the address your `volatile` pointer
in C must point at. For each register the table then gives you:

- **Bit fields.** A register is rarely one value; it is packed fields, each a
  named span of bits (e.g. bits `[2:0]` select a mode, bit `7` is an enable). You
  read or write a field by masking and shifting.
- **Reset value.** What the field holds after power-on/reset, before any code
  touches it. This tells you the default behavior you inherit -- for instance, a
  peripheral that resets to "supervisor-access-only" is closed to unprivileged
  code until something opens it.
- **Access type.** Whether a field is read-only, write-only, read-write, or
  something exotic like write-1-to-clear (you clear a status flag by writing a
  `1`, not a `0`). Get this wrong and a write silently does nothing, or a read
  disturbs the device (the side-effect hazard from the MMIO section).

That is the whole skill: find the base in the memory map, add the offset from the
register table, and respect each field's width, reset value, and access type.
Chapters 3.7, 7.3, and 7.6 all assume you can do this when they quote a specific
register or address window; the exact per-chip numbers live in
`../reference/boards.md`.

## Where to go next

- The other half of the hardware primer -- how the core is *interrupted* to
  handle an event, and how a trap transfers control: the interrupts-and-traps
  primer (Part 0), which builds directly on the register/state and privilege-bit
  vocabulary defined here.
- What a context switch saves and why it must be assembly -- it saves exactly the
  machine state from the first section: Chapter 3.5, *Context switching and the
  silicon contract*.
- Where a protection unit sits on the interconnect, and why that decides whether a
  peripheral can be fenced per thread: Chapter 3.7, *Peripheral isolation and the
  hardware ceiling*.
- Why a region's *memory type* (Normal vs Device) matters as much as its
  permissions, straight out of the MMIO side-effect fact: Chapter 7.6, *The CPU
  reads ahead: memory types and speculative access*.
- The exact contract over the memory map: `../reference/architecture.md` ("Memory
  domains") and `../reference/boards.md` (per-chip memory maps and quirks).
