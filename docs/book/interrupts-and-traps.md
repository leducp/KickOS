<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# Interrupts and traps: how the CPU stops what it is doing

> What can stop a running program to run other code first?
>
> Three things can: a device reports an event (an interrupt), a program asks the
> kernel for something (a trap), or an instruction cannot complete (a fault). You
> will learn:
>
> - how the core finds the code to run for each
> - how the f411 example's timer interrupt and `printf` reach their handlers
> - how to keep an interrupt out of a critical moment in your own code
>
> *Further reading: Tanenbaum, Modern Operating Systems, ch.1 (what an OS is) and
> ch.5 (input/output: interrupts, the I/O software layers).*

## Why the CPU needs another entry path

A CPU fetches and executes instructions in sequence. [Chapter 0.2](what-is-an-os-and-a-kernel.md) showed why the
kernel must still take control while that sequence runs: a timer expires, a byte
arrives, a program asks for a service, or a program touches memory it does not
own. The hardware provides three entry paths into kernel code:

- an **interrupt**, when a device reports an event, such as a byte that arrived
  or a timer that expired
- a **trap**, when a program asks the kernel for a service
- a **fault**, when an instruction cannot complete

The f411 example shows all three: TIM2's interrupt on line 28, the syscall that
`printf("hello\n")` ends in, and a stray store to USART2.

## What an interrupt is

A device often has news for the CPU: a byte arrived on the UART, a timer reached
the time it was set for, a data transfer finished. An **interrupt**, or **IRQ**
(*interrupt request*), is the hardware signal a device sends to the CPU to say so.

When the signal comes, the CPU stops the running program between two
instructions and saves the program's **context**: everything needed to resume it
exactly. That is where it was (the program counter), its status (the flags,
whether interrupts are enabled, the privilege mode) and the values in its
registers. The hardware saves part of the context as it jumps, and the kernel's
entry code saves the rest. The jump goes to a **handler**, a function the kernel
installed in advance for that kind of event. The handler does the work the event
calls for, such as reading the byte that arrived, and returns. The context is then
restored, and the program continues from the instruction where it stopped.

Instead of the interrupted program's context, the kernel may restore another
thread's saved context, and the CPU then continues that thread. This is how a context switch works (Chapter 3.5,
*[Context switching and the silicon
contract](context-switching-and-the-silicon-contract.md)*).

An interrupt is **asynchronous**: nothing in the program's own instructions says
when it comes. The moment depends on the device, not on the code, so the same
program run twice can be interrupted at a different instruction each time.
Reading its own instructions, a program cannot tell where it will be interrupted.

The CPU could do without interrupts. It could check each device in a loop: read
the UART's status register, then the timer's, then the next device's, and start
again. This is **polling**. It works, but the CPU spends its time asking, and
most answers are "nothing happened". Worse, how long an event waits before the CPU
notices it depends on how many devices the loop checks and how long each check
takes. With interrupts, the CPU does useful work, and the device tells it when
there is something to do.

### The vector table

When an interrupt arrives, the CPU must know where its handler starts. The kernel
tells the hardware in advance, and the hardware looks the address up each time.
A handler for an interrupt is also called an **interrupt service routine (ISR)**.

How the CPU finds the handler is part of the core's **Instruction Set
Architecture (ISA)**: the instructions a family of cores understands, and the
rules that come with them, such as how an interrupt is entered. For instance:

- **ARM Cortex-M** keeps a **vector table**: an array in memory with one handler
  address per cause, indexed by the interrupt's number. The CPU reads the entry for
  the interrupt that arrived, and jumps to the address it finds there.
- **RISC-V** keeps a single address in a register, **`mtvec`** (*machine trap
  vector*). In its simplest setup, every interrupt, trap and fault jumps to that
  one address, and the code there reads a second register, `mcause`, to learn what
  happened and pick the right handler.

Both answer the same question, where the CPU jumps. Cortex-M looks the handler up
by cause, in a table. RISC-V jumps to one place and lets the code sort the causes
out.

## The interrupt controller

A CPU may receive interrupts from timers, devices, and external pins. An
**interrupt controller** tracks and arbitrates these sources. Its key concepts
are:

- **Enable:** whether a source may reach the CPU. Disabling it is *masking*.
- **Pending:** whether a source has signaled and awaits service.
- **Priority:** which source is handled first when several are pending. Some
  controllers let a higher-priority interrupt **preempt** an ISR: interrupt it
  and run first.

On a Cortex-M the controller is the **NVIC** (Nested Vectored Interrupt
Controller), which manages interrupt enables, pending state, and priorities. Other
core families have their own controller. You will find a comparison of them in Chapter 4,
*[One kernel, many ISAs](one-kernel-many-isas.md)*.

Enable and pending work together. Suppose the kernel masks interrupts for a short
moment, to update two values that must change together (see the critical section
at the end of this chapter). The timer expires during that moment. The controller does
not drop the event: it sets the timer's pending bit, and the bit stays set while
the interrupt is masked. As soon as the kernel unmasks, the controller sees the
bit and delivers the interrupt. The handler runs a little late, but it runs: a
masked interrupt is delayed, not lost. The bit records only that the event
happened at least once: if the timer expires twice while the interrupt is masked,
the handler still runs once. Chapter 3.8, *[A masked interrupt is
latched, not lost](a-masked-interrupt-is-latched-not-lost.md)*, explains how
KickOS keeps this true for interrupts it hands to drivers.

### The f411 example: TIM2 on line 28

On the STM32F411, TIM2 is the kernel's clock. It counts at the 84 MHz core
clock, and its 32-bit counter wraps about every 51 seconds. On each wrap it raises
NVIC line 28. The first 16 slots of a Cortex-M vector table belong to the core itself:
the starting stack pointer, reset, the faults, `SVC` and SysTick. Device lines
follow, so line 28 is slot 16 + 28 = 44, the word at offset 44 x 4 = `0xB0` of the
table at the start of flash. Here are a few of its slots, as an example:

| Slot | Source | Handler |
|---|---|---|
| 4 | MemManage fault | `HardFault_Handler` |
| 11 | `SVC` trap | `SVC_Handler` |
| 15 | SysTick, the core's own timer | `SysTick_Handler` |
| 44 | TIM2, line 28 | `kickos_tim2_clock_isr` |
| 54 | USART2, line 38 | `kickos_arm_default_irq` |

The chip's startup code builds this table. The TIM2 handler clears the timer's
update flag and adds the wrap to a 64-bit count, so the time read later is
**monotonic**: it never goes backwards. Every other device line enters the one generic handler, which
turns the interrupt into an event for a userspace driver ([Chapter 3](interrupt-model.md)).

## What a trap is

A **trap** is how a program calls the kernel. Some work only the kernel is allowed
to do, such as starting a new thread or waiting for a message from another
program. To ask for it, the program executes a
special instruction, and this instruction makes the CPU jump into the kernel. The
system call of [Chapter 0.2](what-is-an-os-and-a-kernel.md) works this way. Unlike an interrupt, a trap is
**synchronous**: it happens at that exact instruction, every time the program
runs.

The trap instruction has a different name on each ISA. For instance:

- **ARM: `SVC`** (*SuperVisor Call*)
- **RISC-V: `ecall`** (*Environment Call*)
- **Renesas RX: `INT`** (*software Interrupt*)

They all make the CPU switch to privileged mode and jump to the kernel's trap
entry.

The CPU finds that entry the same way it finds an interrupt handler: in the
vector table on Cortex-M and RX, at the address in `mtvec` on RISC-V. And as for
an interrupt, the program's context is saved on the way in, so if the call
returns, the program continues after the trap instruction.

In the f411 example, `printf("hello\n")` formats its text in newlib, the C library,
which then calls `_write` to output it. KickOS's `_write` hands the bytes to the
kernel through a small syscall stub, and on this core the stub executes
`svc #0`. The core enters slot 11, `SVC_Handler`, privileged, at exactly that
instruction. In this example, the kernel owns the console itself, so the kernel
then writes the bytes to USART2, as [Chapter 0.2](what-is-an-os-and-a-kernel.md) showed.

## What a fault is

A **fault** is synchronous too, but involuntary: the instruction was not meant to
enter the kernel, it simply cannot complete. An illegal instruction or a
forbidden memory access causes it, and it reaches a handler through the same
entry machinery. In the f411 example, an app whose protection regions do not cover
USART2 stores to its data register at `0x40004404`. The MPU refuses the store,
and the core enters slot 4, MemManage, which this table routes to
`HardFault_Handler`. Most faults in unprivileged code end the task of the faulting
thread, while the other tasks keep running.

**A fault in the kernel itself stops the whole system.** The kernel can no longer
trust its own state, so it does not try to carry on.

## Masking and critical sections

**Masking** holds an interrupt off. On one core, masking the relevant interrupts
lets code update shared state without an ISR interrupting that update. Such code
is a **critical section**. For example, an ISR must not see one field changed
while a related field still has its old value.

Reading a clock can be one. Suppose the time is made of two parts: a hardware
counter, and a count of how many times that counter wrapped, which the timer's
interrupt handler increases. Read this simple way, code that reads both parts has
to mask that interrupt, so the handler cannot change the wrap count between the
two reads. The interrupt
stays pending and its handler runs as soon as the mask lifts.

Keep critical sections short: pending interrupts wait until the mask lifts, which
adds response latency. Restore the previous mask state on exit, because critical
sections can nest.

Some cores mask every interrupt at once. Others mask only those below a priority
threshold, so the most urgent still get through. KickOS wraps whichever the core
offers as **`IrqLock`**, an object that masks when it is created and restores the
previous state when it goes out of scope.

On a chip with several cores, masking is not enough. Masking stops interrupts on
the core that masks, but the other cores keep running, and they can change the
same data at the same moment. So on such a chip, `IrqLock` also takes a lock: a
core that wants the same data waits until the lock is free. Chapter 2.5, *[One
kernel lock, many ready queues](one-lock-many-ready-queues.md)*, explains it.

## Summary

| Question | Interrupt (IRQ) | Trap | Fault |
|---|---|---|---|
| What triggers it? | A device event, such as TIM2 wrapping | A program executes `SVC`, `ecall`, or `INT` | An instruction fails, such as a forbidden store |
| When? | Asynchronously, between instructions. Delivery can wait while masked | Synchronously, at the trap instruction | Synchronously, at the failing instruction |
| What does the kernel do? | Handle the event | Perform the requested service | Handle the error, possibly ending the user task |

## Where to go next

- Chapter 0.4, *[Monolithic vs microkernel](monolithic-vs-microkernel.md)*, asks
  who should own the f411 example's UART once its interrupt reaches the kernel.
- Chapter 3, *[The interrupt model](interrupt-model.md)*, describes how KickOS
  takes an interrupt in the kernel and hands it to a driver.
- Chapter 3.5, *[Context switching and the silicon
  contract](context-switching-and-the-silicon-contract.md)*, covers the saved context.
- Chapter 3.8, *[A masked interrupt is latched, not
  lost](a-masked-interrupt-is-latched-not-lost.md)*, follows pending interrupts
  through the driver protocol.
- Chapter 3.9, *[The syscall path](the-syscall-path-trap-dispatch-return.md)*,
  follows a trap through dispatch and return.
- Chapter 4, *[One kernel, many ISAs](one-kernel-many-isas.md)*, compares trap
  entry, interrupt controllers and critical sections across the core families.
- *[Porting a new ISA: RISC-V](porting-a-new-isa-riscv.md)* shows how a RISC-V
  port tells an interrupt from an `ecall` or a fault.
- The Journey's [fault and kill](../journey/09-fault-and-kill.md) stop follows a
  user fault through the real code.
