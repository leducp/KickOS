<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# What's under #include: the C library and the C++ runtime

> What does `printf("hello\n")` need underneath it, on a chip with no terminal and
> no files?
>
> You will learn:
>
> - what the C library and the C++ runtime are made of
> - what each piece asks of the operating system
> - how the f411 example's `printf` travels from the application to the kernel
>
> *Further reading: Tanenbaum, Modern Operating Systems, ch.1 (the system-call
> interface and the OS as a resource manager).*

On a desktop, `printf` and `std::vector` seem to work on their own. In fact each
rests on a C library and a C++ runtime, and those rest on the operating system.
On a microcontroller, KickOS is that operating system, so it must provide
what they rest on.

## What a C library needs from the OS

The functions of the standard C library fall into two groups:

- **Self-contained functions**, such as `memcpy`, `strlen` and `qsort`, only
  compute. They work without an OS.
- **Functions that need the environment.** `malloc` needs an **arena**, a block
  of memory it hands allocations out of. `printf` needs somewhere to send its
  output, `fopen` needs files, and `exit` needs a way to end the program.

The C standard names two kinds of environment after this split. A
**freestanding** environment guarantees only a small set of headers, such as
`<stdint.h>` and `<stddef.h>`, and an implementation may provide more. A
**hosted** environment provides the whole library.

A portable C library such as newlib implements most of the second group itself.
Underneath, it calls a small set of functions that the platform must supply:

| Function | What newlib needs it for |
|---|---|
| `_sbrk` | growing the heap: `malloc` and `operator new` take their memory from it |
| `_write` | output: the bytes of `printf`, `fputs` and `std::cout` |
| `_read`, `_close`, `_lseek`, `_fstat`, `_isatty` | the other file operations that stdio uses (stdio is the part of the library behind `printf` and `fopen`) |
| `_exit`, `_kill`, `_getpid` | process control: `exit` and `abort` finish by calling them |
| the lock functions, `__retarget_lock_*` | the locks around the heap and around every stream, when several threads run |

newlib also keeps `errno` and its other per-thread state in a **reentrancy
structure**, `struct _reent`. When each thread has its own, two threads can run
newlib's functions at the same time without mixing their state: the functions
are **reentrant**. The platform must give each thread its structure, and let
newlib find the one of the running thread.

These functions and the reentrancy structures are where newlib meets the OS.
Porting newlib to KickOS means providing them.

## What KickOS provides to newlib

Each function bridges what newlib expects to what KickOS offers:

- **File descriptors live in the program, not in the kernel.** newlib names an
  open file by a **file descriptor**, a small number it passes to `_write`,
  `_read` and the other file functions. A monolithic kernel keeps the table of
  open files itself. In a microkernel a file is served by a server: a console
  driver, a file system on a flash chip or an SD card. KickOS's choice is to keep
  each task's table of descriptors in the task's own runtime. An entry says which
  server holds the file and how that server names it, and reading or writing a
  descriptor is a message to that server. The kernel only carries the messages
  and checks the rights to send them.
- **Standard input, output and error are descriptors 0, 1 and 2,** served by the
  console like any other file. Where no console driver serves standard output,
  the bytes go to the kernel console of
  [Chapter 0.2](what-is-an-os-and-a-kernel.md) instead. The
  [console reference](../reference/console.md) has the exact routing.
- **`_sbrk` grows the heap through a fixed span.** The board's linker script sets
  aside a span of RAM for the heap. `_sbrk` grows the heap through that span and
  fails at its end, and then `malloc` returns null. Memory freed with `free` goes
  back to `malloc` for reuse, and the span stays the heap's. On a chip with memory
  protection, the span is the unused part of the memory the application may use
  ([Chapter 7.3](where-your-ram-goes-full-cxx-memory-floor-and-the-linker-split.md)). The heap code is linked only into a program that allocates.
- **newlib asks the OS for its locks, and KickOS provides them.** Several threads
  may call `malloc` or `printf` at the same time, so newlib protects the heap and
  each stream with a lock. newlib does not implement those locks itself: it calls
  the lock functions, and the OS supplies them. KickOS answers with two
  **recursive** locks, locks that the thread holding them may take again without
  waiting. One is for `malloc`, and the other is for everything else, every stream
  included. So a `printf` does not wait behind another thread's `malloc`, unless
  that `printf` must allocate memory itself.
- **Each task has its own heap and its own pair of locks.** A task is a program
  running in its own protected memory, so its heap, its streams and the locks
  that guard them belong to it alone. The threads of one task share them and
  can call `malloc` or `printf` at the same time safely. A thread of another task
  cannot reach them.

## Why every cross build uses newlib

A **cross build** compiles on one machine for another, here for a microcontroller.
Every cross build takes its compiler from the KickOS toolchain ([Chapter 0.5](the-toolchain-from-source-to-a-running-image.md)), and
that toolchain carries newlib, built from the same sources for every core family
and every multilib. So one set of the functions above, written once, serves every
chip. When a build is configured, KickOS checks that the newlib the compiler will
use matches what the board asks for: the full newlib or its smaller "nano" build,
and how newlib finds the running thread's reentrancy structure.

The **sim**, KickOS built as a Linux program, is the exception. It uses the host's
C library, which already provides all of this, so none of the functions above are
compiled for it. The [boards reference](../reference/boards.md) lists each board's
choices.

## The C++ runtime is three stacked libraries

A full C++ program draws on three runtime libraries, each built on the one before:

1. **libgcc:** helper functions the compiler calls for work the core has no
   instruction for, such as a 64-bit division on a 32-bit core, and the
   **unwinder**. During an exception, the unwinder walks back through the stack
   frames (Chapter 0.1, *[A call and the stack](the-machine-underneath.md#a-call-and-the-stack)*)
   to a matching `catch`. libgcc knows nothing of C++ types or of `std::`.
2. **libsupc++:** the support that the C++ application binary interface (ABI,
   [Chapter 0.5](the-toolchain-from-source-to-a-running-image.md)) requires. It throws and catches exceptions with libgcc's unwinder,
   provides RTTI (run-time type information, the type descriptions that `typeid`
   and `dynamic_cast` read) and `operator new`. It also keeps a small emergency
   pool of memory, so that an exception can still be thrown when the heap is full.
3. **libstdc++:** the standard library itself: containers, strings, algorithms,
   streams and locales. It uses libsupc++, and the C library for memory and
   output.

So a program that throws and catches needs the first two, and a program that
uses containers or streams needs all three.

## Freestanding and full C++

KickOS uses a different level of C++ for the kernel and for applications:

- **The kernel and its libraries use freestanding C++.** They compile with
  `-fno-exceptions -fno-rtti -fno-threadsafe-statics -fno-use-cxa-atexit`. The
  first two turn off exceptions and RTTI. The other two stop the compiler from
  calling runtime support functions around static objects. Classes, templates, RAII (a destructor
  releasing what its constructor acquired) and the headers that need no runtime
  remain available. Exceptions, RTTI and the standard library's runtime do not.
- **Applications can use full C++.** They compile with exceptions and RTTI, and
  link the toolchain's libsupc++ and libstdc++. This has a memory cost. Exceptions
  bring tables of read-only data, whose size depends on how the toolchain
  implements exceptions. The runtime adds code, and facilities such as locales add
  more. [Chapter 7.3](where-your-ram-goes-full-cxx-memory-floor-and-the-linker-split.md) measures the cost.

The build applies both levels: an application links the kernel's freestanding
libraries, and its own code gets the full C++ settings.

## Match the C++ runtime to its C library

libstdc++ is compiled against one C library's headers and ABI. Linked over a
different C library, the two can disagree on shared types and functions. For
instance, `<cstdlib>` can define `div_t` differently from `<stdlib.h>`, and
`<cmath>` can miss C math declarations it expects. NuttX, an RTOS (real-time
operating system) with a C library of its own, documents exactly these mismatches
for its users who link the toolchain's C++ runtime over that library. The
[architecture reference](../reference/architecture.md) records them under
"Toolchain-libc lesson from NuttX".

KickOS has a small C library of its own too: a few string and formatting
functions for the kernel. It never hosts the C++ runtime. A full-C++ application
links libstdc++ and libsupc++ over the newlib they were compiled against, the one
in the same KickOS toolchain, for the board's multilib.

An exception thrown in application code stays in application code: the kernel's
code carries no unwind tables, so an exception can never unwind into the kernel.
KickOS also replaces libstdc++'s terminate handler with a smaller one. Chapter 7.2,
[*Exceptions and RTTI under memory protection*](exceptions-and-rtti-under-memory-protection.md),
explains both.

## Global constructors and boot order

When a C++ global needs code to compute its first value, that code is a
**constructor**, and it runs before `main`. The linker collects the address of
every such constructor into one table, the `.init_array` section, and the startup
code walks that table. On a desktop, the C library and the OS are already running
when that walk starts. On a microcontroller, the startup code runs right after
reset, and the order matters, because a constructor may call the OS.

Take this global from KickCAT, an EtherCAT (industrial Ethernet) stack and
KickOS's sibling project ([Chapter 1](overview-and-principles.md)):

```
static nanoseconds start_time = now();
```

`now()` reads the clock. On a microcontroller, reading the clock is a system call:
the kernel reads the timer for the program. Right after reset no thread runs yet
and the clock is not started, so this constructor cannot run there. On a core that
lets a program read its counter itself, such as a 64-bit Arm, RISC-V or x86 core,
reading the clock makes no system call
([Chapter 2.1](the-time-base-a-monotonic-clock-you-can-trust.md)).

KickOS therefore keeps two tables. The startup code walks the first one before the
kernel starts, and it stays empty: the kernel's own globals need no constructor,
and the link fails if one appears there. Every constructor of the application and
of the C and C++ libraries goes into the second table. The root thread, the first
unprivileged thread ([Chapter 0.5](the-toolchain-from-source-to-a-running-image.md)), walks it after the clock and the scheduler have
started, and before the application's `main` runs.

Where each task has an address space of its own, each task starts from a fresh
copy of the program's data, as a new process does on a desktop. So each task walks
the second table again, once, in its own address space. The first of its threads
to run does the walk, and the task's other threads wait until it is done. The walk
runs as that thread, with its priority, its stack and its permissions, so a
constructor must need nothing the task is not allowed to use.

In the sim, the second table is empty, because the host's own startup code already
runs the constructors.

## The f411 example: what printf pulls in

The f411 example's `main` is one call, `printf("hello\n")`. A compiler may replace
a constant format that ends in a newline with `puts("hello")`, but the path from
the stream down is the same. Built for the `f411disco` board, the call draws on
the pieces above in this order:

1. **newlib's `printf`.** It formats the text into `stdout`. The first use of
   `stdout` also sets up the three standard streams, `stdin`, `stdout` and
   `stderr`.
2. **The reentrancy structure.** `printf` works through the running thread's
   reentrancy structure. On a single-core Cortex-M such as the STM32F411, newlib
   finds it through one global pointer, `_impure_ptr`. KickOS keeps one
   reentrancy structure per thread and tells the kernel where they are. At every
   context switch, the kernel points `_impure_ptr` at the structure of the thread
   it switches to, so each thread has its own `errno`.
3. **A buffer, so a heap.** Before the first write, newlib asks what the stream
   is. `_fstat` answers a character device and `_isatty` answers a terminal, so
   newlib makes `stdout` **line-buffered**: it keeps the bytes until a newline
   comes. It takes that 1 KiB buffer from `malloc`, and `malloc` calls `_sbrk`. So
   a program that never calls `malloc` itself still links the allocator, through
   `printf`. If `malloc` fails, newlib leaves `stdout` unbuffered and writes one
   byte at a time.
4. **Locks.** `printf` takes the libc lock around its use of the stream, and
   `malloc` takes its own lock inside it. When no other thread holds them, neither
   waits.
5. **The newline flushes.** Since `stdout` is line-buffered, the `\n` makes newlib
   send the line in one call, `_write(1, "hello\n", 6)`.
6. **The system call.** `_write` hands the six bytes to KickOS's standard output.
   In this example, no console driver serves standard output, so the bytes go to
   the kernel console through a trap ([Chapter 0.3](interrupts-and-traps.md)). [Chapter 0.2](what-is-an-os-and-a-kernel.md) follows them from
   there to the USART2 data register.

A C++ version, `std::cout << "hello\n"`, adds the layers above newlib. libstdc++
supplies `std::cout`. By default it is kept in step with C stdio and writes
through newlib's `stdout`, so steps 3 to 6 do not change. Before `main`, the root
thread runs libstdc++'s constructor for the standard streams, and libsupc++'s
constructor for its emergency exception pool, which takes its memory from the
same heap. Exceptions and RTTI bring their tables, the unwinder and the terminate
handler ([Chapter 7.2](exceptions-and-rtti-under-memory-protection.md)).

## Under memory protection

When memory protection confines each application thread, every piece above must
sit in memory that thread may reach. The heap, the reentrancy structures and the
unwinder's state must be in memory it may write. The exception tables must be in
memory it may read, next to its code. Chapter 7.2,
[*Exceptions and RTTI under memory protection*](exceptions-and-rtti-under-memory-protection.md),
explains what the runtime touches and how each way of implementing exceptions
reaches it. Chapter 7.3,
[*Where your RAM goes*](where-your-ram-goes-full-cxx-memory-floor-and-the-linker-split.md),
shows how the linker script places each byte and where the heap's span comes from.

## Where to go next

- [*Exceptions and RTTI under memory protection*](exceptions-and-rtti-under-memory-protection.md)
  and [*Where your RAM goes*](where-your-ram-goes-full-cxx-memory-floor-and-the-linker-split.md)
  place the runtime under memory protection and measure its memory cost.
- [*Power-on*](../journey/01-power-on.md) in the Journey runs the application's
  constructors on the root thread, in a debugger.
- The exact C++ rules: the [architecture reference](../reference/architecture.md)
  ("C++ decisions").
- Where `_write`'s bytes go, exactly: the [console reference](../reference/console.md).
