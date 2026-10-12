<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# What is an OS, and what is a kernel?

> What is an operating system for, and which part of it is the kernel?
>
> You will learn:
>
> - what a thread and a task are;
> - why one thread sometimes waits for another;
> - why `printf` on a microcontroller has to ask the kernel;
> - where the kernel sits in the KickOS source tree.
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
only code that may reprogram the protection unit, mask interrupts (hold off the
hardware signals that make the core run other code between two instructions,
Chapter 0.3), or switch from one program to another. Everything else, called
**userspace**, runs without those powers and must ask the kernel when it needs one
of them. A **device driver**, the code that operates one device such as a UART, may
sit on either side.

Where the line falls is a design choice. A **monolithic** kernel puts drivers,
file systems and the network stack inside the privileged part. A **microkernel**
keeps only what cannot be done elsewhere inside it, and runs the rest as ordinary
programs. Chapter 0.4,
*[Monolithic vs microkernel](monolithic-vs-microkernel.md)*, weighs the two. This chapter
only needs the idea that a kernel exists, and that it is the privileged part.

## Programs, threads and tasks

The problem above spoke of "activities". An OS needs two sharper words for them.

A **thread** is one flow of instructions that the kernel can stop and later resume.
It has its own context, the registers of Chapter 0.1 (PC, SP, the general-purpose
registers, the flags, the privilege bit), and its own stack, for the reason
Chapter 0.1's [*The stack and a call*](the-machine-underneath.md#the-stack-and-a-call)
gives. To stop a thread, the kernel saves its context to memory; to resume it, the
kernel loads that context back into the core. Moving the core from one thread to
another this way is a **context switch** (Chapter 3.5). The thread cannot tell it
was stopped.

At any moment a thread is in one of three states:

- **running**: its context is in the core, executing;
- **ready**: it could run, and waits only for a core;
- **blocked**: it waits for something else, such as a byte, a timer or another
  thread, and must not run until that happens.

The **scheduler** is the kernel code that picks which ready thread runs next.

One program often does several things at once. Suppose the f411 example, besides
printing, also counts presses of a button on the board. Waiting for a press and
printing the count are two separate activities, each with its own flow of calls,
so the program runs them as two threads. Both use the same variable, the count:
one thread adds to it, the other prints it. So the two threads must share memory.

The unit that owns that shared memory is what Tanenbaum calls a **process**: a
running program with its own protected memory. KickOS calls it a **task**. A task
is a group of threads that share one set of memory and live and die together.
Each thread of a task also holds its own list of the **kernel objects** it may use
(the locks, queues and timers the kernel keeps on a program's behalf): its
capabilities, the subject of Part 8.

What a task's memory looks like depends on the hardware. An **address space** is
the set of addresses a program can use, and what each of them reaches. On an
application processor, the MMU gives each task its own numbering: the same virtual
address can mean different memory in two tasks. On a microcontroller there is one
flat numbering for everyone (Chapter 0.1), so a task's address space is not a
numbering of its own. It is the part of the one map that the MPU lets the task
reach. KickOS calls that set of memory a **domain**, on both kinds of chip, so a
later chapter that says "one per address space" means one per domain: per group of
threads that share that memory.

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
  on the instruction and the architecture, the attempt is either silently ignored
  or raises a **fault**: the core stops the program and enters the kernel at an
  address the kernel chose in advance (Chapter 0.3). Either way, it does not take
  effect.

User mode alone is not enough. A program that cannot touch control registers could
still overwrite the kernel's data with a store instruction. The second half of the
enforcement is the protection unit (MPU or MMU, Chapter 0.1): the kernel programs it
so that a user-mode access outside the program's own memory faults. The mode decides
*which instructions* run; the protection unit decides *which addresses* a user-mode
access may reach. Only privileged code can change either one.

Each architecture spells this differently. A Cortex-M core runs threads unprivileged
through the `CONTROL.nPRIV` bit, while interrupt handlers always run privileged.
RISC-V separates machine mode (M) from user mode (U), with a supervisor mode (S)
between them on larger cores. x86 has four levels, called rings 0 to 3: the kernel
runs in ring 0 and user code in ring 3, and most systems leave rings 1 and 2 unused.
The idea is the same in all of them: at least two levels, a privileged one for the
kernel and an unprivileged one for programs, and a transfer between them that the
less privileged side cannot make on its own terms. The hardware only provides the
levels; how much code an OS runs at the privileged one is its own design choice,
the subject of Chapter 0.4.

Some cores have no privilege modes at all. Then a kernel can still organise the
machine, but it cannot stop a buggy program from writing the kernel's memory.
Chapter 7.4, *[Privilege is three axes, not one bit](privilege-is-three-axes-not-one-bit.md)*,
takes this case apart.

## The system call: the only door

If user code cannot do privileged work, it needs a way to ask the kernel to do it.
It cannot simply call a kernel function. A call changes the program counter and
nothing else, so it would arrive in kernel code still unprivileged, and at whatever
address the caller chose.

The answer is a **trap instruction** (`SVC` on Arm, `ecall` on RISC-V, `INT` on Renesas RX):
an instruction whose only job is to enter the kernel on purpose, the way a fault
does by accident. Chapter 0.3, *[Interrupts and traps](interrupts-and-traps.md)*,
describes it. When user code executes it, the hardware switches to privileged mode
and jumps to an address the kernel set up in advance. The program chooses *that* it enters the kernel, never
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
protection regions, the address ranges the MPU lets it reach, cover its own code
and data, not USART2. A store from the app to
the data register at `0x40004404` (base `0x40004400`, data register at offset
`0x04`) faults. The app cannot own the UART unchecked, because the UART is shared.

So `printf` has to go through someone who may touch the UART, and it ends in a
syscall. newlib, the C library (Chapter 0.6), formats the text and calls `_write`,
and KickOS's `_write` hands the bytes to the kernel through the trap. What happens
on the trap's far side depends on how the OS is built. There are two answers:

1. **The kernel drives the UART itself.** It queues the bytes and writes them to
   the USART2 data register one at a time, each time the status register's
   transmit-empty bit says the register is free. Device drivers are part of the
   kernel, and run privileged with it. This is the **monolithic** answer.
2. **A separate program drives the UART.** A driver, running unprivileged like the
   app, is the only program whose protection regions include USART2. The kernel
   does not touch the UART: it carries the bytes from the app to the driver, and
   checks that the app is allowed to talk to that driver. This is the
   **microkernel** answer.

In both, the app never touches the UART, and every app's output passes through
one owner that alone sets the baud rate and orders the bytes. What differs is how
much code runs privileged to make that true: in the first answer a bug in the UART
driver can corrupt the kernel, in the second it can only break the driver.

KickOS is a microkernel and gives its drivers the second answer. It keeps one small
exception, the **kernel console**: a minimal output path from the kernel to the
UART, so the kernel can print before any driver runs, and so a program with no
driver behind its output still prints. The f411 example's default build has no
UART driver, so its `printf` ends there. Chapter 0.4 weighs the two answers.

## What a kernel must own

If the kernel is the privileged part, what must be inside it? A useful rule, which
Chapter 3.9 states for KickOS's own syscall list: *an operation belongs in the
kernel only if it needs something only the kernel has.* Four things qualify on any
machine.

**Scheduling.** Deciding which thread runs on the CPU, and switching between them,
means saving one thread's context and loading another's (Chapter 3.5,
*[Context switching and the silicon contract](context-switching-and-the-silicon-contract.md)*).
Only code that every program trusts may do this. Timers belong here too: "wake me in
10 ms" is a scheduling decision driven by a hardware timer.

**Memory protection.** Only privileged code can program the protection unit, and it
must be reprogrammed every time the running program changes, since each program has
its own allowed memory.

**Communication and synchronization (IPC, inter-process communication).** Once
programs are isolated, they need a safe way to cooperate: send a message, wait on a
semaphore, lock a mutex (the next section). Waiting means the scheduler must stop
running the waiter and resume it later, so blocking is kernel work.

**Interrupts.** The table that tells the core where each interrupt's handler starts,
and the interrupt controller that decides which signal the core takes, are privileged
hardware (Chapter 0.3), so the first instruction of every interrupt handler runs in
the kernel. The
kernel may handle the event itself, or forward it to an unprivileged driver.

Everything else, such as drivers, file systems, protocol stacks and console output,
*can* run outside the kernel. Whether it *should* is the monolithic vs microkernel
question of Chapter 0.4.

## Waiting and sharing

**Blocking.** A thread that cannot go on, because the byte, the timer or the other
thread it needs is not there yet, could loop testing a flag. That loop burns the core
that other threads need. Instead the thread **blocks**: it asks the kernel to stop
running it until the event happens. The kernel marks it blocked, runs another ready
thread, and makes it ready again when the event comes. A blocked thread costs no CPU
time at all.

**The mutex.** Two threads share Chapter 0.1's `counter`, and both call `tick`,
which adds 5 to it. `counter` starts at 0, so after both it should be 10. But
`tick` is a load, an add and a store, and each thread works in its own registers.
The kernel can stop thread A between its load and its store, when an interrupt
arrives, and run thread B; on a multicore chip, B may even run at the same moment on
another core:

| Step | Thread A | Thread B | `counter` |
|---|---|---|---|
| 1 | `ldr r3` loads 0 | | 0 |
| 2 | stopped, its `r3` = 0 saved | `ldr r3` loads 0 | 0 |
| 3 | | `adds`, `str` stores 5 | 5 |
| 4 | resumed: `adds` on its old 0, `str` stores 5 | | 5 |

B's update is lost. This is a **race**: the result depends on when the threads
happen to be switched. The cure is a **mutex** (mutual exclusion lock). A thread
**locks** it before touching `counter` and **unlocks** it after. Only one thread can
hold it; a second thread that tries to lock blocks until the holder unlocks. The code
between lock and unlock is a **critical section**: while one thread is inside it, no
other thread can enter a section guarded by the same lock. In KickOS:

```c
kos_mutex_lock(m);
counter = counter + 5;
kos_mutex_unlock(m);
```

Now step 2 cannot happen: B blocks in `kos_mutex_lock` until A has stored 5, then
loads 5 and stores 10.

**The semaphore.** A mutex protects data; a **semaphore** counts events. It holds a
number. **Post** adds one; **wait** takes one, or blocks while the number is 0. A
thread that handles received bytes waits on a semaphore, and each time a byte
arrives, the code that sees it posts. The thread sleeps while nothing comes, and no
byte is missed if two arrive before it runs, since the count remembers both. KickOS
spells them `kos_sem_wait` and `kos_sem_post`.

A third way to cooperate, the **message**, copies data from one thread to another;
Chapter 0.4 and Part 8 cover it. Chapter 2, *[The kernel model](kernel-model.md)*,
teaches how KickOS blocks and wakes threads, and Chapter 2.2,
*[The blocking substrate](the-blocking-substrate-one-wait-wake-primitive.md)*, the
one mechanism behind all of these.

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

- **Priority scheduling with preemption.** Each thread has a **priority**, a number
  saying how urgent it is. The most urgent ready thread runs at once: the kernel
  **preempts** the running thread, stopping it in the middle of its work, to run the
  urgent one first.
- **Short critical sections.** The kernel guards its own data by masking interrupts
  for a moment. While they are masked, an urgent event waits. Every masked section
  adds to the worst-case latency, so each must be short and of bounded length
  (Chapter 0.3).
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
**image**, the one linked binary flashed to the chip, and every thread runs privileged. Threads can block, and priorities give
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
is priority-based and preemptive, and **tickless**: it programs a one-shot timer, one
that fires once at a chosen time, for the next deadline, instead of waking on a
periodic tick. Drivers run unprivileged and receive their interrupts from the kernel
as events. Console output, files and protocols are userspace **servers**, programs
that do a job for others on request, reached by IPC. The kernel keeps one debug
console write, so that it can still report a **panic**, the kernel's stop-and-report
on an unrecoverable error, and print while the system is starting, before any
console server exists.

The cost of a trap per request is managed rather than avoided. The syscall list is
small, and when an operation is hot and safe, the kernel grants a program direct
access to the hardware instead of adding a syscall (Chapter 8.4,
*[The fast path is the capability](the-fast-path-is-the-capability-gpio-direct-mmio.md)*).
Where the hardware has no privilege modes, as on the ESP32's Xtensa LX6 core, the
syscall becomes a plain function call. The kernel still checks every request's
authority, but it cannot stop a thread that writes kernel memory directly.

## Where the KickOS kernel lives in the tree

The privileged part of KickOS is two directories.

**`kernel/`** is the kernel itself, in freestanding C++ (C++ without the standard
library's runtime, Chapter 0.6). It touches no hardware
register directly. Its subdirectories map onto the four jobs:

| Job | Where |
|---|---|
| Scheduling | `kernel/sched/` (ready queues and the scheduling policy, Chapter 2), `kernel/thread/`, `kernel/time/` (the time base and the sleep queue) |
| Memory protection | `kernel/domain/` (the regions a task may touch), `kernel/mem/`, `kernel/task/` |
| IPC and synchronization | `kernel/sync/` (semaphores, mutexes, the wait queue), message endpoints in `kernel/syscall/syscall_ipc.cc` (Chapter 8.3) |
| Interrupts | `kernel/irq/` (interrupts delivered as events), `kernel/notify/` |

The door is `kernel/syscall/`: `syscall_dispatch` in `kernel/syscall/syscall.cc`
routes each syscall number, and `kernel/syscall/cap.cc` holds the capability table,
each thread's list of the kernel objects it may use (Part 8), that every
object-naming syscall checks. Boot starts in `kmain`
(`kernel/init/kmain.cc`).

**`arch/`** is the hardware seam. `arch/include/kickos/arch/arch.h` lists every
operation the kernel may ask of the hardware, such as `arch_switch` (the context
switch), `arch_timer_arm` (the one-shot timer) and `arch_mpu_apply` (the protection
unit). Below it, each CPU family, ISA and chip supplies its own code, including the
trap entry in assembly.

Everything else is unprivileged. `user/` is the runtime every application links; its
`user/src/syscall_stubs.cc` holds the user side of the door: each `kos_*` call packs
its arguments and calls `arch_syscall`, which executes the trap. `system/` holds **init**
(`system/init/compose/`), the user program that starts every other task at
boot, and the user-space drivers in `system/driver/<chip>/`. The
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

- How the trap instruction and the vector table work: Chapter 0.3,
  *[Interrupts and traps](interrupts-and-traps.md)*.
- Where the kernel line falls, and who owns the f411 example's UART: Chapter 0.4,
  *[Monolithic vs microkernel](monolithic-vs-microkernel.md)*.
- Building the f411 example and flashing it: Chapter 0.5,
  *[The toolchain: from source to a running image](the-toolchain-from-source-to-a-running-image.md)*.
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
