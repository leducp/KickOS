<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# The KickOS journey

The journey walks through the KickOS source, starting when the CPU leaves reset. It follows
the code into the kernel, through the first context switch and into an unprivileged
application. Then it traces common system calls from end to end. Each chapter is a sequence
of **stops**. Each stop names a file and a function, explains what happens there, and gives
a short gdb recipe so you can watch it happen on an emulator.

## Who it is for

You have read Part 0 of the [Book](../book/README.md), or you already know the same things:
C and a little C++, how a program is compiled, linked and loaded onto a board, what an
interrupt is, and what a system call is. Now you want to open the code and understand it
file by file.

The journey sits between the two other documents:

- **The Book** ([`../book/`](../book/README.md)) teaches the concepts and the design choices:
  why a microkernel, why capabilities, why the protection unit is reprogrammed at the context
  switch. It explains *why*.
- **The Reference** ([`../reference/`](../reference/README.md)) states the precise rules
  implemented by the code. When they disagree, the code is authoritative. It says *exactly what*.
- **The journey** shows *where and in what order*. It walks the code paths that the Book
  explains and the Reference specifies, and its stops link to both. It does not repeat the
  contract; follow the link when you need the precise rule.

## The reference walk

Chapters 1 to 9 run on one machine: the `qemu` preset. It emulates an Arm MPS2 board
(`mps2-an386`) with a Cortex-M4F core and a PMSAv7 memory protection unit (MPU). You need no
board, only QEMU and gdb. The `f411disco` board (an STM32F411, also a Cortex-M4F) is its
silicon twin. Where the chip layer differs, a stop names the f411 file next to the QEMU one.

Chapters 10 to 17 (the MMU, multicore, and each other CPU family) are told as a difference
from this walk: the same stops, and what changes at each one. Most run on another QEMU machine;
the RX and Xtensa chapters have no emulator and read the linked image instead. Chapter 18 ends
the journey with one real system built from the earlier pieces.

## Chapters

| File | What it walks |
|---|---|
| [`00-setup.md`](00-setup.md) | **Setup**: build the `qemu` preset, run an image, run it under gdb, and a map of the source tree |
| [`01-power-on.md`](01-power-on.md) | **Power-on**: from reset to the first unprivileged instruction of the application's `main` |
| [`02-first-syscall.md`](02-first-syscall.md) | **The first syscall**: the trap, the dispatch, the handle resolve, and the way back to user mode |
| [`03-sleep.md`](03-sleep.md) | **Sleep**: the time base, the one-shot timer, the park, and the wake |
| [`04-mutex.md`](04-mutex.md) | **A mutex**: a word taken in user mode, the contended entry, and priority inheritance |
| [`05-call-and-reply.md`](05-call-and-reply.md) | **Call and reply**: endpoints, the rendezvous, the reply capability, and the register fast path |
| [`06-shared-memory.md`](06-shared-memory.md) | **Shared memory**: arena blocks, grants, and MPU regions committed at the context switch |
| [`07-interrupt-to-a-driver.md`](07-interrupt-to-a-driver.md) | **An interrupt to a driver in user space**: the vector, the notification, the mask, and the re-arm |
| [`08-spawn-and-caps.md`](08-spawn-and-caps.md) | **Spawn and capabilities**: making a thread and a task, granting, resolving and closing capabilities |
| [`09-fault-and-kill.md`](09-fault-and-kill.md) | **Fault and kill**: a user fault, the report, the kill, who survives, and the kernel's own panic |
| [`10-the-mmu.md`](10-the-mmu.md) | **The same walk with an MMU**: `qemu-arm64`, page tables, address spaces, the user copy, the TLB |
| [`11-multicore.md`](11-multicore.md) | **Multicore**: one kernel on four cores (the kernel lock, the doorbell, per-core queues), then one kernel per core |
| [`12-armv6m.md`](12-armv6m.md) | **ARMv6-M**: the same walk on a Cortex-M0 and M0+ |
| [`13-armv8m.md`](13-armv8m.md) | **ARMv8-M**: the same walk on a Cortex-M33, with the PMSAv8 MPU |
| [`14-riscv.md`](14-riscv.md) | **RISC-V**: the same walk on RV32 with PMP, and a look at RV64 |
| [`15-x86_64.md`](15-x86_64.md) | **x86_64**: the same walk on a PC, from UEFI firmware to ring 3 |
| [`16-rx.md`](16-rx.md) | **RX**: the same walk on a Renesas RXv3 |
| [`17-xtensa.md`](17-xtensa.md) | **Xtensa**: the same walk on an ESP32, with register windows and no privilege levels |
| [`18-a-real-system.md`](18-a-real-system.md) | **A real system: an SPI bus service and its client**: the K64F SPI service and the client that calls it |

Read 00 and 01 first. After that, 02 is the base for every syscall chapter (03 to 09), which are
best read in order. The architecture chapters (10 to 17) assume you know 01 and 02; 10 also
builds on 06, and 11 runs on the machine of 10. Read 18 last.

## The stop format

Every stop has the same shape:

```
### Stop N: <what happens here, in a few words>
**Where:** `path/to/file.cc`, `function_name` (and the next symbols to open)

<what happens, in plain English; why in this order; what to notice>

<sometimes a short excerpt, copied from the tree>

**Try it:** the gdb commands, and what gdb printed when the stop was checked.

**Why, and the contract:** links to the Book chapter and the Reference page.
```

- **Where** names a file and a symbol, never a line number. Line numbers move with every
  edit; a symbol can be found with `grep` or with gdb's `break`.
- **Excerpts** are copied from the source, with `// ...` where lines are left out. They are
  short on purpose. Open the file for the rest.
- **Try it** shows real gdb output, trimmed. Addresses come from one particular build, so
  yours may differ. What matters is the shape: which function stops, which
  register holds which kind of value. On a chip no emulator models, the Try it reads the
  linked image with the toolchain's `objdump` or `nm`, or is left out.

## Reading with gdb, or without it

**Without gdb.** Every stop is written to be read on its own. The prose says what the code
does, and the gdb output in **Try it** shows what the machine looked like at that moment. You
can read the whole journey on paper and follow it.

**With gdb.** Follow [`00-setup.md`](00-setup.md) once to build the images and learn how to
start QEMU halted with a gdb server. Then, for any stop, start the image again, attach gdb,
and type the **Try it** commands. Some habits help:

- Started with `-S`, QEMU holds the core at its first instruction. `continue` runs until the next
  breakpoint.
- `break <function>` works for C functions and for C++ functions with their namespace
  (`break kickos::kmain`). The application's `main` is renamed by the build: use
  `break kickos_app_main`.
- **gdb sees memory through the MPU.** When the core is stopped in unprivileged code, QEMU
  applies the running thread's permissions to gdb's reads too. Reading a kernel variable then
  fails with `Cannot access memory at address ...`. Stop in kernel code (any kernel function,
  or a syscall) to inspect kernel state.
- In a thread's backtrace, the last frame often shows as `kos_panic`. gdb names a return
  address by the byte just before it. The kernel plants `kickos_user_thread_return` as the
  thread's first return address, and in the `qemu` image that function sits right after
  `kos_panic`. The thread is not panicking.

## Conventions

The journey follows the Book's conventions ([`../book/README.md`](../book/README.md)): ASCII
only, no project-status language, and the code as it is. The source tree is the
authority. If a stop and the code disagree, the code is right and the stop is a bug.
