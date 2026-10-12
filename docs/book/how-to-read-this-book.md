<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# How to read this book

> Where should a newcomer start, and what else is there to read? This chapter
> says who the Book is written for, which example it follows, how it fits with
> the two other KickOS documents, and in what order to read them. After it you
> know which chapter to open next, and where to look when you want to see the
> code or check an exact rule.

## Who the Book is for

The Book assumes a reader who can read C and a little C++, and who has compiled
a program, linked it and put it on a board or run it. It does not assume a
course on computer architecture or operating systems. Part 0 supplies both, one
concept at a time:

- Chapter 0.1 describes the machine: the processor, memory and devices.
- Chapter 0.2 says what an operating system is, and which part of it is the
  kernel.
- Chapter 0.3 shows how the hardware stops a running program to run the kernel.
- Chapter 0.4 compares two ways to divide the work between the kernel and
  everything else.
- Chapter 0.5 follows a program from source text to a running board.
- Chapter 0.6 shows what a line such as `#include <stdio.h>` brings into the
  program.

Chapter 1 then says what KickOS is for and the principles it applies, and the
chapters after it take KickOS apart one design decision at a time.

The Book teaches the general idea first, the way any operating system faces it,
and only then what KickOS does. For the full theory behind an idea, each chapter
names a chapter of Andrew S. Tanenbaum's *Modern Operating Systems*.

## One example through Part 0

Part 0 follows one small program throughout, **the f411 example**: an
application whose `main` calls `printf("hello\n")`, running on the `f411disco`
board, a development board built around the STM32F411 microcontroller. The
text leaves the chip through USART2, the serial port the board uses as its
console. Each chapter takes the slice of the example it needs:

- Chapter 0.1 finds the serial port in the chip's memory.
- Chapter 0.2 explains why `printf` must ask the kernel instead of writing to the
  port itself.
- Chapter 0.3 follows the program into the kernel, and a timer interrupting it.
- Chapter 0.4 asks who owns the serial port.
- Chapter 0.5 builds the example and puts it on the board.
- Chapter 0.6 lists what `printf` pulls in from the C library.

## How a chapter is shaped

Each chapter follows the same order: the problem, the ways one could solve it,
what KickOS chose and why, and where that choice lives in the code. A reader who
only wants the rule can skip to the last step and follow its link.

## Three documents, three questions

KickOS keeps three bodies of documentation. Each answers one question, and each
links to the others instead of repeating them.

| Document | Answers | Where |
|---|---|---|
| **The Book** | *Why* is it built this way? The ideas, the possible designs, the choice. | [`./`](README.md) |
| **The Journey** | *Where, and in what order*, does it happen in the code? A walk through the source, stop by stop, with debugger recipes. | [`../journey/`](../journey/README.md) |
| **The Reference** | *Exactly what* is the rule? Interfaces, guarantees, porting rules, boards. | [`../reference/`](../reference/README.md) |

The Reference is kept in step with the code, and when the two disagree, the code
wins. The Book can stay timeless because it leaves the exact numbers and rules to
the Reference. The Journey sits between them: it names a file and a function,
says what happens there, and links up to the Book for the reason and across to
the Reference for the rule.

## A reading path for a newcomer

1. **Part 0 in order:** this chapter (0.0),
   [the machine](the-machine-underneath.md) (0.1),
   [what an OS and a kernel are](what-is-an-os-and-a-kernel.md) (0.2),
   [interrupts and traps](interrupts-and-traps.md) (0.3),
   [monolithic versus microkernel](monolithic-vs-microkernel.md) (0.4),
   [the toolchain](the-toolchain-from-source-to-a-running-image.md) (0.5), and
   [the C library and C++ runtime](whats-under-include-libc-and-the-cxx-runtime.md)
   (0.6).
2. **Chapter 1,** [Overview and principles](overview-and-principles.md): what
   KickOS is for, and the principles every later chapter applies.
3. **Build and run once.** Follow [Journey 00, Setup](../journey/00-setup.md) to
   build the `qemu` preset and attach a debugger. Seeing the first message and
   the first breakpoint makes everything after it concrete.
4. **Walk power-on.** [Journey 01](../journey/01-power-on.md) goes from reset to
   the first instruction of `main`.
5. **Alternate Journey and Book.** Read a Journey chapter, then the Book chapter
   that explains the design it walked:

| Journey chapter | Book chapter beside it |
|---|---|
| [02, the first syscall](../journey/02-first-syscall.md) | 3.9, [The syscall path](the-syscall-path-trap-dispatch-return.md) |
| [03, sleep](../journey/03-sleep.md) | 2.1, [The time base](the-time-base-a-monotonic-clock-you-can-trust.md); 2.2, [The blocking substrate](the-blocking-substrate-one-wait-wake-primitive.md) |
| [04, a mutex](../journey/04-mutex.md) | 2.3, [Priority inheritance](priority-inheritance-lending-urgency.md) |
| [05, call and reply](../journey/05-call-and-reply.md) | 8.3, [Endpoints](endpoints-synchronous-ipc-by-rendezvous.md); 8.5, [Call and reply](synchronous-call-and-reply.md) |
| [06, shared memory](../journey/06-shared-memory.md) | 7.5, [Protection follows the CPU](protection-follows-the-cpu-not-the-schedulers-intent.md) |
| [07, an interrupt to a driver](../journey/07-interrupt-to-a-driver.md) | 3.8, [A masked interrupt is latched](a-masked-interrupt-is-latched-not-lost.md) |
| [08, spawn and capabilities](../journey/08-spawn-and-caps.md) | 8.0, [Authority](authority-from-possession-or-from-identity.md); 8.1, [Handles](handles-and-the-resolve-chokepoint.md) |
| [09, fault and kill](../journey/09-fault-and-kill.md) | 7.4, [Privilege is three axes](privilege-is-three-axes-not-one-bit.md) |

6. **Branch out.** The rest of the Book can be read in any order; each chapter
   names what it builds on. A porter reads Chapter 6 and
   [`../reference/porting.md`](../reference/porting.md) next.

## Where to go next

- Chapter 0.1, [*The machine underneath*](the-machine-underneath.md), the first
  step of Part 0.
- The full list of chapters, with what each one covers:
  [the Book's index](README.md).
