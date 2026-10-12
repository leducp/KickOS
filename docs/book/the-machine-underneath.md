<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# The machine underneath: CPU, memory, and peripherals

> What does the machine look like to the software running on it?
>
> No hardware background is needed. You will learn:
>
> - how the core runs a line of C, one instruction at a time;
> - what a function call leaves on the stack;
> - how to read a chip's memory map and register table to drive a device;
> - what a larger processor adds to a microcontroller.

## The CPU core and its state

To software, a CPU core is a set of fast storage slots called **registers** and
logic that fetches and executes instructions. The registers hold the core's
immediate execution state. The important ones are:

- **Program counter (PC).** The address of the next instruction to execute. A
  branch or call changes it.
- **Stack pointer (SP).** The address of the top of the current stack, the
  memory area used for return addresses, saved registers, and some local data.
- **General-purpose registers (GPRs).** Working slots for pointers, counters, and
  intermediate values. The ABI also assigns some to arguments and return values.
  An **ABI** (application binary interface) is the set of rules compiled code
  agrees on so that separately compiled pieces can call each other; its
  **calling convention** names which registers carry arguments and results and
  which ones a function must preserve. Chapter 0.5,
  [*What an ABI is*](the-toolchain-from-source-to-a-running-image.md#what-an-abi-is),
  covers the rest.
- **Status / flags register.** Bits that record conditions such as zero, carry,
  or overflow for later instructions to test.
- **Floating-point (FP) registers.** A separate bank on cores with a hardware FP
  unit. Other cores use software for floating-point operations.
- **Privilege / mode field.** Indicates whether the core is running in
  **privileged** or **unprivileged** mode. Privileged code can configure hardware
  controls; protection hardware restricts what unprivileged code can access.

Together these form the core's **machine state**, or *context*. The CPU executes
one context at a time. An OS saves one thread's context to memory and loads
another's so they can share a core; a **thread** is one flow of instructions the
OS runs, and Chapter 0.2 teaches threads, processes and tasks. Chapter 3.5
(*Context switching and the silicon contract*) explains the switch itself.

*Further reading: Tanenbaum, Modern Operating Systems, ch.1 (the processor, its
registers, and what a program's state is).*

## One instruction at a time: fetch, decode, execute

A program is a sequence of instructions in memory, each a few bytes long. The
core runs it by repeating one cycle:

1. **Fetch.** Read the instruction at the address in PC, and advance PC to the
   next one.
2. **Decode.** Work out what the instruction asks for: which operation, which
   registers, which constant.
3. **Execute.** Do the operation, for example an addition in the arithmetic unit.
4. **Memory access.** Only for an instruction that reads or writes memory. A
   **load** reads a value from a memory address into a register; a **store**
   writes a register's value to a memory address.
5. **Write back.** Put the result in its destination register, and update the
   flags if the instruction sets them.

A branch is an instruction whose execute step writes PC, so the next fetch comes
from somewhere else. Real cores overlap these steps for consecutive instructions
(a *pipeline*), but each instruction still goes through them in this order.

On Arm and RISC-V cores, loads and stores are the only instructions that touch
memory; every computation works on registers. One line of C therefore becomes
several instructions. Take this function, compiled for the STM32F411's Cortex-M4
(the f411 example used through Part 0):

```c
int counter;

void tick(void)
{
    counter = counter + 5;
}
```

The code sits in **flash**, the chip's memory that keeps the program without
power, and `counter` in **SRAM**, its working RAM; both return later in this
chapter. The compiler turns `tick` into five instructions and one data word:

```
ldr  r2, [pc, #8]   @ load the address of counter from the word below
ldr  r3, [r2, #0]   @ load counter's value from that address
adds r3, #5         @ add the constant 5
str  r3, [r2, #0]   @ store the result back to counter
bx   lr             @ return to the caller
.word 0x00000000    @ counter's address, filled in by the linker
```

Each instruction goes through the cycle:

| Instruction | Fetch | Execute | Memory access | Write back |
|---|---|---|---|---|
| `ldr r2, [pc, #8]` | from flash | compute the word's address from PC | **load** the address of `counter` | `r2` |
| `ldr r3, [r2, #0]` | from flash | use `r2` as the address | **load** `counter` from SRAM | `r3` |
| `adds r3, #5` | from flash | add 5 to `r3` | none | `r3`, and the flags |
| `str r3, [r2, #0]` | from flash | use `r2` as the address | **store** `r3` to `counter` | none |
| `bx lr` | from flash | write PC from `lr` | none | none |

Three things stand out. The constant 5 lives inside the `adds` instruction
itself, so it costs no memory access. The variable costs two: one load to read
it and one store to write it back. And the code first has to load the variable's
*address*, because a 32-bit address does not fit inside a 16-bit instruction:
the compiler places it in a data word next to the code, and the linker writes the
real address there (Chapter 0.5 covers that step). `lr`, the **link register**,
holds the return address the call left there, and `bx lr` returns through it.

This is why an **interrupt**, a hardware signal that makes the core run other
code between two instructions (Chapter 0.3), can arrive between the load and the
store: the addition is three separate instructions, not one. Chapter 2.4, *An
atomic buys definedness, not atomicity*, builds on that fact.

## The stack and a call

`tick` returns through `lr`, so something must have put the return address
there. That is the call instruction, `bl` (branch with link): it writes the
address of the instruction after it into `lr`, then branches. `tick` calls
nothing, so `lr` survives until its `bx lr`, and it never uses the stack.

A function that calls on cannot keep its return address in `lr` alone: its own
`bl` would overwrite it. It saves `lr` in memory first, on the **stack**: a
region of SRAM that SP points into. Take a caller of `tick`, in another file:

```c
void tick(void);
void read_pair(int *pair);

int step(int x)
{
    int pair[2];

    read_pair(pair);
    tick();
    return x + pair[0] + pair[1];
}
```

Compiled the same way, it gives (trimmed to the instructions):

```
push  {r4, lr}       @ save r4 and the return address on the stack
sub   sp, #8         @ reserve 8 bytes below them for pair
mov   r4, r0         @ keep x in r4 across the calls
mov   r0, sp         @ pass pair's address as the argument
bl    read_pair      @ call: lr = the next instruction
bl    tick           @ call again: lr is overwritten, the saved copy is safe
ldrd  r2, r3, [sp]   @ load pair[0] and pair[1]
adds  r0, r4, r2     @ x + pair[0], into r0, the return register
add   r0, r3         @ + pair[1]
add   sp, #8         @ release pair
pop   {r4, pc}       @ restore r4, and load the saved lr straight into PC
```

The block `step` owns on the stack is its **stack frame**: its saved registers,
its return address and its locals. While `step` waits for `tick`, it looks like
this:

```
higher addresses
  +----------------------+
  | caller's frame       |
  +----------------------+  <- SP when step was entered
  | saved lr             |
  | saved r4             |
  | pair[1]              |
  | pair[0]              |
  +----------------------+  <- SP while step runs
lower addresses
```

The stack **grows down**: a push lowers SP and stores below the old top, and a
pop reads and raises it again. Each call adds a frame below its caller's, and
each return removes it, so the chain of frames records who called whom.

`x` arrives in `r0`, but `read_pair` may overwrite `r0` to `r3`, so `step` moves
it to `r4`. The ABI makes `r4` to `r11` **callee-saved**: a function that uses
one must give it back unchanged, which is why `step` pushes `r4` and pops it.

A stack therefore holds the live state of one flow of calls: return addresses,
saved registers and locals that the code will read back. Two threads cannot share
one: if thread B pushed onto the stack thread A had left mid-call, B would
overwrite A's frames, and A would return through a wrong address. Each thread
gets its own stack, and its saved context includes its own SP. Chapter 3.6,
*[Thread stacks and the KISS
tension](thread-stacks-and-the-kiss-tension.md)*, covers where KickOS finds that
memory.

## MCU vs application processor: one flat address space, usually no MMU

For an RTOS, the useful distinction is between an **application processor** and
a **microcontroller**.

Application processors, such as those in phones and laptops, usually have a
**memory management unit (MMU)**. It translates the *virtual* addresses a
program uses into the *physical* addresses the bus routes; a **page table** maps
each block of virtual addresses to a physical one. The OS can give each process,
a running program with its own protected memory (Chapter 0.2), its own virtual
address space, at the cost of those tables and the hardware that walks them.

A microcontroller typically puts its core, flash, SRAM, and devices on one chip.
It usually has:

- **On-chip memory.** Flash and SRAM, often measured in kilobytes to a few
  megabytes.
- **A single, flat, physical address space.** Every memory and every device lives
  at a fixed numeric address. With no translation, the same address refers to
  the same physical location for every thread.
- **Usually no MMU.** Some MCUs have a **memory protection unit (MPU)** instead.
  It restricts access to a few address ranges without translating them.

KickOS treats the MCU case as the base: threads share physical addresses, and an
MPU draws protection boundaries around ranges. This is what "MPU-first" means.
What an application processor adds is translation, and with it a private virtual
space per task, KickOS's name for a process (Chapter 0.2). Chapter 7 (*Memory
protection*) covers how KickOS uses each.

*Further reading: Tanenbaum, Modern Operating Systems, ch.1 (kinds of systems)
and ch.3 (memory management: what an MMU does, which is exactly what an MCU
does without).*

## The parts of a microcontroller

Most MCU block diagrams show the same basic parts:

- **Core:** executes instructions and issues loads and stores.
- **Interconnect (bus fabric):** routes accesses to the target memory or device
  and returns responses.
- **Memories:** flash for program code and SRAM for changing data and stacks.
- **Peripherals:** the devices, among them:
  - **timers**, which count clock ticks and can signal when a count is reached;
  - **UARTs**, serial controllers that send and receive bytes over a wire;
  - **GPIO** (general-purpose I/O), pins that software drives high or low, or reads;
  - **ADCs** (analog-to-digital converters), which measure a voltage as a number;
  - **DMA engines** (direct memory access), which copy data between memory and a
    peripheral without the core doing each load and store.

Each memory and peripheral responds to its own range of addresses. The
interconnect routes a core access to that range and returns a result or an error.
An address with no responder may instead leave the access stalled, as Chapter 7.6
explains. Devices are thus reached by address, and a protection unit can guard
only the traffic that passes through it.

## Peripherals and memory-mapped I/O

To start a timer or read a received byte, a program accesses a peripheral
**register**: a fixed-width control or data word at a fixed address. A timer
control register starts the timer; a UART data register returns a byte. Ordinary
loads and stores perform this **memory-mapped I/O (MMIO)**. The peripheral address
range is the **peripheral aperture** (often near `0x40000000` on ARM MCUs). In C,
code commonly accesses it through a pointer to a `volatile` register layout:
`volatile` tells the compiler every access must really happen, in order.

Unlike RAM, **a device register can change state when read**. Reading a UART
data register may remove a byte from its receive queue; reading a status register
may clear flags. A write can issue a command. Chapter 7.6 (*The CPU reads ahead:
memory types and speculative access*) shows what that demands of the hardware.

## Buses and the interconnect: where a guard can sit

Every access has two ends: a **master** starts it (the core, a DMA engine), and
a **slave** answers it (a memory, a peripheral). The interconnect between them
determines which accesses a protection unit can see. On the
**CPU side**, before an access enters the fabric, a guard sees every address the
core emits. On the **bus-slave side**, a guard sees only traffic to the memory or
device in front of it. A slave-side guard on SRAM cannot protect a peripheral.
Chapter 3.7 (*Peripheral isolation and the hardware ceiling*) follows this
distinction across the KickOS fleet.

ARM's common on-chip bus tiers give us names for parts of the fabric:

| Bus | Role |
|---|---|
| **AXI** | The high-performance backbone on larger cores: wide, pipelined, for the memories and fast masters. |
| **AHB** | The main system bus on smaller MCUs: connects the core, flash, SRAM, and fast peripherals. |
| **APB** | The low-speed **peripheral** bus: most control registers (timers, UART, GPIO) hang off here. |

A **bridge** connects tiers; an AHB-to-APB bridge lets the main bus reach slower
peripherals. It may also gate access. On some MCUs, the peripheral bridge rather
than the core's MPU decides whether unprivileged code can touch a device. Chapter
3.7 uses the K64F to show why that placement matters.

## Memory types and the memory map

MCU memory differs in speed, persistence, and whether the core can execute code
from it:

- **Flash.** Retains the program and read-only data without power. Reads are
  relatively slow; writes and erases are slower and work in blocks. Code often
  *executes in place* from flash.
- **SRAM.** Fast, byte-writable memory that loses its contents without power. It
  holds changing data, the heap (the memory `malloc` hands out), and thread
  stacks.
- **TCM (tightly-coupled memory), split into ITCM and DTCM.** Small SRAM banks
  wired close to the core for fast, predictable access. **ITCM** holds
  instructions; **DTCM** holds data. Not every MCU has them.
- **External memory.** Off-chip flash or RAM reached through a controller (QSPI
  flash or SDRAM). It offers more capacity at higher latency. An address window
  can be larger than the memory fitted behind it; Chapter 7.6 shows why that
  matters.

*Further reading: Hennessy and Patterson, Computer Organization and Design, ch.5
(the memory hierarchy: why memories trade speed against size and cost).*

The **memory map** lists these memories and peripherals by address range. The
**linker script**, the file that tells the linker where each part of the program
goes (Chapter 0.5), uses it to place code and data; protection hardware uses it to define
accessible ranges. Per-chip maps live in `../reference/boards.md`, and the
region/domain contract lives in
`../reference/architecture.md` ("Memory domains").

## How to read a memory map and a register table

Hardware chapters use two tables from chip reference manuals: the memory map and
the peripheral register table.

**A memory map** gives each range a *base address*, an *end or size*, and a named
memory or peripheral. Use it to find what responds to an address and to spot
reserved or unpopulated ranges.

**A register table** describes one peripheral. Add its **base address** from the
memory map to a register's **offset** to find that register's address. The table
also gives you:

- **Bit fields.** Named spans of bits: for example, `[2:0]` selects a mode and
  bit `7` enables a feature. Use masks and shifts to access them.
- **Reset value.** The value after reset, before software changes it. This tells
  you the default behavior.
- **Access type.** Read-only, write-only, read-write, or a special rule such as
  write-1-to-clear. A wrong access may do nothing or change device state.

### The f411 example

Part of the STM32F411's memory map, as `platform/stm32f411/chip.yaml` records it
from the reference manual (RM0383):

| Range | Size | What responds |
|---|---|---|
| `0x08000000` - `0x0807FFFF` | 512 KiB | main flash: code and read-only data |
| `0x20000000` - `0x2001FFFF` | 128 KiB | SRAM: data, heap, stacks |
| `0x40000000` - `0x400003FF` | 1 KiB | TIM2, a 32-bit timer |
| `0x40004400` - `0x400047FF` | 1 KiB | USART2, the serial port the console uses |

The two devices sit in the peripheral aperture; the full map holds many more
between and after them. Each row is one of the address ranges the interconnect
routes, and the linker script places code and data in the first two.

USART2's register table gives offsets from its base:

| Register | Offset | Base + offset |
|---|---|---|
| SR (status) | `0x00` | `0x40004400 + 0x00 = 0x40004400` |
| DR (data) | `0x04` | `0x40004400 + 0x04 = 0x40004404` |

One bit of SR shows the other three columns at work: bit 7, TXE, "transmit data
register empty". Sending one byte goes like this:

1. **Reset.** SR's reset value is `0x000000C0`, so TXE starts at 1: a UART fresh
   out of reset is ready to send.
2. **Check.** Software reads SR and tests bit 7. A 1 means DR can take the next
   byte.
3. **Write.** Software writes the byte to DR. The write starts the transmission
   and clears TXE.
4. **Ready again.** Once the hardware has moved the byte on, it sets TXE back to 1,
   and the next byte can go.

TXE's access type is read-only: software never writes it, only the hardware sets
and clears it. SR also shows the read side effect described above: its
receive-error flags clear on a read of SR followed by an access to DR.

A register table also says how wide each access must be. USART2's registers
accept only 16-bit (half-word) or 32-bit (word) reads and writes; a single-byte
access is not guaranteed to work.

## Where to go next

- Chapter 0.2, *[What an OS and a kernel are](what-is-an-os-and-a-kernel.md)*,
  is next: what runs on this machine, and the threads and tasks it runs.
- Chapter 0.3, *[Interrupts and traps](interrupts-and-traps.md)*, explains
  hardware entry into handlers and the kernel, with TIM2's interrupt as the
  example.
- Chapter 3.5, *[Context switching and the silicon
  contract](context-switching-and-the-silicon-contract.md)*, explains how the
  kernel saves and restores CPU state.
- Chapter 3.7, *[Peripheral isolation and the hardware
  ceiling](peripheral-isolation-and-the-hardware-ceiling.md)*, shows why guard
  placement determines peripheral isolation.
- Chapter 7.6, *[The CPU reads ahead](memory-types-and-speculative-access.md)*,
  explains why the hardware must treat device addresses differently from RAM.
- `../reference/architecture.md` ("Memory domains") and
  `../reference/boards.md` hold the exact contracts and per-chip maps.
