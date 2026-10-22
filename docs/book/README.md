<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# The KickOS Book

This Book teaches how an operating system kernel works. It uses KickOS as the example. Each
chapter explains a problem, the ways to solve it, and the choice KickOS made. The exact rules
are in the [Reference](../reference/README.md).

The Book is for a reader who knows some C or C++ and how a program is built and flashed onto a
board. For the full theory, read Andrew S. Tanenbaum's *Modern Operating Systems*.

Part 0 teaches the basic ideas. The later parts show how KickOS is built. Start with
[Chapter 0.0](how-to-read-this-book.md).

## Chapters

| # | Chapter | What you learn |
|---|---|---|
| 0.0 | [How to read this book](how-to-read-this-book.md) | Who the Book is for, how it fits with the Journey and the Reference, and the example it follows. |
| 0.1 | [The machine underneath: CPU, memory, peripherals](the-machine-underneath.md) | What a CPU, memory and peripherals are, and how to read a memory map. |
| 0.2 | [What is an OS / a kernel?](what-is-an-os-and-a-kernel.md) | What an operating system (OS) does, and which part of it is the kernel. |
| 0.3 | [Interrupts and traps](interrupts-and-traps.md) | How the CPU stops its work to handle an event, and then goes back to it. |
| 0.4 | [Monolithic vs microkernel](monolithic-vs-microkernel.md) | Two ways to build a kernel, and how much code each one must trust. |
| 0.5 | [The toolchain: from source to a running image](the-toolchain-from-source-to-a-running-image.md) | How source code becomes an image that runs on a board. |
| 0.6 | [What's under #include: the C library and the C++ runtime](whats-under-include-libc-and-the-cxx-runtime.md) | What the C library and the C++ runtime give a program, and what they need from the OS. |
| 1 | [Overview and principles](overview-and-principles.md) | What KickOS is for, and the principles every later chapter applies. |
| 2 | [The kernel model](kernel-model.md) | Threads, tasks, the scheduler, and how a thread waits and wakes. |
| 2.1 | [The time base: a monotonic clock you can trust](the-time-base-a-monotonic-clock-you-can-trust.md) | Why a kernel needs a clock that never runs backwards, how to get one, and how a program can read it without calling the kernel. |
| 2.2 | [The blocking substrate: one wait/wake primitive](the-blocking-substrate-one-wait-wake-primitive.md) | How one shared mechanism puts a thread to sleep and wakes it up, and how a semaphore in user memory waits on it. |
| 2.3 | [Priority inheritance: lending a thread its blocker's urgency](priority-inheritance-lending-urgency.md) | What priority inversion is, and how a thread that holds a lock borrows the priority of a thread that waits for it. |
| 2.4 | [An atomic buys definedness, not atomicity](an-atomic-buys-definedness-not-atomicity.md) | What `volatile` and atomic variables guarantee when threads share data, and what they do not. |
| 2.5 | [One kernel lock, many ready queues](one-lock-many-ready-queues.md) | How a multicore scheduler keeps the ready threads of each core in their own list, under one kernel lock. |
| 3 | [The interrupt model](interrupt-model.md) | How the kernel takes an interrupt, and how a driver outside the kernel serves it. |
| 3.5 | [Context switching and the silicon contract](context-switching-and-the-silicon-contract.md) | What the kernel saves when it switches threads, and why that code is written in assembly. |
| 3.6 | [Thread stacks and the KISS tension](thread-stacks-and-the-kiss-tension.md) | Who owns the memory of a thread's stack, and how the design stays simple. |
| 3.7 | [Peripheral isolation and the hardware ceiling](peripheral-isolation-and-the-hardware-ceiling.md) | Why the hardware decides how well the kernel can keep a driver away from devices it does not own. |
| 3.8 | [A masked interrupt is latched, not lost](a-masked-interrupt-is-latched-not-lost.md) | Why an interrupt that arrives while it is turned off must wait, and not be lost. |
| 3.9 | [The syscall path: trap, dispatch, return](the-syscall-path-trap-dispatch-return.md) | How a program calls the kernel, and how the kernel returns to it. |
| 4 | [One kernel, many ISAs](one-kernel-many-isas.md) | What an Instruction Set Architecture (ISA) means for a kernel, and how KickOS runs on several of them. A worked example: [porting to RISC-V](porting-a-new-isa-riscv.md). |
| 5 | [What a chip adds to its core](what-a-chip-adds.md) | What differs between two chips that use the same CPU core. |
| 6 | [Porting KickOS](porting-kickos.md) | How to bring KickOS to a new board, a new chip or a new ISA. |
| 7 | [Memory protection](memory-protection.md) | How the kernel keeps each program inside its own memory, on different kinds of hardware. |
| 7.1 | [Alignment across the syscall boundary](alignment-across-the-syscall-boundary.md) | Why the kernel checks the alignment of every pointer a program passes to it. |
| 7.2 | [Exceptions and RTTI under memory protection](exceptions-and-rtti-under-memory-protection.md) | How C++ exceptions and Run-Time Type Information (RTTI) work in a program with protected memory. |
| 7.3 | [Where your RAM goes: the full-C++ memory floor and the linker split](where-your-ram-goes-full-cxx-memory-floor-and-the-linker-split.md) | How much RAM full C++ needs, and how the linker places the kernel and the application in memory. |
| 7.4 | [Privilege is three axes, not one bit](privilege-is-three-axes-not-one-bit.md) | Why "privileged" covers three separate powers, and how KickOS splits them. |
| 7.5 | [Protection follows the CPU, not the scheduler's intent](protection-follows-the-cpu-not-the-schedulers-intent.md) | Why the kernel changes the memory protection at the moment the CPU switches threads. |
| 7.6 | [The CPU reads ahead: memory types and speculative access](memory-types-and-speculative-access.md) | How a CPU can read memory before a program asks, and why the memory map must mark the areas it may read early. |
| 7.7 | [Whoever stacks the trap frame owns the bounds check](whoever-stacks-the-trap-frame-owns-the-bounds-check.md) | Why the kernel checks a thread's stack before it saves registers there on a trap. |
| 8 | [Capabilities and the object model](capabilities.md) | How a program names a kernel object, and what it may do with it. |
| 8.0 | [Authority comes from possession, or from identity](authority-from-possession-or-from-identity.md) | Two ways to decide who may use an object: capabilities and access lists. |
| 8.1 | [Naming a kernel object: the handle and the resolve chokepoint](handles-and-the-resolve-chokepoint.md) | What a handle is, and how the kernel checks it on every use. |
| 8.2 | [Adding a kernel object type: the additive recipe](adding-a-kernel-object-type-the-additive-recipe.md) | The steps to add a new kind of kernel object. |
| 8.3 | [Endpoints: synchronous IPC by rendezvous](endpoints-synchronous-ipc-by-rendezvous.md) | How Inter-Process Communication (IPC) works when the sender and the receiver must meet. |
| 8.4 | [The fast path is the capability: why a chip-select toggle is direct MMIO, not a syscall](the-fast-path-is-the-capability-gpio-direct-mmio.md) | Why a driver toggles a pin with a direct write to memory-mapped I/O (MMIO), and not with a syscall. |
| 8.5 | [Synchronous call/reply: the reply capability](synchronous-call-and-reply.md) | How a client calls a server and waits for its answer. |
| 8.6 | [Rebooting into the bootloader, and who may ask](rebooting-into-the-bootloader-and-who-may-ask.md) | What "reboot" can mean, and which program may ask for it. |
| 8.7 | [A stale handle must not be usable: generations, width, and allocation policy](stale-handles-generation-width-and-allocation-policy.md) | How the kernel detects an old handle to an object that is gone. |
