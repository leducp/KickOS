<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# Overview and principles: what KickOS is for

> What is KickOS for, and what rules does every part of it follow? Part 0 taught
> the general ideas: the machine, what an operating system and a kernel are,
> interrupts and traps, the microkernel structure, the toolchain, and what lies
> under `#include`. This chapter says what KickOS sets out to be and names the
> principles every later chapter applies, each with its reason. Afterwards you can
> say why KickOS is built the way it is, and find where the Book and the Journey
> work out each principle.

*Further reading: Tanenbaum, Modern Operating Systems, ch.1 (section 1.4, the
operating system zoo, from embedded and sensor-node systems to servers; section
1.7, operating system structure).*

## What KickOS is

KickOS is a small microkernel real-time operating system. Its trusted kernel holds
only what cannot live anywhere else: the scheduler, IPC and synchronization,
tickless time, the interrupt plumbing, and memory protection. Everything else, from
device drivers to the console server, runs in userspace as ordinary unprivileged
tasks. Authority is carried by capabilities, and isolation is enforced by the
hardware's protection unit.

It has four aims:

1. **Whole scale.** From a microcontroller with an MPU to an application processor
   with an MMU, from a small single-core part to a multicore one.
2. **Safe and secure.** A microkernel plus capabilities.
3. **Multi-architecture.** Many CPU families, one design.
4. **Teaching material.** A kernel a newcomer can learn from, with documents written
   for that.

Two more principles follow from the first three: the barrier to entry must stay
low, and the time behaviour must stay deterministic. The sections below take them
one at a time.

## Whole scale: one address-space abstraction over two kinds of hardware

**Why.** Embedded systems span a wide range. A sensor node may have 20 KiB of
SRAM and a Cortex-M3; a controller in a robot may have a quad-core Cortex-A53 and
gigabytes of DRAM. Chapter 0.1, *[The machine underneath](the-machine-underneath.md)*,
showed the split that matters to a kernel: a microcontroller usually has an **MPU**,
which checks each access against a few address ranges and translates nothing, while
an application processor has an **MMU**, which translates every address through page
tables.

**How one could.** The usual answer is two systems: a small RTOS for the
microcontrollers and a general-purpose OS for the application processors, with
different APIs, different isolation models and different bugs. Another answer is to
target only one class, and leave the other to someone else.

**What KickOS chose.** One kernel, with **one address-space abstraction over two
kinds of protection hardware**. The unit of isolation is the **memory domain**. On a
region MPU, a domain is a set of regions, and every domain lives in one physical
address space. On an MMU, a domain owns its own page tables, and the boundary is a
virtual address space. The kernel above the arch layer makes the same calls on both;
only the arch layer knows how to program the hardware. A chip declares which kind it
has in its chip file, a region unit or an MMU, never both.

**The f411 example, at the other end of the scale.** Chapter 0.5 built an
application whose `main` calls `printf("hello\n")` and ran it on the `f411disco`
board. There, the task's code, data and stack are MPU regions, and the text leaves
through USART2. Install the package for `qemu-arm64` instead, a Cortex-A53 with an
MMU, and the same `main.c` and `CMakeLists.txt` build against it unchanged. The task
now runs at EL0, the unprivileged level of Arm application cores, in its own
address space, and its regions have become page-table mappings. The program cannot
tell. (The same plain C program lives in the tree as
`user/apps/common/sysdefault/`.) A whole system scales the same way:
`examples/composition/` describes one set of tasks for `xmc4800-relax`, an MPU board,
and for `qemu-arm64`, a four-core MMU board, in two system files under
`examples/composition/systems/`.

The same reach applies to cores: one kernel can schedule several cores that share
memory (SMP), or several independent kernels can each own a core (AMP).

**Where it is developed.** Chapter 3.7, *[Peripheral isolation and the hardware
ceiling](peripheral-isolation-and-the-hardware-ceiling.md)*; Chapter 7, memory
protection, and its deep dives, in particular 7.5, *[Protection follows the
CPU](protection-follows-the-cpu-not-the-schedulers-intent.md)*; Chapter 2.5, *[One
kernel lock, many ready queues](one-lock-many-ready-queues.md)*. In the Journey:
[10. The same walk with an MMU](../journey/10-the-mmu.md) and
[11. Multicore](../journey/11-multicore.md). The contract is
`../reference/architecture.md` ("Memory domains").

## Safe and secure: a small trusted kernel and authority by capability

**Why.** Chapter 0.4, *[Monolithic vs microkernel](monolithic-vs-microkernel.md)*,
showed that every line of privileged code is part of the trusted computing base, and
that a bug anywhere in it breaks isolation for everyone. On an embedded controller
the cost is concrete: a fault in a sensor driver must not stop the control loop.

**What KickOS chose.** Four rules, each a consequence of that argument.

- **The kernel is small.** It keeps only what fails Liedtke's minimality test, and a
  new feature goes outside the kernel if a user program could do the job.
- **Drivers are unprivileged tasks.** A driver is granted its device's register
  window and a capability on its interrupt line, and nothing else. When it faults,
  the kernel kills its task, and the rest of the system runs on.
- **Authority comes from possession.** A thread reaches a kernel object only through
  a capability, an entry in its own capability table with a type and rights, and
  names it by a handle, the number a thread uses to name a capability-table entry.
  There are no global ids to guess. A few powers name no single object, such as
  muxing a pin; those are **authority bits**, one per kind of act that covers every
  object of that kind, such as `KOS_AUTH_PINMUX` for muxing any pin.
- **Isolation is as strong as the hardware permits, and a gap is stated, never
  hidden.** A part with a fine-grained protection unit gets fine-grained isolation;
  a part with none, such as the STM32F103 or the ESP32's Xtensa core, still runs the
  same kernel, with the protection it cannot enforce made explicit. Where the
  hardware cannot give what a configuration asks, KickOS **refuses rather than
  silently masks**: a build that asks for enforcement on a chip whose file declares
  no protection unit fails at configure time, instead of producing an image that only
  looks protected. Chapter 3.7 applies the same rule to peripherals, stating the
  isolation ceiling per chip rather than lowering it for the whole fleet.

**Where it is developed.** Chapter 7.4, *[Privilege is three axes, not one
bit](privilege-is-three-axes-not-one-bit.md)*; Chapter 8.0, *[Authority comes from
possession, or from identity](authority-from-possession-or-from-identity.md)*;
Chapter 8.1, *[The handle and the resolve
chokepoint](handles-and-the-resolve-chokepoint.md)*. In the Journey:
[7. An interrupt to a driver](../journey/07-interrupt-to-a-driver.md),
[8. Spawn and capabilities](../journey/08-spawn-and-caps.md) and
[9. Fault and kill](../journey/09-fault-and-kill.md). The fault containment rule is
`../reference/porting.md` ("Fault-isolation contract").

## Multi-arch: the uniform-fleet thesis

**Why.** A kernel for several CPU families can be built two ways. Each port can be its
own variant, where the scheduler or the protection rules bend a little to fit the chip;
an application then behaves the same only on the board it was tested on. Or the
semantics can be fixed above the hardware, and each port only realises them.

**What KickOS chose.** The second, stated as a thesis: **the same semantics hold on
every target, and a divergence is a bug, not a port quirk.** A syscall returns the
same result, a priority means the same thing, a capability prints the same value,
on an 8-region Cortex-M0+ and on a RISC-V core with page tables. When one target
behaves differently, the fix goes into that target until it conforms, not into a
note that excuses it.

This only works if the part that changes per target is small and sharply bounded.
KickOS keeps it in one seam: `arch/include/kickos/arch/arch.h` lists every operation
the kernel may ask of the hardware, such as `arch_switch`, `arch_timer_arm` and
`arch_mpu_apply`. Below it, `arch/<family>/` holds each ISA's trap entry and context
switch, and `arch/<family>/chip/<chip>/` each chip's clocks, timer and protection
unit. The kernel in `kernel/` names no ISA, no register and no protection-hardware
format. The host `sim` is one more target of the same seam: it runs the real kernel
and its userspace as one Linux process, so the semantics can be tested with no board.

Porting is therefore a bounded job: a new chip implements the chip half of the seam,
and a new ISA the arch half, without restructuring the kernel. The list of arch
backends and chips changes over time, so the Book does not keep it;
[`../reference/porting.md`](../reference/porting.md) enumerates it, and
[`../reference/boards.md`](../reference/boards.md) has each board.

**Where it is developed.** Chapter 3.5, *[Context switching and the silicon
contract](context-switching-and-the-silicon-contract.md)*, states the per-arch
contract; Chapter 4, *[One kernel, many ISAs](one-kernel-many-isas.md)*, compares the families; Chapter 6 and *[Porting a new ISA:
RISC-V](porting-a-new-isa-riscv.md)* show a port. In the Journey, chapters
[12](../journey/12-armv6m.md) to [17](../journey/17-xtensa.md) walk the same stops on
each CPU family and show what changes at each one.

## A low barrier: write a main, that's it

**Why.** A kernel that is safe but painful to start with does not get used, and a
capability system can easily ask a newcomer to describe every capability before
"hello" can print. KickOS's usability benchmark is its
sibling project **KickCAT**, an EtherCAT stack: *write a `main`, that's it.*

**What KickOS chose.** A low barrier is a hard constraint, on two kinds of user.

- **Application authors** write a plain `main`, in C or C++, and use the standard
  library: `printf`, `std::cout`, `malloc`. The f411 example is the whole of it: two
  files, linked against `KickOS::kernel` and `KickOS::system_default`, the board's
  default composition. That composition runs `main` as an unprivileged task whose
  thread holds a **sane default set of capabilities**, enough for a plain program,
  and the value `main` returns ends the system. A plain application never writes a manifest.
  Shaping the capability set, adding tasks and drivers, is opt-in: a system file
  such as those in `examples/composition/systems/` lists the tasks and the authority
  their threads start with. Easy things stay easy, and hard things stay possible.
- **Porters** implement the small arch/chip seam described above, not a kernel
  restructure.

The capability model is bent to fit this goal, and not the reverse: capabilities
hide behind defaults, and the runtime wires them before `main` runs.

**Where it is developed.** Chapter 0.5, *[The toolchain: from source to a running
image](the-toolchain-from-source-to-a-running-image.md)*, builds the f411 example;
Chapter 0.6, *[What's under #include](whats-under-include-libc-and-the-cxx-runtime.md)*,
explains how `printf` reaches the kernel. In the Journey:
[1. Power-on](../journey/01-power-on.md) follows the boot to the first instruction of
`main`. The contract is `../reference/architecture.md` ("Design pillars",
"Guiding invariants").

## seL4's paradigm, without its burden

**Why.** seL4 showed what a microkernel built entirely on capabilities can be, and
proved its C implementation correct against its specification. Its design principles
answer the safety aim directly. Its verification machinery has a cost: proof
engineering, and a capability manifest (CapDL) to describe the system before it can
boot.

**What KickOS chose.** KickOS takes seL4's **design principles**:

- a kernel small by nature;
- capability-based authority, through a per-thread table of typed,
  rights-bearing capabilities named by handles;
- drivers, file systems and network stacks as unprivileged servers reached by IPC;
- synchronous endpoints with a one-shot reply capability;
- services wired at start by a root task that hands each thread the capabilities
  it needs.

It deliberately does **not** pursue formal verification, CapDL or proof engineering;
its guarantees rest on a small kernel, review and tests. It also leaves out seL4
mechanisms that a small static RTOS does not need, such as untyped memory and a
capability derivation tree, each refused on arithmetic rather than taste
(`../reference/architecture.md`, "Non-goals").

The syscall surface follows from the same paradigm and stays minimal. `read`, `open`
and `socket` are userspace stubs that send IPC to servers, never kernel calls. The
one sanctioned exception is the debug console write, `kos_kconsole_write`, which lets
a panic, a fault report or early boot print before any driver exists. The f411
example uses both paths: its `printf` goes to the UART driver task when one serves
standard output, and falls back to the kernel console when none does.

**Where it is developed.** Chapter 0.4 compares the structures and says what KickOS
takes from seL4; Chapter 3.9, *[The syscall path](the-syscall-path-trap-dispatch-return.md)*,
covers the minimal surface; Chapters 8.3, *[Endpoints](endpoints-synchronous-ipc-by-rendezvous.md)*,
and 8.5, *[Synchronous call/reply](synchronous-call-and-reply.md)*, cover IPC. In the
Journey: [5. Call and reply](../journey/05-call-and-reply.md).

## Tickless time and real-time determinism

**Why.** A real-time system is judged by its worst case, not its average: a control
loop that runs on time a million times and late once has failed. Two common habits
work against that. A periodic tick wakes the CPU on every period whether or not
anything is due, which costs power and bounds the timing resolution to the period.
Hidden allocation and long kernel paths make the worst case hard to state.

**What KickOS chose.**

- **Priority-based, preemptive scheduling**, FIFO within a priority (first come,
  first served: a thread runs until it blocks or yields), with round-robin as an
  option: threads of one priority take turns, each running for at most a quantum, a
  fixed slice of time. A switch is decided in one place, `pick_and_seat` in
  `kernel/sched/sched.cc`, whatever triggered it.
- **Tickless time.** There is no mandatory periodic interrupt. The kernel keeps a
  monotonic clock and arms one one-shot timer for the earliest deadline; a system
  with nothing timed arms nothing. Every deadline, from a sleep to the end of a
  quantum, rests on that clock.
- **Priority inheritance** on the mutex, so a low-priority holder cannot block a
  high-priority waiter behind a medium one; call/reply lends the caller's priority
  to the server the same way.
- **Static allocation first.** Kernel objects live in fixed pools, and a system can
  run with the heap disabled.
- **A short kernel**, so the paths that run with interrupts masked stay short.

**Where it is developed.** Chapter 2.1, *[The time
base](the-time-base-a-monotonic-clock-you-can-trust.md)*; Chapter 2.2, *[The blocking
substrate](the-blocking-substrate-one-wait-wake-primitive.md)*; Chapter 2.3,
*[Priority inheritance](priority-inheritance-lending-urgency.md)*. In the Journey:
[3. Sleep](../journey/03-sleep.md) and [4. A mutex](../journey/04-mutex.md). The
contract is `../reference/architecture.md` ("Scheduler").

## A kernel to learn from

**Why.** The reasons behind a kernel's design often live only in its history, and its
documentation describes the interface but not the choice. KickOS is meant to be read,
so its documentation is part of the design.

**What KickOS chose.** Three documents, each answering one question and linking to the
others instead of repeating them:

- **The Book** (this document) answers *why*: the concept, the design space, and the
  choice. It stays timeless by leaving exact rules to the Reference.
- **The Journey** (`../journey/`) answers *where, and in what order*: it walks the real
  source stop by stop, with gdb recipes.
- **The Reference** (`../reference/`) answers *exactly what*: the interfaces, the
  invariants and the porting rules, kept in step with the code. When the two disagree,
  the code wins.

In the code, a comment explains a hidden constraint, not what the code plainly does.
Chapter 0.0, *[How to read this book](how-to-read-this-book.md)*, gives a reading
path through the three documents.

## When the principles pull apart

The principles sometimes disagree, and the way KickOS settles each case is part of the
design.

- **Safety against a low barrier.** Capabilities could demand a manifest. KickOS keeps
  the capabilities and moves the work into defaults.
- **Uniform semantics against the hardware ceiling.** A chip may lack the protection
  another has. KickOS keeps the semantics, states the ceiling per chip, and refuses a
  configuration it cannot honour, rather than lowering every board to the weakest one.
- **Isolation against speed.** A syscall on every hot operation would be safe and
  slow. Where an operation is hot and the authority is a register window, holding the
  window is the capability, and no trap is taken (Chapter 8.4, *[The fast path is the
  capability](the-fast-path-is-the-capability-gpio-direct-mmio.md)*).

## Where to go next

- The structure these principles rest on: Chapter 0.4, *[Monolithic vs
  microkernel](monolithic-vs-microkernel.md)*.
- Building and running the f411 example: Chapter 0.5, *[The toolchain: from source
  to a running image](the-toolchain-from-source-to-a-running-image.md)*.
- A reading path through the Book, the Journey and the Reference: Chapter 0.0,
  *[How to read this book](how-to-read-this-book.md)*.
- The kernel model, starting with time: Chapter 2.1, *[The time base: a monotonic
  clock you can trust](the-time-base-a-monotonic-clock-you-can-trust.md)*.
- The code from reset: [Journey 01, Power-on](../journey/01-power-on.md).
- The exact contract: `../reference/architecture.md` ("Design pillars", "North star",
  "Non-goals", "Memory domains").
