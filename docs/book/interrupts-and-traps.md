<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# Interrupts and traps: how the CPU stops what it is doing

> This primer explains the three ways control leaves the running instruction
> stream for a handler: an interrupt, a deliberate trap, and a fault. It
> introduces IRQs, vector tables, interrupt controllers, masking, and critical
> sections. The exact KickOS contracts live in `../reference/`.
>
> *Further reading: Tanenbaum, Modern Operating Systems, ch.1 (what an OS is) and
> ch.5 (input/output: interrupts, the I/O software layers).*

## Why the CPU needs another entry path

A CPU fetches and executes instructions in sequence. A timer, serial port, or
unprivileged program may need the kernel's attention while that sequence runs.
Three entry paths provide the transfer into kernel code:

- an **interrupt**, when a device signals;
- a **deliberate trap**, when the program asks the kernel for a service;
- a **fault**, when an instruction cannot complete.

The f411 example shows all three: TIM2's interrupt on line 28, the syscall that
`printf("hello\n")` ends in, and a stray store to USART2.

## What an interrupt is

An **interrupt**, or **IRQ** (*interrupt request*), is a hardware signal that
makes the CPU save its place and run a handler. When the handler finishes, the CPU
can resume the interrupted code.

An interrupt is **asynchronous**: it arrives at a time the running program did
not choose. A timer expires or a byte arrives, and the CPU responds without
repeatedly checking the device. That repeated checking is **polling**.

### The vector table

The CPU uses a **vector table** to find the handler: an array of addresses or
jump instructions indexed by interrupt number. The handler is an **interrupt
service routine (ISR)**.

The entry mechanism differs by architecture:

- On **ARM Cortex-M**, the table holds handler addresses. On cores with **VTOR**
  (Vector Table Offset Register), software can relocate it.
- On **RISC-V**, **`mtvec`** names the trap entry. In *direct* mode, traps enter
  one handler; in *vectored* mode, interrupt causes select offsets from the base.

## The interrupt controller

A CPU may receive interrupts from timers, devices, and external pins. An
**interrupt controller** tracks and arbitrates these sources. Its key concepts
are:

- **Enable:** whether a source may reach the CPU. Disabling it is *masking*.
- **Pending:** whether a source has signaled and awaits service.
- **Priority:** which source is handled first when several are pending. Some
  controllers allow a higher-priority interrupt to preempt an ISR.

Examples in the KickOS fleet:

- **ARM: the NVIC** (Nested Vectored Interrupt Controller) manages Cortex-M
  interrupt enables, pending state, and priorities.
- **RISC-V: the CLINT and the PLIC.** The **CLINT** (Core-Local Interruptor)
  handles local timer and software interrupts on relevant platforms. The
  **PLIC** (Platform-Level Interrupt Controller) prioritizes external device
  interrupts and routes them to cores.

A **pending** bit can latch an interrupt while its line is masked. When the line
is enabled again, the controller can deliver the pending interrupt. Chapter 3.8,
*[A masked interrupt is latched, not lost](a-masked-interrupt-is-latched-not-lost.md)*,
explains how KickOS preserves this behavior across its driver protocol.

### The f411 example: TIM2 on line 28

On the STM32F411, TIM2 is the kernel's clock. It counts at the 84 MHz core
clock, and its 32-bit counter wraps about every 51 seconds. On each wrap it raises
NVIC line 28. The Cortex-M table starts with 16 core exceptions, so line 28 is
slot 16 + 28 = 44, the word at offset 44 x 4 = `0xB0` of the table at the start
of flash. A few slots of that table:

| Slot | Source | Handler |
|---|---|---|
| 4 | MemManage fault | `HardFault_Handler` |
| 11 | `SVC` trap | `SVC_Handler` |
| 15 | SysTick, the core's own timer | `SysTick_Handler` |
| 44 | TIM2, line 28 | `kickos_tim2_clock_isr` |
| 54 | USART2, line 38 | `kickos_arm_default_irq` |

`arch/arm/chip/stm32f411/startup.S` builds the table. The TIM2 handler, in
`arch/arm/chip/stm32f411/chip_stm32f411.cc`, clears the timer's update flag and
adds the wrap to a 64-bit count, so time read later stays monotonic. The timer
that wakes sleeping threads is SysTick, armed for the next deadline. Every other
device line enters the one generic handler, which turns the interrupt into an
event for a userspace driver (Chapter 3).

## Interrupts, traps, and faults

All three transfer control away from the running program, but differ in origin.

**An interrupt is asynchronous:** a timer or device signals independently of
the instruction the program is executing.

**A deliberate trap is synchronous:** the program executes an instruction to
request a kernel service, such as sending a message or waiting on a semaphore.
The request occurs at that instruction.

The trap instruction has a different name on each architecture, but the same job:

- **ARM: `SVC`** (SuperVisor Call). A userspace thread executes `SVC` to cross
  into the kernel.
- **RISC-V: `ecall`** (environment call). It enters the trap handler.
- **Renesas RX: `INT`** (a software interrupt instruction). It enters an
  interrupt handler in software.

| | Interrupt (IRQ) | Trap (SVC / ecall / INT) |
|---|---|---|
| Origin | outside the program (a device) | the program itself |
| Timing | asynchronous, unpredictable | synchronous, an exact instruction |
| Purpose | handle an external event | request a kernel service |

Both use hardware entry paths that save enough state to return later. A **system
call** uses the deliberate trap path so unprivileged code can request a service
and hardware can perform the privilege transition.

In the f411 example, `printf("hello\n")` formats its text in newlib, which calls
`_write` (`user/src/newlib_stubs.cc`). KickOS's `_write` hands the bytes to the
kernel through a syscall stub (`user/src/syscall_stubs.cc`), and on this core
the stub's `arch_syscall` (`arch/arm/armv7m/switch.S`) executes `svc #0`. The
core enters slot 11, `SVC_Handler`, privileged, at exactly that instruction.
Chapter 0.3 follows the bytes to USART2.

A **fault** is also synchronous, but involuntary: an illegal instruction or
forbidden memory access causes it. It reaches a handler through the same entry
machinery. In the f411 example, an app whose protection regions do not cover
USART2 stores to its data register at `0x40004404`. The MPU refuses the store,
and the core enters slot 4, MemManage, which this table routes to
`HardFault_Handler`. A fault taken by unprivileged code ends that thread's task
and nothing beyond; a fault in the kernel itself is a kernel bug.
`../reference/porting.md` ("Fault-isolation contract") states the rule. In this
book, "trap" means a deliberate request unless we say "fault."

Later chapters show the concrete paths:

- Chapter 3.9, *[The syscall path: trap, dispatch,
  return](the-syscall-path-trap-dispatch-return.md)*, follows a trap from the
  trap instruction through dispatch and back. Chapter 3.5,
  *[Context switching and the silicon contract](context-switching-and-the-silicon-contract.md)*,
  covers the saved state.
- Chapter 4, *[Porting a new ISA: RISC-V](porting-a-new-isa-riscv.md)*, shows how
  the port reads **`mcause`** to distinguish an interrupt from an `ecall` or fault.
- The Journey's [fault and kill](../journey/09-fault-and-kill.md) stop follows a
  user fault through the real code.

## Masking and critical sections

**Masking** holds an interrupt off. On one core, masking the relevant interrupts
lets code update shared state without an ISR interrupting that update. Such code
is a **critical section**. For example, an ISR must not see one field changed
while a related field still has its old value.

The f411 clock is one: `tim2_ticks` reads the TIM2 counter and the wrap count
with interrupts masked, so the TIM2 handler cannot add a wrap between the two
reads. The handler's interrupt stays pending and runs when the mask lifts.

Keep critical sections short: pending interrupts wait until the mask lifts, which
adds response latency. Restore the previous mask state on exit, because critical
sections can nest.

Some hardware masks only interrupts below a priority threshold. ARM Cortex-M uses
**BASEPRI** for this; RISC-V also has the global `mstatus.MIE` bit. KickOS wraps
the per-architecture operation as **`IrqLock`**. Its rules live in
`../reference/invariants.md` (`irqlock-nesting-safe`,
`basepri-write-needs-barrier`) and `../reference/porting.md` (`arch_irq_*`).
Masking one core does not hold off another; `../reference/architecture.md`
("Scheduler", its exclusion rule) states what a multicore kernel adds.

## Where to go next

- [The machine underneath](the-machine-underneath.md) introduces CPU state and
  privilege, and locates TIM2 and USART2 in the memory map.
- Chapter 3 describes KickOS's two-tier interrupt model.
- Chapter 3.5, *[Context switching and the silicon
  contract](context-switching-and-the-silicon-contract.md)*, covers saved state.
- Chapter 3.8, *[A masked interrupt is latched, not
  lost](a-masked-interrupt-is-latched-not-lost.md)*, follows pending interrupts
  through the driver protocol.
- Chapter 3.9, *[The syscall path](the-syscall-path-trap-dispatch-return.md)*,
  follows a trap through dispatch and return.
- Chapter 4, *[Porting a new ISA: RISC-V](porting-a-new-isa-riscv.md)*, shows
  `mcause`, `mtvec`, and `ecall` in a concrete port.
