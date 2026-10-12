<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# Capabilities and the object model: who may do what to which object

> A microkernel acts on objects for callers that cannot touch those objects
> themselves. This chapter introduces the questions every such kernel must
> answer: how a caller names an object, how the kernel decides whether the call
> is allowed, and where that authority comes from. It then shows how KickOS
> answers them with capabilities, authority bits, and the composition that grants
> both at start. The sub-chapters of Part 8 go deep into each piece. The exact
> contract is in `../reference/architecture.md` ("Object model, capabilities &
> IPC").
>
> *Further reading: Tanenbaum, Modern Operating Systems, ch.9 (protection domains,
> access control lists, capabilities) and ch.2 (interprocess communication,
> message passing).*

## The problem: a kernel acts for callers who cannot act

Chapter 0.4, *[Monolithic vs microkernel](monolithic-vs-microkernel.md)*, moved
the drivers out of the kernel and left a small core behind. That core owns a few
kinds of **kernel object**: a semaphore, a mutex, an endpoint two tasks talk
through, the binding of an interrupt line to a driver, a notification. An
unprivileged thread cannot reach any of them in memory. It can only make a system
call and ask the kernel to act for it. Every such call asks two questions:

1. **Which object does the caller mean?** It must pass a number, because it
   cannot pass a pointer into kernel memory.
2. **May the caller do this to that object?** Waiting on a semaphore and posting
   to it are different powers.

The simplest answer is a global number: semaphore 3 is semaphore 3 for everyone.
Then anyone who can guess the number can use the object. That is **ambient
authority**, power that comes from being there rather than from being given
anything, and it is the opposite of the isolation the microkernel was built for.
In a microkernel the questions are also on the hot path: every message between
two tasks is a system call that names an endpoint. The answer must be cheap, and
it must not be ambient.

## Two ways to answer: identity or possession

Write every subject (a thread or a task) down one side of a table and every
object across the top, and put in each cell what that subject may do to that
object. This **access matrix** is stored in one of two directions:

- **By column: access lists.** Each object lists who may use it. On a call, the
  kernel identifies the caller and searches the list. Authority comes from
  **identity**. Unix file permissions work this way.
- **By row: capabilities.** Each subject lists what it may use. A **capability**
  is one entry: a reference to an object plus the rights it carries. The caller
  names an entry in its own list, and the kernel checks that entry. Authority
  comes from **possession**.

The two differ in delegation, revocation, and what happens when a subject's slot
is reused. Chapter 8.0, *[Authority comes from possession, or from
identity](authority-from-possession-or-from-identity.md)*, works through each
difference. One kind of permission fits neither direction: a power that names no
object, such as "may allocate memory" or "may shut the system down". A system
needs a third mechanism for these.

## What KickOS chose: three kinds of power

KickOS uses possession: a capability check is a bounded lookup in the caller's
own table, cheap enough for every IPC call, and authority is passed explicitly. A
thread's power comes from three places, and only three.

**Object capabilities, in a per-thread table.** An entry names one kernel object,
its type, the rights this thread has over it, and a **generation**, a counter
that changes each time the slot is emptied. User code holds a **handle**: an
opaque 32-bit number that selects a slot in its own table. The same number names
different things in different threads. A thread cannot forge a capability,
because the table is kernel memory, and cannot borrow another thread's, because
the kernel only looks in the caller's own table. The rights are few:
`KOS_CAP_WAIT` (wait, or receive), `KOS_CAP_SIGNAL` (post, or send),
`KOS_CAP_TRANSFER` (may be passed on), and on an endpoint `KOS_CAP_HANDOUT`
(`system/include/kickos/sys/cap_rights.h`).

**The authority word, for acts that name no object.** Each thread carries a set
of bits in its control block: `KOS_AUTH_MEMORY` (reserve RAM), `KOS_AUTH_PINMUX`
(choose a pin's function), `KOS_AUTH_PSTATE` (set the CPU clock),
`KOS_AUTH_IRQ` (claim an interrupt line), `KOS_AUTH_SYSTEM` (shut down, or
reboot), `KOS_AUTH_CONSOLE` (hand the console to a driver), `KOS_AUTH_TASKS`
(create a task), and `KOS_AUTH_BUS_MASTER` (a window over a device that masters
the bus). The word names nothing, holds no reference and takes no table slot.
Chapter 7.4, *[Privilege is three axes, not one
bit](privilege-is-three-axes-not-one-bit.md)*, explains why it lives beside the
table and not in it.

**Memory and device windows, enforced by hardware.** A thread reaches the memory
and the device registers it was granted, and nothing else; the MPU or MMU checks
every load and store. Read, write and execute are therefore not capability
rights: they live in the protection hardware's descriptors, and a copy in the
capability would be a second truth. A grant is also **authenticated**: a task may
grant or share only RAM it reserved itself, and the kernel records which task
reserved each block (`kernel/include/kickos/ramown.h`). Chapter 3.7,
*[Peripheral isolation and the hardware
ceiling](peripheral-isolation-and-the-hardware-ceiling.md)*, covers what a chip
can and cannot enforce.

There are no global object ids and no privileged user. Root, the first thread,
runs unprivileged like the others; its power is a full authority word.

## The resolve: one place where every check happens

When a system call names an object, one function, `cap_resolve_e`
(`kernel/syscall/cap.cc`), turns the handle into the object. It checks that the
index is inside the caller's table, that the slot is live and its generation
matches the handle, that the type is the one the call needs, and that the rights
include the ones it needs. Then the object's pool checks the object's own
generation. The kernel holds its lock from the resolve through the use, so no
other thread can close the object in between. A dead, stale or mistyped handle
is refused with `-KOS_EBADF`, a live one without the right with `-KOS_EACCES`.

One chokepoint is one place to review. Chapter 8.1, *[Naming a kernel object: the
handle and the resolve chokepoint](handles-and-the-resolve-chokepoint.md)*,
builds it up, including why an interrupt handler never resolves a handle.
Chapter 8.7, *[A stale handle must not be
usable](stale-handles-generation-width-and-allocation-policy.md)*, asks what the
generation really guarantees. Chapter 8.2, *[Adding a kernel object
type](adding-a-kernel-object-type-the-additive-recipe.md)*, shows the object
side, and how a new type is added without touching the table.

## The life of a capability

A capability is created, passed down, narrowed and closed. No step adds a right
that was not already held.

- **Created.** A thread that creates an object (`kos_sem_create`,
  `kos_mutex_create`, `kos_endpoint_create`) receives a capability with every
  right. The object lives exactly as long as some capability names it.
- **Granted at spawn.** A spawn carries a list of source handles, each with a
  rights mask. The source must carry `KOS_CAP_TRANSFER`, and the child gets the
  parent's rights masked down, never more. The kernel checks the whole list
  before it creates anything. The grants land at known slots, from
  `KOS_SPAWN_DELEGATED_CAP0` up. The authority word passes down the same way: a
  thread may give a child only bits it holds.
- **Narrowed.** `kos_cap_narrow` drops rights from a capability in place. Given
  the pseudo-handle `KOS_CAP_AUTHORITY`, it drops bits from the caller's
  authority word.
- **Minted by the kernel.** When a client calls a server, the kernel places a
  one-shot **reply capability** in the server's table, naming the waiting client.
  When a console driver is published, the kernel seats a send-only copy of its
  endpoint in slot 0, `KOS_CAP_STDOUT`, of each later thread outside the
  driver's task.
- **Closed.** `kos_handle_close` empties the slot, changes its generation so the
  old handle stops working, and drops one reference; the last reference destroys
  the object. A thread that exits closes everything it holds.

The gaps are deliberate. Capabilities move only at spawn: no system call copies
one between two running threads, and a message carries bytes, not capabilities.
There is no revocation of a copy already given away. Chapter 8.0 weighs that
choice: the derivation tree that would buy revocation costs more on a small part
than the property is worth.

## Endpoints: the objects that connect tasks

Two tasks that share no memory talk through an **endpoint**, a meeting point. A
sender with `KOS_CAP_SIGNAL` sends, a receiver with `KOS_CAP_WAIT` receives, and
the kernel copies a bounded message from one thread straight into the other,
with no buffer of its own. Whichever side arrives first waits. Chapter 8.3,
*[Endpoints: synchronous IPC by
rendezvous](endpoints-synchronous-ipc-by-rendezvous.md)*, explains why KickOS
chose this rendezvous over a buffered channel.

A driver answers requests. `kos_call` sends and waits; the reply capability the
kernel mints for the server carries the answer back, and the waiting client
lends its priority to the server meanwhile. Chapter 8.5, *[Synchronous
call/reply: the reply capability](synchronous-call-and-reply.md)*, explains why
the waiting caller can itself be the reply object.

The endpoint capability is what makes a connection an act of authority: a client
can call a server only if it holds the server's endpoint with `KOS_CAP_SIGNAL`. A
published service is that capability for control, plus a shared-memory grant
where the data is large.

Not every authority needs a system call to use. A task that holds a device's
register window has had every store into it checked by the hardware already, so
a chip-select toggle inside an SPI transfer is a plain store: the granted window
is the capability. Chapter 8.4, *[The fast path is the
capability](the-fast-path-is-the-capability-gpio-direct-mmio.md)*, works through
that case and where the kernel path is still right.

## Authority words: acts with no object

Some acts reach beyond any one resource. Setting the CPU clock
(`kos_cpu_clock_set`) changes the speed of every thread. Choosing a pin's
function can take the pin from another driver. Shutting down ends every task.
Rebooting into the bootloader replaces the firmware itself. None of these names
an object a capability could point to, so each is a bit in the authority word,
and the kernel asks one question: does the caller hold the bit?

`KOS_AUTH_SYSTEM` is the clearest case. It gates `kos_shutdown` and `kos_reboot`,
the request to restart into the chip's firmware-download mode. Chapter 8.6,
*[Rebooting into the bootloader, and who may
ask](rebooting-into-the-bootloader-and-who-may-ask.md)*, separates the acts
hiding in "reboot", explains why an act keyed to no resource needs an explicit
authority, and shows how to test an act whose success leaves nothing running to
report it.

Like capabilities, authority bits only narrow. The kernel seats the full set on
root, every later thread gets a subset of its spawner's bits, and any thread can
drop its own.

## The composition: initial capabilities, declared once

Every capability must start somewhere. seL4 answers with an initial distribution
that a system describes in a manifest (CapDL). KickOS keeps the idea, a root
task that hands each task what it needs, and takes the burden off the
application writer.

Root runs the **init**, which reads the system's **composition**: a YAML file
listing the tasks, and for each its priority, its authority bits (`authority:`),
the device windows and interrupt lines it holds (`devices:`, `lines:`), the
endpoint it serves (`serves:`) and the endpoints it calls (`uses:`). The build
turns the file into a table, after checking it against what the chip can
enforce. The init creates the endpoints, keeps only the rights it needs on its
own copies, and spawns each task's first thread with exactly the grants its
entry lists. A task finds a granted endpoint by the name the composition gave it
(`kos_grant_endpoint`), not by a number.

A plain application writes no composition. Each board has a default one,
`boards/<board>/composition.yaml`, with a single task named `main`; the app
links `KickOS::system_default`, and the kernel seats the reserved slots on every
thread. A system that needs a driver task or a second app writes its own file.
Chapter 0.5, *[How to read this book, and the
toolchain](the-toolchain-from-source-to-a-running-image.md)*, shows where the
composition sits in the build.

## The f411 example: what the printf app holds

The f411 example is a `main` that calls `printf("hello\n")`, linked with
`KickOS::system_default` on `f411disco`. The default composition,
`boards/f411disco/composition.yaml`, says `stdout: kernel` and starts one task:

```yaml
tasks:
  - name: main
    entry: kickos_main
    stack: 4096
    priority: 2
    ceiling: 31
    authority: [memory, system, tasks]
```

So the thread that runs `main` holds:

- **Slot 0, `KOS_CAP_STDOUT`: empty.** No console driver was published.
- **Slot 1, `KOS_CAP_LIBC_LOCK`: a mutex with no rights**, the C library's lock
  for this address space, which `printf` and `malloc` take (Chapter 0.6,
  *[What's under #include](whats-under-include-libc-and-the-cxx-runtime.md)*).
  Locking it needs no right, and without `KOS_CAP_TRANSFER` it can never be
  passed on.
- **Every other slot: empty**, for objects the app creates itself.
- **The authority word: `memory`, `system`, `tasks`.** `hello` uses none of
  them; the board's default grants them so a plain `main` can reserve RAM, start
  tasks, and shut down without a manifest.
- **Windows over its code, its data and its stack.** None covers USART2's
  registers at `0x40004400`, so a store there faults.

Why may this app print at all? `printf` ends in `_write`, which calls
`stdout_write` (`user/include/kickos/sys/emit.h`). That function first sends the
bytes on slot 0. The slot is empty, the send answers `-KOS_EBADF`, and
`stdout_write` falls back to `kos_kconsole_write`, the kernel's debug console.
That one system call needs no capability and no authority bit: it is the single
sanctioned exception to the capability rule, because the kernel must print a
panic before any driver exists, and the app shares the same door. The kernel
checks that the buffer is the caller's to read and copies it in; the kernel, not
the app, stores into the USART2 data register.

Now give the app a console driver, as
`user/apps/common/selftest/consoles/f411disco/f4uartirq.yaml` does. Its
composition says `stdout: /svc/console`, and its `console` task holds
`devices: [/dev/usart2]` and the line-38 interrupt. When the init starts that
driver, the driver's endpoint is published as the console (`kos_console_publish`,
which needs `KOS_AUTH_CONSOLE`), and every later thread gets a send-only copy in
slot 0. The same `printf` now succeeds on its first send: the right to write the
console is a capability the app possesses. The source does not change, and the
app still holds no window on USART2. Only the driver does.

## Reading map

- **8.0** *[Authority comes from possession, or from
  identity](authority-from-possession-or-from-identity.md)*: capabilities against
  access lists, and why a microkernel's IPC path decides between them.
- **8.1** *[Naming a kernel object: the handle and the resolve
  chokepoint](handles-and-the-resolve-chokepoint.md)*: what a handle is, and how
  one function validates every use of one.
- **8.2** *[Adding a kernel object type: the additive
  recipe](adding-a-kernel-object-type-the-additive-recipe.md)*: how an object is
  created, passed, closed and torn down, so a new type touches nothing else.
- **8.3** *[Endpoints: synchronous IPC by
  rendezvous](endpoints-synchronous-ipc-by-rendezvous.md)*: how two tasks exchange
  a message with no buffer in the kernel.
- **8.4** *[The fast path is the
  capability](the-fast-path-is-the-capability-gpio-direct-mmio.md)*: why a
  chip-select toggle is a direct register store, not a system call.
- **8.5** *[Synchronous call/reply: the reply
  capability](synchronous-call-and-reply.md)*: how a request finds its answer,
  and why the call lends its priority.
- **8.6** *[Rebooting into the bootloader, and who may
  ask](rebooting-into-the-bootloader-and-who-may-ask.md)*: which acts "reboot"
  means, and why a systemic act needs an explicit authority.
- **8.7** *[A stale handle must not be
  usable](stale-handles-generation-width-and-allocation-policy.md)*: what a
  generation buys, and why the allocator's policy is part of the guard.

## Where to go next

- The code, from a capability entry to a spawn that grants one:
  [Journey 08, Spawn and capabilities](../journey/08-spawn-and-caps.md).
- One call and its reply through an endpoint:
  [Journey 05, Call and reply](../journey/05-call-and-reply.md).
- A whole system, from its composition to the bytes on an SPI bus:
  [Journey 18, A real system](../journey/18-a-real-system.md).
- The exact contract: `../reference/architecture.md` ("Object model,
  capabilities & IPC"), `../reference/ipc-call-reply.md` for call and reply, and
  `../reference/console.md` for the console's ownership.
