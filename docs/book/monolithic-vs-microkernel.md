<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# Monolithic vs microkernel: how much code to trust

> Which parts of an operating system should run privileged, and which should
> not? This chapter compares the answers and what each costs in safety and in
> speed. After it you can say whether a bug in a driver can bring the whole
> system down, why a microkernel turns a function call into a message, and who
> owns the f411 example's UART in KickOS.
>
> *Further reading: Tanenbaum, Modern Operating Systems, ch.1, section 1.7
> (operating system structure).*

## The problem: privileged code can do anything

Chapter 0.1 introduced the privilege bit, and Chapter 0.2, *[What is an OS, and
what is a kernel?](what-is-an-os-and-a-kernel.md)*, the kernel as the privileged
part of the OS. Privileged code can program the
protection unit, mask interrupts, and touch every device and every byte of memory.
Unprivileged code can touch only what privileged code allowed it.

An operating system contains a lot of code: a scheduler, memory management, device
drivers, a file system, a network stack, a console. The structural question is
**which of these pieces run privileged**.

It matters because a bug in privileged code has no fence around it. A stray pointer
in an unprivileged program faults, and the kernel can kill that program. A stray
pointer in privileged code writes wherever it points: another program's data, the
scheduler's queues, a device register. The fault, if any, comes later and somewhere
else.

## The trusted computing base

The **trusted computing base (TCB)** is the code that must be correct for the
system's protection to hold. Privileged code is always in it. If a program's
isolation depends on a driver not scribbling over memory, and that driver runs
privileged, the driver is in the TCB.

Its size matters because bugs, review and proof all scale with code. Every line of
the TCB is a line where a mistake breaks isolation for everyone. A small TCB can be
read in full and tested path by path; a large one cannot. Formal verification is
practical only for a small kernel: seL4's proof covers about ten thousand lines of C,
and the proof itself is many times larger. So one way to compare OS structures is to
ask how large each makes its TCB.

## The structures: where the privilege line can go

Tanenbaum's section 1.7 surveys the main shapes.

**Monolithic.** The whole OS is one privileged program: scheduler, memory manager,
drivers, file systems, network stack. A service call is an ordinary function call
inside the kernel, which is fast and simple. The cost is the TCB: a bug in the
least-tested driver can corrupt the scheduler. Linux and the classic UNIX kernels
have this shape. Loadable modules split the code into files, but a module still
runs privileged, so it does not shrink the TCB.

**Layered.** Still all privileged, but organised as layers, each using only the one
below, as in Dijkstra's THE system. Layering helps a designer reason, but unless the
hardware enforces the layers, as MULTICS's protection rings did, the TCB stays the
same.

**Microkernel.** Only the smallest possible core runs privileged. Drivers, file
systems and network stacks become ordinary unprivileged programs, called
**servers**, each running as its own **process**: a running program with its own
protected memory, which KickOS calls a task (Chapter 0.2). A client reaches a
server by **inter-process communication (IPC)**: it
sends a message, the kernel delivers it, and the server replies. The TCB shrinks to
the microkernel plus the servers a given program actually relies on. MINIX 3, QNX,
the L4 family and seL4 have this shape.

**Hybrid.** Kernels that began from microkernel ideas but run most services
privileged for speed, as Windows NT and macOS's XNU are usually described. Their TCB
is close to a monolithic one.

**Library OS.** The opposite direction: the OS is a library linked into the
application, and the image runs as one program. On a microcontroller this shape
appears naturally: many small RTOSes link the kernel, the drivers and the
application into one image at one privilege level. It is simple
and fast, and the TCB is the whole firmware.

| Structure | What runs privileged | Service call | TCB |
|---|---|---|---|
| Monolithic | everything | function call | whole kernel |
| Layered | everything, in layers | function call | whole kernel |
| Microkernel | a small core | IPC to a server | core + the servers used |
| Hybrid | most services | mostly function calls | close to monolithic |
| Library OS | kernel and application together | function call | whole image |

## The f411 example: who owns the UART

Chapter 0.2 followed `printf("hello\n")` on the STM32F411 to USART2 through a
syscall, and ended on a question: who should own the UART? Each structure answers it
differently.

- **Monolithic.** The UART driver is part of the kernel. `printf` ends in a `write`
  syscall; inside the kernel, the driver queues the bytes and stores each one into
  the data register at `0x40004404`, refilling it from its handler on interrupt line
  38. One trap per write. The driver runs privileged, so an overrun in its buffer can
  corrupt anything in the kernel.
- **Library OS, or a one-image RTOS.** No trap: `printf` calls the driver as a
  function, and the driver stores into `0x40004404` itself. It is the shortest path,
  and nothing stops the logger from rewriting the baud-rate register at `0x40004408`
  in the middle of it.
- **Microkernel.** The UART belongs to a driver task, which alone may touch
  USART2's 32-byte register window at `0x40004400` and is woken by line 38.
  `printf` ends in a message to that task; the kernel delivers it, and the driver
  stores the bytes. The kernel never touches USART2, and a bug in the driver kills
  the driver, not the app.

The microkernel answer raises a question of its own: if the kernel never touches the
UART, how does it print a panic, or anything before the driver has started? Every
microkernel keeps a small exception for this. KickOS's is below.

## What a microkernel moves out, and what it must keep

A driver moved out of the kernel is a program like any other: unprivileged, with
its own device registers and memory and nothing else, killed like any program if it
faults. The same move works for a file system (a server that answers "read this
block") and a network stack ("send this packet").

Some things cannot move. Jochen Liedtke, who designed L4, stated the test as the
**minimality principle**: a concept is tolerated inside the microkernel only if
moving it outside the kernel, permitting competing implementations, would prevent
the implementation of the system's required functionality. What fails the test, and
so stays in, is roughly:

- **Protection domains.** Only privileged code can program the MPU or MMU, so only
  the kernel can draw the walls.
- **Threads and scheduling.** Choosing the next thread and switching to it needs
  privileged access to every thread's context.
- **IPC.** The mechanism that crosses a wall cannot live on one side of it.
- **Interrupt delivery.** The hardware vectors an interrupt into privileged code.
  The kernel's part is small: catch it and turn it into an event that wakes the
  driver.
- **Authority.** The decision of who may talk to whom, and who may use which
  memory, has to be enforced where it cannot be bypassed.

The test is a design discipline as much as a definition: before a feature goes into
the kernel, ask whether a user program could do the same job. If it could, the
feature belongs outside.

## The cost: every service call is IPC

In a monolithic kernel, reading a sensor through a driver is a function call. In a
microkernel it is a message to the driver task, a switch into it, the driver's work,
a reply, and a switch back: two privilege crossings and two context switches where
there was one call.

The first generation of microkernels, Mach above all, made this cost large enough
that many concluded the idea was impractical. Liedtke's L4 kernels of the 1990s
showed that the slowness came from how IPC had been built, not from the idea. L4
made IPC synchronous (no kernel buffering: the message goes straight from sender to
receiver), passed short messages in registers, avoided needless scheduler work on a
call, and dropped every feature that failed the minimality test. Its IPC was more
than an order of magnitude faster than Mach's on the same hardware. The cost became
a matter of engineering, though not zero: a design still decides which interactions
deserve a message.

## Capabilities: who may talk to whom

With services as separate programs, a new question appears: who may send to the
UART driver? One answer is identity: the caller is user 1000, and a table says what
user 1000 may do. Many microkernels answer with **capabilities**: an unforgeable
token that both names an object and grants rights on it. A client may send to the
UART driver only if it holds a capability for it. Holding it is the permission;
there is no global name to guess.

seL4 builds the whole kernel on this idea: every kernel object is reached only
through a capability, and a program's authority is exactly the set it holds. seL4 is
also the first general-purpose kernel with a machine-checked proof that its C
implementation matches its specification.
Chapter 8.0, *[Authority comes from possession, or from
identity](authority-from-possession-or-from-identity.md)*, compares the two models.

*Further reading: Tanenbaum, Modern Operating Systems, ch.9 (protection domains,
access control lists and capabilities).*

## The RTOS angle

On an embedded controller the microkernel argument is also about keeping the job
running.

- **A driver fault does not take the system down.** If the UART driver dereferences
  a bad pointer, the kernel kills the driver's task. The control loop in another
  task keeps running, and a supervisor can restart the driver. MINIX 3 and QNX both
  make this argument.
- **A short kernel keeps the worst case short.** Interrupt latency is bounded by
  the kernel paths that run with interrupts masked; fewer kernel features, fewer
  long paths.
- **Isolation works without an MMU.** An MPU (Chapter 0.1) has no translation and
  few regions, but can fence each task to its own code, data, stack and device
  registers, which is enough to contain a driver. How finely it can fence a
  peripheral depends on where it sits on the bus (Chapter 3.7, *[Peripheral
  isolation and the hardware
  ceiling](peripheral-isolation-and-the-hardware-ceiling.md)*). On a part with no
  protection unit, the structure stays but a stray write is no longer caught.

## What a task and its threads hold

In KickOS, what a program may do is a short list of things it holds. Four words
name them, and the rest of the Book uses them constantly. A task holds the memory
its threads share; each thread holds its own capabilities.

- A **grant** is memory or a device a task is given: a range of addresses, with
  read, write or execute rights, that the protection unit lets its threads reach.
  The task's code, data and heap, and each thread's stack, are grants; so is a
  device's register window, given to the driver task that owns the device. An
  access outside every grant faults.
- An **endpoint** is a kernel object that carries messages between threads: a
  server thread waits on it to receive, and clients send to it. A service is
  reached through its endpoint.
- A **capability** is the kernel's record that a thread may use one kernel object,
  such as an endpoint: which object, and which rights over it (send only, for
  example). Each thread has a small table of them, kept in kernel memory where the
  thread cannot write.
- A **handle** is the number a thread passes to the kernel to name an entry in its
  own table. The number grants nothing by itself: the kernel looks it up in the
  caller's table, so a guessed number reaches nothing the caller did not already
  hold.

**The f411 example: what the printf app holds.** A **composition** is the file that
says which tasks a system runs and what each is given (Chapter 0.5). The board's
default one, `boards/f411disco/composition.yaml`, runs one task, `main`, whose `main`
function calls `printf("hello\n")`. The task holds grants on its code, data, stack
and an 8 KiB heap, and none on USART2: a store from the app to `0x40004404` faults,
and the kernel kills the task. Slot 0 of its thread's table, `KOS_CAP_STDOUT`, is
reserved for standard output. This composition leaves standard output with the
kernel, so the slot stays empty and `printf` ends in a syscall that asks the kernel
to print; the kernel owns the UART here and writes the bytes itself. When a
composition names a console driver instead, the kernel fills slot 0 of every thread
it starts with a send-only capability on that driver's endpoint. Holding it is the
app's whole permission to write the console, and send is the only right it carries.

Part 8 builds each piece: Chapter 8, *[Capabilities and the object
model](capabilities.md)*, the whole model; Chapter 8.1 the handle and how the kernel
checks it; Chapter 8.3 the endpoint. Grants are Chapter 7's subject, *[Memory
protection](memory-protection.md)*.

## What KickOS chose, and why

KickOS is a microkernel. Its kernel keeps what fails Liedtke's test: threads, tasks
and the scheduler (`kernel/thread/`, `kernel/task/`, `kernel/sched/`); the time base
(`kernel/time/`); semaphores and priority-inheritance mutexes (`kernel/sync/`);
endpoints and call/reply (`kernel/syscall/syscall_ipc.cc`); the capability table
(`kernel/syscall/cap.cc`); protection domains and grants (`kernel/domain/`,
`kernel/grant/`); and the interrupt entry (`kernel/irq/irq.cc`). The PI mutex shows
the test at work: lending a thread a higher priority is a scheduler action that no
user program can perform (Chapter 2.3, *[Priority
inheritance](priority-inheritance-lending-urgency.md)*). The only hardware the kernel
drives itself is the system timer, the interrupt controller, the protection unit, and
a debug console.

Everything else lives in userspace. A device driver is an unprivileged task, granted
its device's register block as a memory region and a capability on its interrupt
line. The kernel's interrupt entry masks the line and wakes the driver's thread; the
driver services the device as an ordinary thread, then unmasks the line. A shared
driver is a **service**: a thread that receives requests on an endpoint and replies.
The same driver can instead be linked into a single client as a plain library, the
**class**, with no IPC at all. The consumer chooses; the kernel does not change. The
reasoning is in `../reference/architecture.md` ("Drivers & interrupts", "Driver
packaging: class versus service").

**The f411 example: two owners, one at a time.** KickOS uses two of the answers
above, in turn. The kernel keeps a minimal debug console: on the f411 it drives
USART2 itself, through a transmit queue it drains on line 38, and the C library
reaches it through the `kos_kconsole_write` syscall. This is the standard microkernel
exception (seL4 has `seL4_DebugPutChar` for the same reason): a panic, a fault report
and early boot must print before any driver exists, and while one is dying. The full
UART driver, interrupt-driven and buffered, with receive, is a userspace task,
`f4uartirq` (`system/driver/stm32f411/f4uartirq/`). A system's composition can hand
it USART2 and name it as standard output, as
`user/apps/common/selftest/consoles/f411disco/f4uartirq.yaml` does with
`devices: [/dev/usart2]` (the composition's name for USART2) and
`stdout: /svc/console`. From then on the kernel stops touching USART2, until a panic
or the driver's death takes it back.

`printf` finds the owner by itself. newlib's `_write` calls `stdout_write`
(`user/include/kickos/sys/emit.h`), which sends the bytes as a message on the
thread's stdout capability (`KOS_CAP_STDOUT`) and falls back to the kernel console
only when no driver serves it. The application is the same either way.
`../reference/console.md` has the exact ownership rules.

**Privileged threads.** A few threads run privileged, such as `idle`, the thread that
runs when nothing else is ready. Each is a hole in the microkernel claim, so the set
is fixed at boot and kept small; Chapter 7.4, *[Privilege is three axes, not one
bit](privilege-is-three-axes-not-one-bit.md)*, says which are required.

**The IPC cost.** KickOS pays it the way the L4 family taught. Endpoints are
synchronous and unbuffered (Chapter 8.3), and on several architectures a small call
to a server already waiting completes inside the trap itself (Chapter 8.5 gives the
conditions). A hot path that needs only a register store, such as setting a GPIO pin,
is not a call at all: the task holds the register window, and holding it is the
authority (Chapter 8.4, *[The fast path is the
capability](the-fast-path-is-the-capability-gpio-direct-mmio.md)*).

## What KickOS takes from seL4, and what it does not

KickOS follows seL4's design principles: a kernel small by design; authority by
capability, through a per-thread table of typed, rights-bearing handles (Chapter
8.1); drivers, and any file system or network stack, as unprivileged servers reached
by IPC; synchronous endpoints with a one-shot **reply capability** (Chapters 8.3 and
8.5); and each task handed, at start, the capabilities it needs.

It deliberately does not take seL4's verification machinery: **no formal proof**.
The goal is a low barrier: an application author writes a `main`, and the system's
composition gives each task a sane default set of capabilities. Without a proof,
the guarantees rest on a small kernel, review, and tests.

It also leaves out several seL4 mechanisms that a small static RTOS does not need;
Chapter 8.0 says which, and why. Powers that name no single object, such as pin
muxing (choosing which peripheral drives a pin) or claiming an interrupt line, are
granted as **authority bits**, one per kind of power (`KOS_AUTH_PINMUX`,
`KOS_AUTH_IRQ`, and others).
`../reference/architecture.md` ("Non-goals -- seL4 machinery deliberately NOT
adopted") gives the arithmetic behind each refusal.

## How it is done in the code

The journey walks these paths through the real source:

- [5. Call and reply](../journey/05-call-and-reply.md): one synchronous call from a
  client to a server and back, with the reply capability, the lent priority and the
  register fast path.
- [7. An interrupt to a driver in user space](../journey/07-interrupt-to-a-driver.md):
  the kernel's whole share of a driver, from the vector to the unprivileged thread
  that services the device and unmasks the line.
- [9. Fault and kill](../journey/09-fault-and-kill.md): an unprivileged thread
  faults, the kernel kills its task, and another thread runs on.
- [18. A real system](../journey/18-a-real-system.md): a bus driver service and its
  client, including what the client sees when the driver dies, and the same client
  built to own the bus controller itself.

The fault-containment rule is `../reference/porting.md` ("Fault-isolation
contract"); the call/reply contract, including what each side sees when the other
dies, is `../reference/ipc-call-reply.md`.

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
- The exact contract: `../reference/architecture.md` ("North star", "Non-goals",
  "Drivers & interrupts", "Object model, capabilities & IPC").
