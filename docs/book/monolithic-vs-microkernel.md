<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# Monolithic vs microkernel: how much code to trust

> Which parts of an operating system should run privileged?
>
> You will learn:
>
> - the main ways to split the work between the kernel and the rest
> - what each costs in safety and in speed
> - why a microkernel turns a function call into a message
> - who owns the f411 example's UART in KickOS.
>
> *Further reading: Tanenbaum, Modern Operating Systems, ch.1, section 1.7
> (operating system structure). Liedtke, On micro-kernel construction (SOSP 1995),
> for the minimality principle.*

## The problem: privileged code can do anything

Chapter 0.2, *[What is an OS, and what is a kernel?](what-is-an-os-and-a-kernel.md)*,
described the kernel as the privileged part of the operating system (OS).
Privileged code can program the protection unit, mask interrupts, and touch every
device and every byte of memory. Unprivileged code can touch only what privileged
code allowed it.

An OS contains a lot of code: a scheduler, memory management, device drivers, a
file system, a network stack, a console. The question this chapter answers is
**which of these pieces run privileged**.

It matters because nothing limits what a bug in privileged code can damage. A
stray pointer in an unprivileged program faults as soon as it points outside the
program's own memory, and the kernel can stop that program. A stray pointer in
privileged code writes wherever it points: another program's data, the scheduler's
queues, a device register. The fault, if any, comes later and somewhere else.

## The trusted computing base

The **trusted computing base (TCB)** is the code that must be correct for the
system's protection to hold. Privileged code is always in it. If a program's
isolation depends on a driver not writing over other memory, and that driver runs
privileged, the driver is in the TCB.

Its size matters because the number of bugs, and the work of review and proof, all
grow with the amount of code. Every line of the TCB is a line where a mistake breaks
isolation for everyone. A small TCB can be read in full and tested path by path,
and a large one cannot. **Formal verification**, a machine-checked proof that the
code does what its specification says, has so far been done only for small
kernels. seL4 was the first general-purpose kernel to have one: its proof covers
about ten thousand lines of C, and the proof itself is many times larger. So one
way to compare OS structures is to ask how large each makes its TCB.

## The structures: where the privilege line can go

Tanenbaum's section 1.7 surveys the main shapes.

**Monolithic.** The whole OS is one privileged program: scheduler, memory manager,
drivers, file systems, network stack. A service call is an ordinary function call
inside the kernel, which is fast and simple. The cost is the TCB: a bug in the
least-tested driver can corrupt the scheduler. Linux and the classic UNIX kernels
have this shape. Loadable modules split the code into files, but a module still
runs privileged, so it does not shrink the TCB.

**Layered.** Everything still runs privileged, but the code is organised as layers,
each using only the one below, as in Dijkstra's THE system. Layering helps a
designer reason about the code. But unless the hardware enforces the layers, as
MULTICS's protection rings did, the TCB stays the same.

**Microkernel.** Only the smallest possible core runs privileged. Drivers, file
systems and network stacks become ordinary unprivileged programs, called
**servers**, each in its own protected memory space. A
client reaches a server by **inter-process communication (IPC)**: it sends a
message, the kernel delivers it, and the server replies. The TCB shrinks to the
microkernel plus the servers a given program actually relies on. MINIX 3, QNX and
the L4 family, seL4 among them, have this shape.

**Hybrid.** A hybrid kernel starts from microkernel ideas but brings some services
back into the privileged part, for speed. Windows NT and the XNU kernel of macOS
are usually described this way. Some parts still run as servers outside the kernel. For instance,
Windows runs its Win32 environment as a user-mode server process, and macOS runs
its display server and many device drivers in user space. The line moves with
performance: Windows NT 4.0 moved its window manager and graphics code from that
server into the kernel to make them faster. So the TCB of a hybrid depends on what
it chooses to keep privileged, and it can sit anywhere between a microkernel's and
a monolithic kernel's. Windows and macOS keep a large share of their services
privileged, which puts theirs close to the monolithic end.

**Library OS.** The opposite direction: the OS is a library linked into the
application, and the image runs as one program. This is the "kernel as a library"
of [Chapter 0.2](what-is-an-os-and-a-kernel.md#why-a-kernel-still-matters-on-a-microcontroller),
and it is common on microcontrollers: many small real-time operating systems
(RTOSes) link the kernel, the drivers and the application into one image that runs
at one privilege level. It is simple and fast, and the TCB is
the whole firmware.

## The f411 example: who owns the UART

This section is about the hardware, the UART, and not about the console: in the
f411 example USART2 carries the console, so whoever drives USART2 also delivers
the console's text. Each structure above gives its own answer to who drives
USART2, at its own cost:

- **Monolithic.** The UART driver is part of the kernel. `printf` ends in a
  `write` syscall. Inside the kernel, the driver queues the bytes, stores each one
  into the data register at `0x40004404`, and refills it from its interrupt handler
  on line 38. Each write costs one trap. The driver runs privileged, so an overrun
  in its buffer can corrupt anything in the kernel.
- **Library OS, or a one-image RTOS.** There is no trap: `printf` calls the driver
  as a function, and the driver stores into `0x40004404` itself. It is the shortest
  path, and nothing stops another part of the firmware, such as the logger of
  [Chapter 0.2](what-is-an-os-and-a-kernel.md), from rewriting the baud-rate register at `0x40004408` in the middle
  of it.
- **Microkernel.** The UART belongs to a driver task. Only that task may touch
  USART2's registers, the 32 bytes from `0x40004400`, and the kernel wakes it when
  line 38 fires. `printf` ends in a message to that task, the kernel delivers it,
  and the driver stores the bytes. The kernel never touches USART2, and a bug in
  the driver kills the driver, not the app.

The microkernel answer raises a question of its own: if the kernel never touches the
UART, how does it print a panic, or anything before the driver has started? Every
microkernel keeps a small exception for this. KickOS's is described below.

## What a microkernel moves out, and what it must keep

The UART driver is one example of what a microkernel moves out. The same move works
for a file system (a server that answers "read this block") and a network stack
("send this packet").

Some things cannot move. Jochen Liedtke, who designed L4, stated the test as the
**minimality principle**: a feature is allowed inside the microkernel only if
moving it outside, where competing versions of it could exist, would make the
system unable to do its required job (Jochen Liedtke, *On micro-kernel
construction*, ACM Symposium on Operating Systems Principles, 1995). What must
stay in is roughly:

- **Protection domains.** Which memory each task may use, device registers
  included. These limits are set in the protection unit
  ([Chapter 0.1](the-machine-underneath.md)), and only privileged code can program it.
- **Threads and scheduling.** Choosing the next thread and switching to it needs
  privileged access to every thread's context.
- **IPC.** A message goes from one task's memory to another's, so the code that
  carries it must reach both, and only the kernel can.
- **Interrupt delivery.** On the cores KickOS supports, the hardware sends every
  interrupt to privileged code. The kernel's part is small: catch it and turn it
  into an event that wakes the driver.
- **Authority.** The decision of who may talk to whom, and who may use which
  memory, has to be enforced where it cannot be bypassed.

In practice, before a feature goes into the kernel, ask whether a user program
could do the same job. If it could, the feature belongs outside.

## The cost: every service call is IPC

In a monolithic kernel, once a program's system call has entered the kernel,
reaching the driver is a function call. In a microkernel it is a message to the
driver task, a switch into it, the driver's work, a reply, and a switch back. That
is two context switches and two more trips across the privilege line, where there
was one function call.

The first generation of microkernels, Mach above all, made this cost large enough
that many concluded the idea was impractical. Liedtke's L4 kernels of the 1990s
showed that the slowness came from how IPC had been built, not from the idea. L4
made IPC synchronous (no kernel buffering: the message goes straight from sender to
receiver), passed short messages in registers, avoided needless scheduler work on a
call, and dropped every feature that failed the minimality test. Its IPC was more
than an order of magnitude faster than Mach's on the same hardware. The cost did
not disappear, but it became small enough to manage: a design still decides which
interactions are worth a message.

How a system is cut into servers sets how many messages a request costs. For
instance, with the disk driver in one server and each file system (ext4, XFS) in
its own, reading a file takes two messages: program to file system, file system to
disk. Put the disk driver and the file systems in one server and a read takes one
message, but a bug in any file system can now crash the disk driver and every
other file system with it. Fewer messages means bigger pieces that fail together.
KickOS gives the same choice for each driver, as the section on what KickOS chose
explains below.

## Capabilities: who may talk to whom

With services as separate programs, a new question appears: who may send to the
UART driver? One answer is identity: the caller is user 1000, and a table says what
user 1000 may do. Many microkernels answer with **capabilities** instead. A
capability is a token that both names an object and grants rights on it. A program
cannot forge one: it can only receive it from a holder that passes it on. A client
may send to the UART driver only if it holds a capability for it. Holding it is the
permission. There is no global name that another program could guess.

Think of a guard at a door. With identity, you show your badge, and the guard looks
for your name on a guest list. The guard must keep that list, and must update it
every time someone may come in. With capabilities, you show a ticket with a seal
on it. The guard checks only that the seal is genuine, never who you are. Anyone
who holds the ticket gets in, and you can hand it to a friend, which is how
authority is passed on. Nobody can make a ticket without the seal, which is what
"cannot forge one" means.

seL4 builds the whole kernel on this idea: every kernel object is reached only
through a capability, and a program's authority is exactly the set it holds.
KickOS uses capabilities the same way for its kernel objects, with one exception:
a few rights cover the whole chip instead of one object, such as the right to set
the function of any pin. The section *[What KickOS takes from seL4, and
what it does not](#what-kickos-takes-from-sel4-and-what-it-does-not)*, below,
explains it.
Chapter 8.0, *[Authority comes from possession, or from
identity](authority-from-possession-or-from-identity.md)*, compares the two models.

*Further reading: Tanenbaum, Modern Operating Systems, ch.9 (protection domains,
access control lists and capabilities).*

## The RTOS angle

On an embedded controller, a microkernel also helps the system keep doing its job
when one part fails.

- **A driver fault does not take the system down.** If the UART driver dereferences
  a bad pointer, the kernel kills the driver's task. The control loop in another
  task keeps running, and a supervisor can restart the driver. MINIX 3 and QNX are
  both built on this, and MINIX 3 restarts a crashed driver automatically.
- **A short kernel keeps the worst case short.** Interrupt latency, the delay
  before a handler runs, depends on the longest kernel path that runs with
  interrupts masked. A kernel with fewer features usually has fewer long paths.
- **Isolation works without an MMU.** An MPU ([Chapter 0.1](the-machine-underneath.md)) does not translate
  addresses and has only a few regions. It can still limit each task to its own
  code, data, stack and device registers, which is enough to contain a driver. How
  finely it can protect a peripheral depends on where the peripheral sits on the
  bus (Chapter 3.7, *[Peripheral isolation and the hardware
  ceiling](peripheral-isolation-and-the-hardware-ceiling.md)*). On a part with no
  protection unit, the tasks and servers stay the same, but a stray access cannot
  be caught.

## What a task and its threads hold

In KickOS, a program may do only what it has been given, defined by four core
concepts:

- A **grant** is memory or a device a task is given: a range of addresses, with
  read, write or execute rights, that the protection unit lets its threads reach.
  The task's code, data and heap, and each thread's stack, are grants. A driver
  task also gets a grant on its device: the range of addresses where the device's
  registers sit, called its register window. An access outside every grant faults.
- An **endpoint** is a kernel object that carries messages between threads: a
  server thread waits on it to receive, and clients send to it. A service is
  reached through its endpoint.
- A **capability**, in KickOS, is a record the kernel keeps that says a thread may
  use one kernel object, such as an endpoint: which object, and which rights over
  it (send only, for example). Each thread has a small table of them, kept in
  kernel memory where the thread cannot write.
- A **handle** is the number a thread passes to the kernel to name an entry in its
  own table. The number grants nothing by itself: the kernel looks it up in the
  caller's table, so a guessed number reaches nothing the caller did not already
  hold.

Memory belongs to the task, and all its threads share it. Capabilities belong to
each thread, one by one.

### The f411 example: what the printf app holds

A **composition** is a file that describes the system: it lists the tasks to run,
and for each task, the memory and the capabilities it gets
([Chapter 0.5](the-toolchain-from-source-to-a-running-image.md)). The
board's default composition runs one task, named `main`, whose code calls
`printf("hello\n")`.

The task holds grants on its code, data, stack and an 8 KiB heap, and none on
USART2: a store from the app to `0x40004404` faults, and the kernel kills the task.

Slot 0 of its thread's capability table is kept for standard output. This
composition leaves standard output with the kernel, so the slot stays empty, and
`printf` ends in a syscall that asks the kernel to print. In this example, the
kernel owns the UART and writes the bytes itself. When a composition names a
console driver instead, the kernel fills slot 0 of every other task's threads with
a capability on that driver's endpoint. Holding it is the app's whole permission
to use the console, and the rights it carries decide what the app may do there:
send text to print, for instance.

Chapter 8, *[Capabilities and the object model](capabilities.md)*, covers
capabilities, handles and endpoints. Chapter 7, *[Memory
protection](memory-protection.md)*, covers grants.

## What KickOS chose, and why

KickOS is a microkernel. Its kernel keeps what Liedtke's test says must stay in:

- threads, tasks and the scheduler
- the time base
- the waiting half of the semaphore and of the priority-inheritance (PI) mutex
- endpoints and call/reply
- the capability table
- protection domains and grants
- the interrupt entry.

The PI mutex shows the test at work. Taking a free mutex is one atomic compare-and-swap
(CAS) on a word in user memory, so it stays out of the kernel. Lending a thread a higher
priority is a scheduler action that no user program can perform, so the kernel takes
part only when a thread waits (Chapter 2.3, *[Priority
inheritance](priority-inheritance-lending-urgency.md)*). A semaphore follows the same
test: a wait that finds a unit and a post that finds no waiter stay in user memory.
Apart from a few chip-wide settings that a thread with the right authority may ask for,
such as pin muxing, the only hardware the kernel drives itself is the system timer, the
interrupt controller, the protection unit, and a debug console.

Everything else lives in userspace. A device driver is an unprivileged task, granted
its device's register window as a memory region and a capability on its interrupt
line. When the line fires, the kernel's interrupt entry masks it and wakes the
driver's thread. The driver services the device as an ordinary thread, and the
kernel unmasks the line when the driver waits for the next event. A shared driver
runs as a **service**: a thread that receives requests on an endpoint and replies.
The same driver can instead be linked into a single client as a plain library, the
**class**, with no IPC at all. The application author chooses, and the kernel does
not change.

**The f411 example: two owners, one at a time.** KickOS uses two of the answers
above, one at a time. The kernel keeps a minimal debug console. On the f411 it
drives USART2 itself, through a transmit queue it empties from the interrupt on
line 38. This is the standard microkernel exception, and seL4 keeps a debug print
call for the same reason: a panic, a fault report and early boot must print before
any driver exists, and while a driver is dying. The full UART driver, which uses
interrupts, buffers its data and can also receive, is a userspace task. A system's
composition can give that driver USART2 and name it as standard output. From then
on the kernel stops touching USART2, until a panic or the driver's death takes it
back. A system can also give USART2 to no driver at all. The kernel then keeps it,
which is a small step toward a hybrid: the kernel owns one more device, and no
driver task runs for it.

`printf` does not need to know which owner is active. KickOS's `_write`, which the
C library calls, sends the bytes as a message on slot 0 of the thread's table, and
falls back to the kernel console only when no driver serves it. The application is
the same either way.

**Privileged threads.** Only one thread runs privileged: `idle`, the thread that
runs when nothing else is ready, one per core. Every other thread runs unprivileged, including the first program the kernel
starts. Chapter 7.4, *[Privilege is three axes, not one
bit](privilege-is-three-axes-not-one-bit.md)*, explains the rest of the privilege
model.

**The IPC cost.** KickOS keeps it low the way L4 did. Endpoints are synchronous and
unbuffered ([Chapter 8.3](endpoints-synchronous-ipc-by-rendezvous.md)). On several
architectures, a short call to a server that is already waiting takes a shorter
path through the kernel ([Chapter 8.5](synchronous-call-and-reply.md) gives the
conditions). An operation that is done very often and needs only one register
store, such as setting a general-purpose I/O (GPIO) pin, is not a call at all. The
task is given a grant on the GPIO register window, so it writes the register
itself. No call is needed to check the write: the protection unit already lets
this task, and only this task, reach those registers (Chapter 8.4, *[The fast path
is the capability](the-fast-path-is-the-capability-gpio-direct-mmio.md)*).

## What KickOS takes from seL4, and what it does not

KickOS follows seL4's design principles:

- a kernel small by design
- authority by capability, through a per-thread table of typed handles that carry
  rights ([Chapter 8.1](handles-and-the-resolve-chokepoint.md))
- drivers, and any file system or network stack, as unprivileged servers reached by
  IPC
- synchronous endpoints with a one-shot **reply capability** (Chapters 8.3 and 8.5)
- each thread handed, at start, the capabilities it needs.

It deliberately does not take seL4's verification machinery: **no formal proof**.
The goal is to keep KickOS simple to use: an application author writes a `main`,
and the system's composition gives each task's threads a reasonable default set of
capabilities. Without a proof, the guarantees rest on a small kernel, review, and
tests.

It also leaves out several seL4 mechanisms that a small static RTOS does not need.
[Chapter 8.0](authority-from-possession-or-from-identity.md) says which, and why. A few rights cover the whole chip instead of
one object, such as pin muxing (choosing which peripheral drives a pin) or claiming
an interrupt line. They are granted as **authority bits**, one bit per right. A thread with the
pin-muxing bit may change any pin.

## Summary

| Question | Monolithic | Layered | Microkernel | Hybrid | Library OS |
|---|---|---|---|---|---|
| What runs privileged? | everything | everything, in layers | a small core | the core plus chosen services | kernel and application together |
| How does an application reach a service? | a syscall, then a function call in the kernel | a syscall, then a function call in the kernel | a syscall that sends IPC to a server | a syscall, then a function call or IPC | a function call |
| What is in the TCB? | the kernel, all drivers, all services | the kernel, all drivers, all services | the kernel, and the servers in use | the kernel, and the services kept inside it | all the code in the image |
| Typical examples | Linux, FreeBSD, NuttX (protected build), Zephyr (user mode on) | THE | MINIX 3, QNX, the L4 family (seL4) | Windows NT, XNU | FreeRTOS, ThreadX, NuttX (flat build), Zephyr (default) |

## Where to go next

- What a kernel is for, and why `printf` needs one: Chapter 0.2, *[What is an OS,
  and what is a kernel?](what-is-an-os-and-a-kernel.md)*, the primer this chapter
  builds on.
- Building the f411 example and flashing it: Chapter 0.5, *[The toolchain: from
  source to a running image](the-toolchain-from-source-to-a-running-image.md)*.
- Authority by possession: Chapter 8.0, *[Authority comes from possession, or from
  identity](authority-from-possession-or-from-identity.md)*, then Chapter 8.1,
  *[Naming a kernel object: the handle and the resolve
  chokepoint](handles-and-the-resolve-chokepoint.md)*.
- IPC, the microkernel's central primitive: Chapter 8.3, *[Endpoints: synchronous
  IPC by rendezvous](endpoints-synchronous-ipc-by-rendezvous.md)*, and Chapter 8.5,
  *[Synchronous call/reply: the reply capability](synchronous-call-and-reply.md)*.
- The exact contracts, in the Reference: the [KickOS
  architecture](../reference/architecture.md), the [kernel
  console](../reference/console.md), and [synchronous call/reply
  IPC](../reference/ipc-call-reply.md).
- The Journey follows these paths through the real code:
  [Call and reply](../journey/05-call-and-reply.md),
  [an interrupt to a driver](../journey/07-interrupt-to-a-driver.md),
  [fault and kill](../journey/09-fault-and-kill.md) and
  [a real system](../journey/18-a-real-system.md).
