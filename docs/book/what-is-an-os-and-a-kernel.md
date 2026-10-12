<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# What is an OS, and what is a kernel?

> This primer explains what problems an operating system solves, which part of it
> is the kernel, and why the hardware must enforce the line between the kernel and
> everything else. It introduces user and privileged mode, the system call, what a
> kernel minimally owns, and what makes an OS real-time. It ends with what the
> KickOS kernel owns and where it lives in the tree.
>
> *Further reading: Tanenbaum, Modern Operating Systems, ch.1 (sections 1.1 on what
> an OS is, 1.4 on the kinds of OS including real-time ones, 1.6 on system calls)
> and ch.2 (scheduling, including real-time scheduling).*

## The problem: several programs, one machine

Chapter 0.1 described the machine: one CPU core, some memory, and devices reached
by address. A small embedded product already runs several activities on it: a
control loop that must run every millisecond, a communication stack that waits for
packets, a logger, a user interface. Each is written as if it had the machine to
itself.

Without anything in between, each activity would have to know about all the
others: leave time for them, avoid their memory, agree on who programs the shared
timer and UART. One bug in any of them, a stray pointer or an endless loop, would
bring down all of them.

Take the f411 example from Chapter 0.1: an app on the STM32F411 calls
`printf("hello\n")`, and the bytes leave through USART2, whose registers start at
`0x40004400`. The logger and the communication stack print through the same USART2.
If each wrote its data register directly, their lines would interleave byte by byte,
and one that rewrote the baud-rate register would garble everyone's output. One
UART, several users: that is the problem an OS exists to solve.

An **operating system (OS)** is the software that solves this. It does three jobs.

**It shares the machine.** CPU time is divided between activities, so each runs
when it needs to. Memory is divided so that each gets its own part. Devices are
handed out so that two programs do not drive the same register at once.

**It protects programs from each other.** A program may only touch its own memory
and the devices it was given. A fault in one program is stopped at that program,
and does not corrupt the others or the OS itself.

**It gives programs abstractions.** Instead of a timer register, a program asks
to sleep for 10 ms. Instead of polling a status flag, it waits on an event. Instead
of sharing a variable, it sends a message. These abstractions are the same on every
machine the OS runs on, so the program does not change when the chip does.

Tanenbaum describes this as two views of one thing: the OS as a **resource
manager** (the first two jobs) and the OS as an **extended machine** (the third).

## The OS and the kernel are not the same thing

"Operating system" usually names a whole collection: the kernel, the C library,
device drivers, file systems, network stacks, a shell, utilities. Most of that is
ordinary code, no different in kind from an application.

The **kernel** is the part that runs with the hardware's full powers. It is the
only code that may reprogram the protection unit, mask interrupts, or switch from
one program to another. Everything else runs without those powers and must ask the
kernel when it needs one of them.

Where the line falls is a design choice. A **monolithic** kernel puts drivers,
file systems and the network stack inside the privileged part. A **microkernel**
keeps only what cannot be done elsewhere inside it, and runs the rest as ordinary
programs. Chapter 0.4,
*[Monolithic vs microkernel](monolithic-vs-microkernel.md)*, weighs the two. This chapter
only needs the idea that a kernel exists, and that it is the privileged part.

## Two modes, and why the hardware must enforce them

Protection cannot be a promise that programs make to each other. A program that
contains a bug does not know it contains one, and a hostile program will not keep
the promise. If any program could reprogram the protection unit, the protection
unit would protect nothing. So the line must be drawn by the hardware, which every
program has to obey.

The CPU does this with a **privilege mode**, the bit Chapter 0.1 listed among the
core's registers:

- In **privileged mode** the core may execute every instruction and access every
  control register, including those that configure interrupts, the protection unit,
  and the mode bit itself.
- In **user mode** (unprivileged mode) the core refuses those operations. Depending
  on the instruction and the architecture, the attempt either traps to the kernel
  as a fault or is silently ignored. Either way, it does not take effect.

User mode alone is not enough. A program that cannot touch control registers could
still overwrite the kernel's data with a store instruction. The second half of the
enforcement is the protection unit (MPU or MMU, Chapter 0.1): the kernel programs it
so that a user-mode access outside the program's own memory faults. The mode decides
*which instructions* run; the protection unit decides *which addresses* a user-mode
access may reach. Only privileged code can change either one.

Each architecture spells this differently. A Cortex-M core runs threads unprivileged
through the `CONTROL.nPRIV` bit, while interrupt handlers always run privileged.
RISC-V separates machine mode (M) from user mode (U). x86 calls its levels rings, and
user code runs in ring 3. The idea is the same in all of them: two levels, and a
transfer between them that the less privileged side cannot do on its own terms.

Some cores have no privilege modes at all. Then a kernel can still organise the
machine, but it cannot stop a buggy program from writing the kernel's memory.
Chapter 7.4, *[Privilege is three axes, not one bit](privilege-is-three-axes-not-one-bit.md)*,
takes this case apart.

## The system call: the only door

If user code cannot do privileged work, it needs a way to ask the kernel to do it.
It cannot simply call a kernel function. A call changes the program counter and
nothing else, so it would arrive in kernel code still unprivileged, and at whatever
address the caller chose.

The answer is a **trap instruction** (`SVC` on Arm, `ecall` on RISC-V, `INT` on RX),
described in Chapter 0.2, *[Interrupts and traps](interrupts-and-traps.md)*. When user
code executes it, the hardware switches to privileged mode and jumps to an address
the kernel set up in advance. The program chooses *that* it enters the kernel, never
*where*. A request made this way is a **system call** (syscall): the program puts a
number naming the operation and its arguments in registers, traps, and finds the
result in a register when the kernel returns it to user mode.

This one door is what makes the kernel's checks possible. Every request passes
through the same entry, so the kernel can treat every argument as untrusted: a
pointer may point into another program's memory, a number may name an object the
caller has no right to. The kernel checks before it acts, and refuses with an error
code when the check fails. Chapter 3.9,
*[The syscall path: trap, dispatch, return](the-syscall-path-trap-dispatch-return.md)*,
follows one syscall the whole way.

## The f411 example: why printf needs the kernel

Back to `printf("hello\n")` on the STM32F411. The app runs in user mode, and its
protection regions cover its own code and data, not USART2. A store from the app to
the data register at `0x40004404` (base `0x40004400`, data register at offset
`0x04`) faults. The app cannot own the UART unchecked, because the UART is shared.

So `printf` ends in a syscall. newlib formats the text and calls `_write`
(Chapter 0.6), and KickOS's `_write` hands the bytes to the kernel through the trap.
On the trap's far side the kernel console queues them and writes them to the
USART2 data register one at a time, each time the status register's transmit-empty
bit says the register is free. Every app's output passes through that one queue,
and no app can touch the baud rate. The door is the only path
from the app to the UART, and the kernel decides what goes through it.

A UART can also be handed to a user-space driver instead, with the kernel only
checking who may talk to that driver. Who should own the UART is the question of
Chapter 0.4.

## What a kernel must own

If the kernel is the privileged part, what must be inside it? A useful rule, which
Chapter 3.9 states for KickOS's own syscall list: *an operation belongs in the
kernel only if it needs something only the kernel has.* Four things qualify on any
machine.

**Scheduling.** Deciding which program runs on the CPU, and switching between them,
means saving one program's registers and loading another's (Chapter 3.5,
*[Context switching and the silicon contract](context-switching-and-the-silicon-contract.md)*).
Only code that every program trusts may do this. Timers belong here too: "wake me in
10 ms" is a scheduling decision driven by a hardware timer.

**Memory protection.** Only privileged code can program the protection unit, and it
must be reprogrammed every time the running program changes, since each program has
its own allowed memory.

**Communication and synchronization (IPC).** Once programs are isolated, they need a
safe way to cooperate: send a message, wait on a semaphore, lock a mutex. Waiting
means the scheduler must stop running the waiter and resume it later, so blocking is
kernel work.

**Interrupts.** The vector table and the interrupt controller are privileged
hardware, so the first instruction of every interrupt handler runs in the kernel. The
kernel may handle the event itself, or forward it to an unprivileged driver.

Everything else, such as drivers, file systems, protocol stacks and console output,
*can* run outside the kernel. Whether it *should* is the monolithic vs microkernel
question of Chapter 0.4.

## A general-purpose OS and a real-time OS

A desktop or server OS is a **general-purpose OS**. It aims at throughput and
fairness: finish as much work as possible and give every program a fair share. A
program that occasionally waits 50 ms longer than usual is acceptable if the average
is good.

A **real-time OS (RTOS)** aims at something else: **determinism**. For a motor
controller or a brake, a correct answer that arrives late is a wrong answer. What
matters is not the average response time but the worst one, and the worst one must
be **bounded**: known in advance, and short enough for the application. Hard real
time means missing a deadline is a failure; soft real time means it degrades quality.

That goal changes how a kernel is built:

- **Priority scheduling with preemption.** The most urgent ready thread runs, at
  once, even if a less urgent one is in the middle of its work.
- **Short critical sections.** While the kernel masks interrupts, an urgent event
  waits. Every masked section adds to the worst-case latency, so each must be short
  and of bounded length (Chapter 0.2).
- **No unbounded work in the kernel.** A kernel path whose length depends on how many
  objects exist, or on a search that may retry forever, has no worst case to quote.
- **No priority inversion.** A low-priority thread holding a lock can delay a
  high-priority thread waiting for it; the kernel must bound that delay (Chapter 2.3,
  *[Priority inheritance](priority-inheritance-lending-urgency.md)*).
- **Precise time.** A deadline is only as good as the clock that measures it
  (Chapter 2.1, *[The time base](the-time-base-a-monotonic-clock-you-can-trust.md)*).

## Why a kernel still matters on a microcontroller

Chapter 0.1 showed that a microcontroller usually has one flat physical address
space and no MMU. Every address means the same thing to every thread. It is fair to
ask what a kernel adds there. There are three common ways to build MCU firmware.

**No kernel: a superloop.** One `main` loop polls each activity in turn, and
interrupt handlers do the urgent parts. This is simple and works for small products.
It breaks down when activities must wait: a loop that waits for a packet stops the
control loop, so every activity must be rewritten as a state machine that never
blocks. Response time becomes the length of the whole loop.

**A kernel as a library.** A scheduler, semaphores and timers are linked into the
image, and every thread runs privileged. Threads can block, and priorities give
bounded response times. A syscall is an ordinary function call, which is cheap. But
nothing protects one thread from another: a stray pointer in the logger can corrupt
the control loop's state, or the scheduler's.

**A protected kernel.** The kernel runs privileged and threads run unprivileged. The
protection unit, even an MPU that only draws ranges and translates nothing, fences
each thread's memory. A stray pointer faults instead of corrupting, and the kernel
can stop the faulty program while the rest keep running. The cost is a trap on every
syscall.

A flat address space does not remove the need for a kernel: scheduling, blocking,
timers and interrupts are the same problems as on a large machine, and an MPU gives
real protection without address translation.

## What KickOS chose, and why

KickOS is a protected kernel, and a microkernel. Its kernel runs privileged, its
threads unprivileged, and every request crosses a trap. The goal is isolation that
holds on every target the project supports, with the same semantics everywhere, so
that a fault in one task stays in that task.

The kernel is kept small: it owns the four jobs above and little else. Its scheduler
is priority-based and preemptive, and **tickless**: it programs a one-shot timer for
the next deadline instead of waking on a periodic tick. Drivers run unprivileged and
receive their interrupts from the kernel as events. Console output, files and
protocols are userspace servers reached by IPC; the kernel keeps one debug console
write, for the panic and bring-up paths.

The cost of a trap per request is managed rather than avoided. The syscall list is
small, and when an operation is hot and safe, the kernel grants a program direct
access to the hardware instead of adding a syscall (Chapter 8.4,
*[The fast path is the capability](the-fast-path-is-the-capability-gpio-direct-mmio.md)*).
Where the hardware has no privilege modes, as on the ESP32's Xtensa LX6 core, the
syscall becomes a plain function call. The kernel still checks every request's
authority, but it cannot stop a thread that writes kernel memory directly.

## Where the KickOS kernel lives in the tree

The privileged part of KickOS is two directories.

**`kernel/`** is the kernel itself, in freestanding C++. It touches no hardware
register directly. Its subdirectories map onto the four jobs:

| Job | Where |
|---|---|
| Scheduling | `kernel/sched/` (ready queues and the FIFO/round-robin policy), `kernel/thread/`, `kernel/time/` (the time base and the sleep queue) |
| Memory protection | `kernel/domain/` (the regions a task may touch), `kernel/mem/`, `kernel/task/` |
| IPC and synchronization | `kernel/sync/` (semaphores, mutexes, the wait queue), endpoints in `kernel/syscall/syscall_ipc.cc` |
| Interrupts | `kernel/irq/` (interrupts delivered as events), `kernel/notify/` |

The door is `kernel/syscall/`: `syscall_dispatch` in `kernel/syscall/syscall.cc`
routes each syscall number, and `kernel/syscall/cap.cc` holds the capability table
that every object-naming syscall checks. Boot starts in `kmain`
(`kernel/init/kmain.cc`).

**`arch/`** is the hardware seam. `arch/include/kickos/arch/arch.h` lists every
operation the kernel may ask of the hardware, such as `arch_switch` (the context
switch), `arch_timer_arm` (the one-shot timer) and `arch_mpu_apply` (the protection
unit). Below it, each CPU family, ISA and chip supplies its own code, including the
trap entry in assembly.

Everything else is unprivileged. `user/` is the runtime every application links; its
`user/src/syscall_stubs.cc` holds the user side of the door: each `kos_*` call packs
its arguments and calls `arch_syscall`, which executes the trap. `system/` holds the first user program, the init in
`system/init/compose/`, and the user-space drivers in `system/driver/<chip>/`. The
[Reference: repo layout](../reference/architecture.md#repo-layout) is the full map,
and [Reference: architecture](../reference/architecture.md) ("User/kernel
separation") the exact contract.

## See it in the code

The journey walks these ideas through the source with gdb:

- [1. Power-on](../journey/01-power-on.md) follows one image from reset, through
  `kmain` and the first context switch, to the first unprivileged instruction of the
  application's `main`. It is the moment the machine splits into a kernel and a
  program.
- [2. The first syscall](../journey/02-first-syscall.md) follows a `kos::print` call
  through the trap, `syscall_dispatch`, the argument and handle checks, and back to
  user mode. It is the door, opened once.

## Where to go next

- How the trap instruction and the vector table work: Chapter 0.2,
  *[Interrupts and traps](interrupts-and-traps.md)*.
- Where the kernel line falls, and who owns the f411 example's UART: Chapter 0.4,
  *[Monolithic vs microkernel](monolithic-vs-microkernel.md)*.
- Building the f411 example and flashing it: Chapter 0.5,
  *[How to read this book, and the toolchain](how-to-read-this-book-and-the-toolchain.md)*.
- What `printf` pulls in before it reaches `_write`: Chapter 0.6,
  *[What's under #include](whats-under-include-libc-and-the-cxx-runtime.md)*.
- One syscall from trap to return, and why the KickOS syscall list is short:
  Chapter 3.9, *[The syscall path: trap, dispatch, return](the-syscall-path-trap-dispatch-return.md)*.
- What a context switch saves and restores, privilege included: Chapter 3.5,
  *[Context switching and the silicon contract](context-switching-and-the-silicon-contract.md)*.
- What "privileged" means once CPU mode, memory access and authority are separated:
  Chapter 7.4, *[Privilege is three axes, not one bit](privilege-is-three-axes-not-one-bit.md)*.
- The exact user/kernel contract: `../reference/architecture.md` ("User/kernel
  separation") and `../reference/invariants.md`.
