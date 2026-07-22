<!-- SPDX-License-Identifier: CECILL-C -->
# Interrupts and traps: how the CPU stops what it is doing

> A front-of-book primer on the one mechanism the whole rest of the interrupt
> story rests on: how a CPU is made to stop the instruction stream it is running
> and jump somewhere else -- either because the outside world knocked (a device
> interrupt) or because the running program asked it to (a trap into the kernel).
> This chapter *defines the vocabulary* -- IRQ, vector table, interrupt
> controller, pending, mask, synchronous vs asynchronous, critical section -- that
> the later chapters lean on without re-introducing. It teaches the ideas; the
> exact KickOS contract lives in `../reference/`, and the chapters that build on
> each idea are forward-linked as they come up.
>
> *Further reading: Tanenbaum, Modern Operating Systems, ch.1 (what an OS is) and
> ch.5 (input/output -- interrupts, the I/O software layers).*

## The problem: a CPU only does one thing

A processor, left alone, does exactly one thing: it fetches the instruction the
program counter points at, runs it, advances the program counter, and repeats. It
has no idea that a timer somewhere just reached zero, that a byte just arrived on a
serial port, or that the program it is running would like to ask the operating
system for a service. Nothing in "fetch, execute, advance" makes room for any of
that.

So how does a key you press reach a program that is busy computing? How does a
one-millisecond timer ever get to run the scheduler if the CPU is deep inside some
loop? And how does an ordinary, unprivileged program hand control to the kernel
without the kernel having to constantly *check* whether it was needed?

The answer to all three is the same piece of hardware machinery, and it has two
faces. This chapter is about that machinery.

## What an interrupt is

An **interrupt** -- often abbreviated **IRQ**, for *interrupt request* -- is a
hardware signal that makes the CPU stop the instruction stream it is running,
remember exactly where it was, and jump to a designated piece of code. When that
code finishes, the CPU restores where it was and resumes as if nothing had
happened. The interrupted program never consented and, done right, never even
notices.

The essential word is **asynchronous**: the interrupt arrives *at a moment the
running code did not choose*. A network packet lands, a timer expires, a button is
pressed -- none of these are scheduled by the program that gets interrupted. They
happen on the world's clock, not the program's. That is the whole point:
interrupts are how a CPU that can only do one thing at a time still reacts
promptly to events it cannot predict. The alternative -- **polling**, where the
program repeatedly asks "anything yet? anything yet?" -- burns the CPU on
questions whose answer is almost always "no." An interrupt inverts that: stay
quiet until there is something to say, then speak.

### The vector table: a hardware-indexed jump table

When an interrupt fires, how does the CPU know *which* code to jump to? It cannot
search; it is hardware, mid-instruction. The answer is a **vector table**: a fixed
array of code addresses (or of jump instructions), one slot per interrupt source,
laid out at a location the silicon knows. Each interrupt has a number, and that
number is an index into the table. The CPU takes the interrupt, looks up slot
number N, and jumps to the address it finds there. No search, no branching on the
cause -- just an array lookup wired into the hardware.

The routine a vector points at is the **interrupt service routine**, or **ISR**:
the code that runs to handle that particular interrupt.

Where the table lives, and whether you can move it, is per-architecture:

- On **ARM Cortex-M**, the vector table is an array of addresses. It sits at the
  bottom of memory at reset, but a register called **VTOR** (the Vector Table
  Offset Register) lets software *relocate* it -- typically into RAM, so the table
  can be built or patched at runtime.
- On **RISC-V**, a control register named **`mtvec`** (machine trap-vector) holds
  the address the CPU jumps to on a trap. It can be configured in *direct* mode
  (every trap enters one common handler, which then works out the cause) or
  *vectored* mode (an indexed table, like ARM's).

The vocabulary to carry forward: **an interrupt is an asynchronous jump, and the
vector table is the hardware-indexed jump table that decides where it lands.**

## The interrupt controller

A CPU has many possible interrupt sources -- dozens of on-chip peripherals, timers,
external pins -- and it needs a way to arbitrate among them: which are turned on,
which are waiting to be serviced, which is most urgent. That arbitration is not
done by the CPU core itself; it is done by a dedicated block of hardware, the
**interrupt controller**, that sits between the peripherals and the core.

Every interrupt controller, whatever its name, exposes the same three kinds of
bits per source:

- **Enable** (also called *unmask*): is this source allowed to reach the CPU at
  all? A disabled source is ignored by the core.
- **Pending**: has this source fired and not yet been serviced? The controller
  *sets* pending when the event arrives and *clears* it when the ISR takes it.
- **Priority**: when several sources are pending at once, which one wins? A
  higher-priority interrupt can even interrupt the ISR of a lower-priority one
  (called *nesting* or *preemption*).

The named examples across the KickOS fleet:

- **ARM: the NVIC** (Nested Vectored Interrupt Controller), built into every
  Cortex-M core. It holds enable, pending, and priority registers per interrupt,
  and its "nested" and "vectored" names announce exactly the two features above:
  priority-based preemption and a hardware vector table.
- **RISC-V: the CLINT and the PLIC.** The **CLINT** (Core-Local Interruptor)
  handles the two most basic, per-core sources -- the timer interrupt and the
  software interrupt. The **PLIC** (Platform-Level Interrupt Controller) handles
  the many *external* device interrupts, arbitrating them by priority and routing
  the winner to the core.

The crucial property -- the one a later chapter builds an entire correctness
argument on -- is what the **pending** bit means when a line is **masked**
(disabled). The pending bit is a **latch**: if a source fires while its line is
disabled, the controller *remembers* it by setting pending, and the interrupt is
delivered the instant the line is enabled again. The event is not lost; it is
*deferred*. This is not a KickOS invention -- it is what the NVIC and PLIC silicon
do natively. Chapter 3.8, *[A masked interrupt is latched, not
lost](a-masked-interrupt-is-latched-not-lost.md)*, is entirely about why the
kernel must honor that latch (and why a tempting "just drop it while masked"
shortcut is a quiet lie); this section is the substrate it assumes.

## Synchronous vs asynchronous: two reasons to reach the kernel

Here is the load-bearing distinction of the whole chapter, and the one that later
chapters use as settled vocabulary.

Both of the following stop the running instruction stream and jump through the
vector/trap table into privileged code. But they do so for *opposite* reasons, and
conflating them is the single most common confusion for a reader new to the topic.

**An interrupt is an outside event (asynchronous).** A device fired. The timer
expired. A pin changed. The running program did nothing to cause it and cannot
predict when it will come. This is everything the two sections above described.

**A trap is a deliberate request (synchronous).** The running program *executes a
specific instruction* whose entire purpose is to jump into the kernel. It is not
an accident and not an outside event -- it is a program asking, on purpose, for a
service it is not privileged to perform itself (read a file, send a message, block
until a semaphore is free). It is called **synchronous** because it happens at an
exact, program-chosen point in the instruction stream: the trap instruction. Run
that program again and the trap lands at the same place every time.

The trap instruction has a different name on each architecture, but the same job:

- **ARM: `SVC`** (SuperVisor Call). A userspace thread executes `SVC` to cross
  into the kernel.
- **RISC-V: `ecall`** (environment call). Same role -- the user-to-machine-mode
  door.
- **Renesas RX: `INT`** (a software interrupt instruction). Same idea again.

So both an interrupt and a trap arrive at the kernel through the vector/trap table.
The difference is *who initiated it and why*:

| | Interrupt (IRQ) | Trap (SVC / ecall / INT) |
|---|---|---|
| Origin | outside the program (a device) | the program itself |
| Timing | asynchronous -- unpredictable | synchronous -- an exact instruction |
| Meaning | "the world needs attention" | "I request a kernel service" |

Why does one mechanism serve both? Because the *effect* the kernel needs is
identical in both cases: stop the current code, switch to privileged mode, and run
trusted kernel code with the interrupted state safely saved. The silicon already
builds that effect for interrupts, so exposing a *deliberate* instruction that
triggers the same machinery is nearly free -- and it gives an unprivileged program
the only safe way to enter the kernel. This is why a **system call** (a program's
request to the OS) is implemented as a trap: the program cannot simply *jump* into
kernel code (that would defeat privilege separation), so it raises a trap and lets
the hardware perform the privileged crossing on its behalf.

Two later chapters carry this split forward:

- What happens *after* the trap lands -- how the `SVC`/`ecall` handler hands off to
  a small **trampoline** so a blocking system call can suspend and resume the
  calling thread cleanly -- is Chapter 3.5, *[Context switching and the silicon
  contract](context-switching-and-the-silicon-contract.md)*.
- How a single trap handler *tells the two apart* at runtime -- reading a cause
  register to demultiplex "interrupt" from "deliberate trap/exception" -- is shown
  concretely in the RISC-V port, which reads **`mcause`** (its top bit
  distinguishes an interrupt from an `ecall` or a fault) in Chapter 4,
  *[Porting a new ISA: RISC-V](porting-a-new-isa-riscv.md)*.

A third category rounds out the picture: a **fault** (or *exception* in the narrow
sense) -- a divide-by-zero, an illegal instruction, a forbidden memory access. Like
a trap it is synchronous (caused by the very instruction executing) but, like an
interrupt, it is *involuntary* -- the program did not want it. It reaches the
kernel through the same table. In this book "trap" means the deliberate,
service-requesting kind unless a fault is named explicitly.

## Masking and critical sections

The last piece of vocabulary follows directly from the enable bit. If enabling a
line lets its interrupt through, then **disabling** it -- **masking** -- holds the
interrupt off. And because an interrupt is the *only* way the CPU's attention gets
yanked away mid-sequence, turning interrupts off for a stretch of code guarantees
that stretch runs start-to-finish with nothing else slipping in between.

That is exactly what a **critical section** needs. Consider code that updates two
related fields, or reads-then-writes a shared counter. If an interrupt fires
halfway through and its ISR touches the same data, it can observe a half-finished
update, or its own change can be silently overwritten when the interrupted code
resumes. Making the sequence **atomic** -- indivisible, all-or-nothing from any
other context's point of view -- is the fix, and the simplest tool for it on a
single core is: mask interrupts, do the sequence, unmask.

There is nothing free about it. While interrupts are masked, the CPU is *deaf* --
a timer tick, an urgent device, a high-priority event all have to wait. Hold the
mask too long and you add latency to everything the system was supposed to react
to promptly; that lengthened worst-case response is the enemy of a real-time
system. So the rule of thumb is universal: a critical section must be **as short as
possible**, and interrupts must be restored to *exactly* their prior state on the
way out (not blindly switched back on -- critical sections nest, and an inner one
must not unmask something an outer one deliberately masked).

Real hardware refines the blunt on/off switch. Rather than mask *all* interrupts,
many controllers let you mask only those *below a chosen priority*, so the most
urgent handful stay live even inside a critical section. ARM Cortex-M does this
with a register called **BASEPRI** (mask everything at or below a priority
threshold); RISC-V's basic control is the global `mstatus.MIE` enable bit. KickOS
wraps this per-architecture concept in one primitive, **`IrqLock`**, so the kernel
above the seam says "enter a critical section" and each port implements it with the
right instruction. This chapter only names the concept; the design -- including why
raising BASEPRI needs a memory barrier, and why device interrupts must be
programmed into the maskable priority band -- is carried by Chapter 3.8 and the
per-ISA chapters, and the exact, code-synced rules live in
`../reference/invariants.md` (see `irqlock-nesting-safe`, `basepri-write-needs-barrier`,
and `device-irq-in-maskable-band`) with the per-controller `arch_irq_*` contract in
`../reference/porting.md`. Do not memorize the register names; memorize the idea:
**masking buys atomicity by trading away responsiveness, so you buy as little as
you can get away with.**

## Where to go next

- **Chapter 0.1** (the front-of-book primer on the machine: privilege, user vs
  kernel mode) -- the ground this chapter stands on: *why* a program must trap into
  the kernel rather than jump there is the privilege boundary that chapter defines.
- **Chapter 3** (*Interrupt model*) -- how KickOS turns the raw ISR into a
  two-tier design: a tiny first-level handler plus a userspace driver thread.
- Chapter 3.5, *[Context switching and the silicon
  contract](context-switching-and-the-silicon-contract.md)* -- what happens after a
  trap lands: the syscall trampoline and the saved-state contract.
- Chapter 3.8, *[A masked interrupt is latched, not
  lost](a-masked-interrupt-is-latched-not-lost.md)* -- the pending-while-masked
  latch this chapter introduced, taken all the way to why the kernel must never
  drop it.
- Chapter 4, *[Porting a new ISA:
  RISC-V](porting-a-new-isa-riscv.md)* -- the trap-vs-interrupt demux (`mcause`),
  the `mtvec` vector, and the `ecall` syscall path, worked end to end on real
  silicon.
