<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# Memory protection: one abstraction over two kinds of hardware

> This chapter opens Part 7. It explains why a kernel fences memory, the two kinds of
> hardware that can do it, and the one abstraction KickOS puts over both: the memory
> domain. It then shows what the kernel asks of each kind, how stacks and placement
> follow from the hardware, and why every pointer a program hands the kernel is
> checked against that program's memory. The sub-chapters 7.1 to 7.7 go deep on one
> question each; the reading map at the end says which. The exact contract is in
> `../reference/architecture.md` ("Memory domains").
>
> *Further reading: Tanenbaum, Modern Operating Systems, ch.3 (memory management:
> address spaces, paging, the MMU and the TLB) and ch.9 (protection domains).*

## Why a kernel fences memory

Chapter 0.1 described a microcontroller as one flat address space: every memory and
every device sits at a fixed address, and any instruction can name any address. With no
protection, every program on the chip is in effect one program. A stray pointer in one
thread can overwrite another thread's stack, the kernel's ready queue, or a device
register. The bug shows up somewhere else, later, and the system has no way to say who
did it.

Memory protection turns that into a fault at the faulting instruction. The kernel
describes, for the program that runs now, which addresses it may read, write or execute.
The hardware checks every access against that description, and refuses one that falls
outside it. Three things follow, on any OS:

- **Only privileged code may write the description.** If a program could edit its own
  fence, the fence would mean nothing. This is why protection is kernel work (Chapter 0.3).
- **The description changes with the running program.** Each program has its own fence,
  so the kernel reloads the hardware when it switches from one to another.
- **A refused access is a trap.** The kernel then decides what dies: on KickOS, the
  faulting thread's task and nothing beyond it.

## Two kinds of hardware

**Why there are two.** A protection unit sits between the core and the bus, and there are
two ways to build one. Chapter 0.1 named them: the MPU of a microcontroller and the MMU
of an application processor. They differ in what they can describe, and that shapes
everything else.

**Region protection.** An MPU (ARM's PMSA, the RISC-V PMP, the Renesas RX MPU, the NXP
SYSMPU) holds a small table of **regions**. Each row names a range of physical addresses
and the rights unprivileged code has there. An access that no row allows faults. Nothing
is translated: every program sees the same memory at the same address, and the unit only
decides who may touch what. The table is small (eight rows on the f411's Cortex-M4) and
each row has a shape rule. On PMSAv7 a row covers a power-of-two size, at least 32 bytes,
aligned to its own size; on PMSAv8, the SYSMPU and the RX MPU it is a base and a limit on a
fixed granule; the RISC-V PMP, in the NAPOT form KickOS uses, follows the power-of-two
rule. Switching programs means rewriting a few registers.

**Page tables.** An MMU translates every address the program issues (a **virtual**
address) into a **physical** one, through a tree of tables in memory. Each leaf maps one
page, typically 4 KiB, with its rights. The kernel can give each task its own tree, its
own **address space**: the same virtual address means different memory in two tasks, and
memory a task has no mapping for does not exist for it. Switching tasks means writing a
new root register. The cost is the machinery: frames of RAM for the tables, a walker in
the core, and a translation cache (the TLB) the kernel must keep honest.

| | Region unit (MPU, PMP) | Page tables (MMU) |
|---|---|---|
| What a task sees | the one physical address space | its own virtual address space |
| Unit of protection | a range, with a shape rule | a page |
| How many | a handful of rows | as many pages as RAM allows |
| Sharing memory | the same bytes in two region sets | the same frame mapped into two spaces |
| A switch | rewrite the rows | write a new root |

Some parts have neither. On those nothing is enforced, and the kernel still runs, as one
trusted program with its applications.

## One abstraction: the memory domain

**How one could.** A system that wants both kinds of part has three obvious choices. It
can be two systems, a small RTOS for the microcontrollers and a general-purpose OS for the
application processors, with two APIs and two isolation models. It can treat the MMU as a
big MPU: map everything at its physical address and use only the permission bits, which
wastes translation. Or it can pretend every part has an MMU, which a region unit cannot
fake: it has no way to give two tasks different memory at one address.

**What KickOS chose.** One abstraction, defined by what both kinds of hardware can keep:
the **memory domain**, the set of memory a task may reach. Threads belong to a task, and a
task owns one domain that its threads share. On a region part the domain carries a
**region set**, and every domain lives in the one physical address space. On a
translating part it carries a **page-table root**, and the domain is a virtual address
space. The calls a program makes are the same on both: reserve RAM (`kos_ram_alloc`),
grant it to yourself (`kos_mem_self_grant`), give a child a data region or a window when
you spawn it (`kos_thread_params`). Above the arch layer the kernel names no register and
no descriptor format.

The portable promise is the floor both kinds keep: **a grant reaches its holder, and
nothing a task was not granted is reachable from it**. Each backend may give more. A
region unit can fence one thread's stack from its siblings, because the region set is
reloaded per thread anyway. Page tables give each task private addresses and let a frame
be mapped where its holder chooses. The Reference states the floor and names each
strengthening; Chapter 1, *[Overview and principles](overview-and-principles.md)*, says
why one kernel spans the whole range of parts.

## What the kernel asks of each kind

The kernel above the seam knows which kind it runs on, and compiles in only what means
something there. A chip states its unit twice, in its chip file (`protection: { unit: ... }`
in `platform/<chip>/chip.yaml`) and in its Kconfig selection, and the build refuses a chip
whose two statements disagree. A build then has `KICKOS_HAVE_MPU` or `KICKOS_HAVE_ASPACE`
set, never both.

**From a region unit**, the kernel needs a way to say whether one row can describe a range
exactly (`arch_mpu_region_encodable`), to encode a whole region set into the unit's own
words (`arch_mpu_encode`), and to install it. Installing is two steps: `arch_mpu_apply`
names the incoming thread's set when the scheduler decides, and `kickos_arch_mpu_commit`
writes it into the unit at the physical switch, where the outgoing thread has really
stopped running. Chapter 7.5 explains why. The arch also rounds every RAM block to a size
one row can describe (`arch_ram_region_size`) and lists the blocks it owns for life, which
no grant may touch (`arch_reserved_blocks`). The kernel spends almost no rows on itself:
privileged code reaches what the rows do not describe, through the ARM **background
region** or its equivalent on the other units. The rows describe only what unprivileged
code may reach.

**From page tables**, the kernel needs a space it can create, map into, unmap from, switch
to and destroy (`arch_aspace_create`, `arch_aspace_map`, `arch_aspace_unmap`,
`arch_aspace_activate`, `arch_aspace_destroy`), a way to reach the bytes behind a user
virtual address from the kernel (`arch_aspace_acquire`, `arch_aspace_release`), and the
range of RAM it may cut frames from (`arch_frame_pool_bounds`). The walk itself is shared
by every translating backend; each spells only its descriptor format.

**Built for one kind only.** A translating build compiles in the frame pool
(`FrameAllocator`), frames as kernel objects named by capability (`kos_frame_create`,
`kos_frame_map`, `kos_frame_unmap`, with `kos_aspace_self` naming the caller's space), and
a per-task space in the domain. It compiles out the region set's enforcing path, since
there is no row to seat. A region build is the reverse: the region set (`MpuSet`), the
stash and commit, the rounding of arena blocks, and the record of which task reserved each
block (`kernel/mem/ramown.cc`), which on a translating build is the space's own range list.

Both meet in one place. When the scheduler picks the next thread, `switch_book`
(`kernel/sched/sched.cc`) makes the same two calls on every board:
`aspace_activate_for(next)` and `next->mpu.apply()`. On a region build the first compiles to
nothing; on a translating build the second does.

## Stacks and placement

**Stacks.** Every thread needs a stack of its own, even when it shares everything else with
its task. On a region part a thread's stack is a block of the user
RAM arena, rounded and aligned so one row covers it, and a row of that thread's own set.
Because the set is reloaded on every switch, even between two threads of one task, a
sibling that writes into another's stack faults. On a translating part a stack is a run of
frames mapped into the task's space, with one page below it left unmapped as a **guard**
(`ustack_alloc`, `kernel/mem/ustack.cc`): an overflow hits a missing translation and
faults. That stack is mapped task-wide, so a sibling does reach it; the per-thread stack is
a region-backend strength, not the portable promise. A spawner may also bring the stack
itself (`stack_base` and `stack_size` in `kos_thread_params`): a block its own task reserved
with `kos_ram_alloc`, checked like any other grant. Chapter 3.6, *[Thread stacks](thread-stacks-and-the-kiss-tension.md)*, explains who owns a
stack under a no-free allocator.

**Placement.** A row with a shape rule makes placement a protection question. If the app's
writable globals are spread across RAM between the kernel's, no small set of rows can grant
one without the other. So on a region part the linker places them: the kernel's own data in
one place, the app's data in one window sized and aligned for one row, and the arena that
stacks and reservations come from beside them. Every byte of the image must land on the right
side of that line, the C++ runtime's included. On a translating part placement matters less:
protection is per page, and translation hides where a frame sits physically. Chapters 7.2 and
7.3 are about getting placement right.

### The f411 example: the regions of a hello

The f411 example is a `main` that calls `printf("hello\n")` (Chapter 0.5 builds it). On the
STM32F411 its thread runs unprivileged under a PMSAv7 MPU, and the regions that matter are
three, set by the linker script `arch/arm/chip/stm32f411/stm32f411.ld`:

| Row | Range | Rights | What it holds |
|---|---|---|---|
| code | `0x08000000`, 512 KiB | read, execute | the whole flash: the app's code and constants |
| app data | `0x20008000`, 16 KiB | read, write, never execute | the app's globals, and in the space left over the heap that `printf`'s buffer comes from |
| stack | a block of the arena, `0x2000C000` to `0x2001E000` | read, write | `main`'s own stack |

Three of eight rows. The kernel's data, from `0x20000000` to `0x20008000`, is in no row, so
`main` cannot touch it; the kernel reaches it through the background region. USART2, at
`0x40004400`, is in no row either. That is why `printf` ends in a syscall: a store from `main`
to the data register at `0x40004404` would fault, so the bytes go to the kernel, which writes
the register itself (Chapter 0.3).

Now install the package for `qemu-arm64`, a Cortex-A53 with an MMU, and build the same
`main.c`. The task runs at EL0 in its own address space. Its code is mapped read and execute,
its data read and write, its stack as pages with a guard page below. The kernel lives in the
other half of the address space, which EL0 cannot reach. The UART at `0x09000000` has no
mapping in the task at all. The rows have become page-table mappings, the `printf` still ends
in a syscall, and the program cannot tell. Journey chapter 10 walks those tables.

## Why every syscall argument is checked against the caller's memory

**Why.** A syscall is the one place where an unprivileged program hands the kernel a value
the kernel then uses with privilege. On a region part the kernel runs under the background
region, so the MPU does not check its accesses on the caller's behalf. On a translating part
the kernel can reach every frame. Suppose `printf`'s write passed the kernel a pointer to
another task's buffer, or to the kernel's own data. If the kernel read through it unchecked,
it would print memory the caller could never read itself. The kernel would have become what
the literature calls a **confused deputy**: a privileged party tricked into using its power
for someone who lacks it.

**How one could.** A kernel can trust callers, which forfeits isolation the moment one
misbehaves. It can touch user memory with the caller's rights and catch the fault, which needs
hardware that checks a privileged access on someone else's behalf. Or it can check every
pointer, length and index at the boundary, against the caller's own grants, before using it.

**What KickOS chose.** The check at the boundary. It works the same on every unit, and on
none, and it fails as a clean error code rather than a kernel fault. A pointer must lie in
memory the caller could reach itself, with the right it needs; a struct is copied in rather
than read in place; a scalar that indexes something is bounded before it does. Enforcement
does not make this optional: the kernel is on the privileged side of the very unit that
enforces.

**How it is done.** When the f411 example's `printf` reaches the kernel console, the write
hands the kernel a buffer and a length. The
`KOS_SYS_KCONSOLE_WRITE` case in `kernel/syscall/syscall.cc` clamps the length, asks
`user_readable_ok` whether the caller may read the whole buffer, and answers `-KOS_EFAULT` if
not; `console_write_user` then copies the bytes into a kernel buffer with `kaccess_from_user`
before the console sees them. On a region part the check reads the caller's region set, and a
plain copy follows, since kernel and caller share addresses. On a translating part every user
access goes through `arch_aspace_acquire`, which walks the caller's own tables and answers
nothing for an unmapped page. On a part with no unit there is no set to read, so the check
falls back to the chip's own memories as the linker defines them (`arch_user_text_readable`,
`arch_user_data_writable`). The contract is in `../reference/architecture.md`
("Syscall-argument validation") and `../reference/invariants.md`
(`user-args-validated-at-boundary`); Chapter 3.9,
*[The syscall path](the-syscall-path-trap-dispatch-return.md)*, shows where in the path the
check sits.

## Reading map

Each sub-chapter answers one question this overview leaves open.

- **7.1** *[Alignment across the syscall boundary](alignment-across-the-syscall-boundary.md)*:
  why owning a pointer is not enough, and how an alignment check becomes a portability bug.
- **7.2** *[Exceptions and RTTI under memory protection](exceptions-and-rtti-under-memory-protection.md)*:
  what a `throw` touches at run time, and how an unprivileged thread reaches all of it.
- **7.3** *[Where your RAM goes](where-your-ram-goes-full-cxx-memory-floor-and-the-linker-split.md)*:
  what full C++ costs in writable RAM, and how the linker splits kernel bytes from app bytes.
- **7.4** *[Privilege is three axes, not one bit](privilege-is-three-axes-not-one-bit.md)*:
  what "privileged" really names (CPU mode, memory posture, authority) and why they come apart.
- **7.5** *[Protection follows the CPU, not the scheduler's intent](protection-follows-the-cpu-not-the-schedulers-intent.md)*:
  why the region set is written at the physical switch and not at the scheduler's decision.
- **7.6** *[The CPU reads ahead](memory-types-and-speculative-access.md)*: why a region's memory
  type matters as much as its rights, and what a permissive background costs.
- **7.7** *[Whoever stacks the trap frame owns the bounds check](whoever-stacks-the-trap-frame-owns-the-bounds-check.md)*:
  why the stack pointer a trap saves through is untrusted input, and who must check it.

## Where to go next

- The Journey shows the code. [6. Shared memory](../journey/06-shared-memory.md) follows a
  block from `kos_ram_alloc` to an MPU row and a refused store;
  [9. Fault and kill](../journey/09-fault-and-kill.md) follows the fault to the end of the task;
  [10. The MMU](../journey/10-the-mmu.md) runs the same walk on `qemu-arm64`, page tables and
  all.
- Chapter 3.7, *[Peripheral isolation and the hardware
  ceiling](peripheral-isolation-and-the-hardware-ceiling.md)*, shows why a device window is
  only as private as the unit's place on the bus allows.
- `../reference/architecture.md` ("Memory domains", "User/kernel separation") holds the
  contract, and `../reference/porting.md` ("MPU descriptor encodings, per backend", and the
  `arch_aspace_*` pages) what a port owes.
