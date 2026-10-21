<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# What's under #include: the C library and the C++ runtime

> What does `printf("hello\n")` need underneath it, on a chip with no terminal and
> no files?
>
> You will learn:
>
> - what the C library and the C++ runtime are made of;
> - what each piece asks of the operating system;
> - how the f411 example's `printf` travels from the application to the kernel.

On a desktop, `printf` and `std::vector` can look self-contained. On an MCU,
their dependencies matter: output needs a sink, allocation needs memory, and
full C++ brings runtime code and data.

*Further reading: Tanenbaum, Modern Operating Systems, ch.1 (the system-call
interface and the OS as a resource manager).*

## What a C library needs from the OS

The standard C library has functions with different needs:

- **Self-contained operations** such as `memcpy`, `strlen`, and `qsort` work
  without an OS. A **freestanding** C environment guarantees only a limited set
  of headers and facilities; implementations may provide more.
- **Environment-dependent operations** need services underneath: `malloc` needs
  an arena (the block of memory it carves allocations from), `printf` needs
  output, `fopen` needs files, and `exit` needs a way to terminate. A
  **hosted** C environment provides the full standard library.

A portable libc such as newlib implements most of the second group itself. The
platform supplies a small set of primitives underneath it:

| Primitive | What the library needs it for |
|---|---|
| `_sbrk(incr)` | grow the heap arena; `malloc`/`operator new` carve from it |
| `_write(fd, buf, len)` | the sink for `printf`/`fputs`/`std::cout` |
| `_read` `_close` `_lseek` `_fstat` `_isatty` | the file model stdio is written against |
| `_exit` `_kill` `_getpid` | process control that `exit`/`abort` bottom out in |
| `errno` via `_impure_ptr` (newlib's reent) | per-context error state stdio and math set |
| `__retarget_lock_*` | the locks around every arena mutation and every stream, *when threaded* |

newlib keeps `errno` and its other per-thread state in a `struct _reent`, its
reentrancy structure, so that it can be reentrant: safe in two threads at once,
since each keeps its own state. These primitives are the main porting seam
between newlib and KickOS.

## KickOS's bottom edge

Most stubs live in `user/src/newlib_stubs.cc`; `_sbrk` lives separately in
`user/src/newlib_sbrk.cc`. Each bridges a newlib expectation to KickOS:

- **`_write` reaches the console.** It ignores `fd`, since KickOS has no file
  descriptor namespace, and hands the bytes to `kickos::stdout_write`
  (`user/include/kickos/sys/emit.h`): a send to the console driver where one
  serves stdout, the kernel's debug console (`kos_kconsole_write`) otherwise.
  `../reference/console.md` has the exact routing.
- **`_sbrk` grows a linker-bounded heap.** The chip linker script defines
  `_kickos_heap_start` and `_kickos_heap_limit`; `_sbrk` advances within that
  span and fails at the limit. `malloc` can reuse freed blocks inside the arena,
  but does not return the arena to the OS. On a protected chip, the heap occupies
  unused space in the app's granted data window (Chapter 7.3).
- **`_sbrk` is linked only when needed.** The main stubs file is force-linked;
  keeping `_sbrk` separate means a heapless app need not resolve heap bounds.
  An allocating app on a board with no bound symbols gets a link error. Current
  chip scripts instead define a span, even if empty, so allocation can fail at
  runtime. The nRF51 script uses a zero-length `.userheap` for this purpose.
- **The C library takes two recursive locks per address space: malloc's, and
  one for everything else.** A recursive lock is a lock its holder may take
  again; an address space is a task's protected memory (Chapter 0.2). newlib's
  retargetable locks (`__retarget_lock_*`) map malloc's lock to one mutex and
  every other lock, each stream's included, to a second, so a `printf` never
  waits behind another thread's allocation, unless that `printf` must allocate
  itself. Each mutex is a word in the static data of whatever owns the heap,
  the task's address space or the whole image, so each address space has its
  own pair. The word names its holder, and the depth sits beside it, so taking
  a free lock, taking one the thread already holds and giving it back all cost
  no syscall, and nesting two stdio locks is recursion on the one. Threads of one address space share one heap and one `stdout`, so two
  of them can `malloc` or `printf` at once safely.
  The exact contract is `libc-locks-belong-to-the-heap-owner` in
  `../reference/invariants.md`.

Other stubs reflect the lack of a filesystem: `_read` returns 0,
`_close`/`_kill` return `-1`, and `_isatty` returns 1. The sim, KickOS built as
a Linux program, uses host glibc instead of these stubs.

## Why the cross-compiled fleet uses newlib

Every cross build takes its compiler from the KickOS toolchain (Chapter 0.5),
and that toolchain carries newlib, built from the same pinned sources for every
family and multilib. One symbol interface therefore lets one `newlib_stubs.cc`
serve the whole cross-compiled fleet. At configure time,
`cmake/cross_newlib.cmake` checks that the newlib the compiler flags select
matches what the board asks for: its reentrancy, and the full or the smaller
"nano" build.

The hosted **sim** uses the host libc and does not compile the stubs. A
freestanding target with **no C library** has no newlib seam to fill. These
postures explain why reentrancy support is selected separately from the
architecture. Board-specific choices live in
[`../reference/boards.md`](../reference/boards.md).

## The C++ runtime is three stacked libraries

A full C++ program draws on three runtime layers:

1. **libgcc:** compiler helpers and the unwinder, the code that walks the stack
   frames (Chapter 0.1, *[A call and the stack](the-machine-underneath.md#a-call-and-the-stack)*)
   back to a matching `catch` during an exception. It provides mechanism, not
   C++ types or `std::` facilities.
2. **libsupc++:** C++ ABI support for throwing, catching, RTTI (run-time type
   information, the type descriptions `typeid` and `dynamic_cast` read),
   `operator new`, and emergency exception storage. It uses libgcc's unwinder.
3. **libstdc++:** containers, strings, algorithms, streams, and locale. It uses
   libsupc++ and the C library for allocation and I/O.

The standard library sits above ABI support and compiler runtime. Exceptions
need libgcc and libsupc++; containers and streams need libstdc++.

## Freestanding and full C++

KickOS uses different C++ runtime levels for the kernel and applications:

- **The kernel and its libraries use freestanding C++.** They compile with
  `-fno-exceptions -fno-rtti -fno-threadsafe-statics -fno-use-cxa-atexit` and
  link with `-nostdlib++`. Classes, templates, RAII (a destructor releasing what
  its constructor acquired), and runtime-free headers remain available;
  exceptions, RTTI, and the standard library runtime do not.
  See `KICKOS_FREESTANDING_CXX_FLAGS` in `cmake/kickos.cmake`.
- **Applications can use full C++.** They enable exceptions and RTTI and link
  the toolchain's `libsupc++` and `libstdc++`. Exception tables can add roughly
  10-15 KiB of read-only data, plus runtime code and optional facilities such as
  locale. Chapter 7.3 measures the memory cost.

An app links `KickOS::kernel` and a system target that supplies startup. The
kernel and its libraries are compiled freestanding by
`kickos_apply_freestanding()`; the app uses the full C++ settings.

## Match the C++ runtime to its C library

The toolchain's `libstdc++` must be paired with a compatible C library. Linking
it over KickOS's small freestanding libc risks ABI and header mismatches.

`libstdc++` is compiled against one C library's headers and ABI. A different
libc may define shared types or functions differently: `<cstdlib>`
can disagree with `<stdlib.h>` over `div_t`, and `<cmath>` can miss expected C
math declarations. The NuttX experience is recorded under "Toolchain-libc
lesson from NuttX" in `../reference/architecture.md`.

Full-C++ apps therefore link `libstdc++` and `libsupc++` over the newlib they
were compiled against, the one in the same KickOS toolchain, for the board's
multilib. KickOS's freestanding libc does not host that runtime.

Where an exception thrown in app code may travel, and the smaller terminate
handler KickOS supplies, are in Chapter 7.2,
[*Exceptions and RTTI under memory protection*](exceptions-and-rtti-under-memory-protection.md).

## Global constructors and boot order

Global constructors run before `main`. On a hosted system, startup initializes
the C runtime first. The linker collects the addresses of every global
constructor into one table, the `.init_array` section, which startup walks.
KickOS must arrange that order itself: it once ran every `.init_array`
constructor from `Reset_Handler`, before the clock and scheduler were ready.
KickCAT, KickOS's sibling EtherCAT (industrial Ethernet) stack (Chapter 1), had a constructor that
called the clock:

```
static nanoseconds start_time = now();
```

`now()` calls `kos_clock_now`, whose pointer check needs a current thread. At
that point in boot there was none, so the constructor faulted before `main`.

KickOS therefore splits `.init_array`. Kernel, arch, chip, and library constructors
needed by `kmain` run early. App and toolchain-runtime constructors go into
`.kickos_app_init_array` and run from `kickos_root_entry`, the entry of the root
thread, the first unprivileged thread (Chapter 0.5), after the clock and
scheduler start and before `kickos_app_main`. Where each task has an address
space of its own, a task starts from the program's untouched data, as a new
process does, so it walks the same table again in its own space, once, from
`kickos_task_start`: the first of its threads to run does, and the others wait
for it. That walk runs as that thread, with its priority, its stack and its
authority, so a constructor must need nothing the task is not granted. A
strong reference to the section bounds makes a missing linker-script
definition a link error. The sim defines an empty app array because its host
runtime already runs constructors. See
`../../user/src/root_entry.cc` (`kickos_root_entry`), `../../arch/sim/start.cc`,
`../../user/include/kickos/app.h`, and the `.init_array` / `.kickos_app_init_array` split
in `KICKOS_CORTEXM_CTORS`, the body every Cortex-M chip script shares
(`../../arch/arm/common/cortexm.ld.h`).

The ordering matters for the same reason as the libc stubs: constructors may
call OS services, so those services must exist before the constructors run.

## The f411 example: what printf pulls in

The f411 example's `main` is one call, `printf("hello\n")`. (A compiler may
rewrite a constant format ending in a newline as `puts("hello")`; the path from
the stream down is the same.) Linked with `KickOS::kernel` for `f411disco`, the
call draws on the pieces above in this order:

1. **newlib's `printf`.** It finds the running thread's reentrancy structure
   through `_REENT` and formats into `stdout` with `_vfprintf_r`. Formatting is
   a self-contained operation; the steps below need the OS. The first use of
   `stdout` runs `__sinit`, which sets up the three standard streams.
2. **The reentrancy structure.** On a single-core Cortex-M such as the
   STM32F411, newlib reaches it through one word, `_impure_ptr`. KickOS keeps
   one `struct _reent` per thread slot (`s_reent` in
   `../../user/src/newlib_reent.cc`) and describes them to the kernel through
   `kickos_reent_seam`. The kernel rewrites `_impure_ptr` at every context
   switch, so each thread has its own `errno`.
   `../../kernel/include/kickos/reent.h` states the seam.
3. **A buffer, so a heap.** Before the first write, newlib asks what the stream
   is. `_fstat` answers a character device and `_isatty` answers yes, so newlib
   makes `stdout` line-buffered and `malloc`s a 1 KiB buffer for it: `malloc`
   calls `_sbrk`, which carves the span between `_kickos_heap_start` and
   `_kickos_heap_limit`. A program that never calls `malloc` still links the
   allocator through `printf`. If `malloc` fails, newlib leaves `stdout`
   unbuffered and writes one byte at a time.
4. **Locks.** `printf` brackets the stream with the address space's libc lock,
   and `malloc` brackets the heap with its own lock inside it. newlib asks the
   OS for each lock, and KickOS takes a free one with no kernel entry; the nested
   takes that follow inside the same call are counted in the app's memory and
   cost no syscall either.
5. **The newline flushes.** `stdout` is line-buffered, so the `\n` makes newlib
   flush the line in one call, `_write(1, "hello\n", 6)`.
6. **The syscall seam.** `_write` hands the six bytes to
   `kickos::stdout_write`. The example's composition says `stdout: kernel`
   (`../../boards/f411disco/composition.yaml`): no console driver serves stdout,
   so the bytes go to the kernel console through `kos_kconsole_write`, a trap.
   Chapter 0.2 follows them from there to the USART2 data register.

A C++ version, `std::cout << "hello\n"`, adds the layers above newlib.
libstdc++ supplies `std::cout`; synchronised with C stdio by default, it writes
through newlib's `stdout`, so steps 3 to 6 do not change. Before `main`,
`kickos_root_entry` runs libstdc++'s constructor for the standard streams and
libsupc++'s emergency exception pool, whose constructor takes its buffer from
the same heap. Exceptions and RTTI bring their tables, the unwinder, and the
terminate handler (Chapter 7.2).

## Under memory protection

When the MPU confines each app thread, an unprivileged thread must reach every
piece above: the heap, the reentrancy structures and the unwinder's state through
its data grant, the tables through its code grant. Chapter 7.2,
[*Exceptions and RTTI under memory protection*](exceptions-and-rtti-under-memory-protection.md),
explains what the runtime touches and how each exception model reaches it.
Chapter 7.3,
[*Where your RAM goes*](where-your-ram-goes-full-cxx-memory-floor-and-the-linker-split.md),
shows how the linker script places each byte and where the heap span comes from.

## Where to go next

- `../reference/architecture.md` states the C++ and memory-domain contracts.
- `../reference/console.md` states where `_write`'s bytes go.
- [*Exceptions and RTTI under memory protection*](exceptions-and-rtti-under-memory-protection.md)
  and [*Where your RAM goes*](where-your-ram-goes-full-cxx-memory-floor-and-the-linker-split.md)
  place the runtime under memory protection and measure its memory cost.
- [*Power-on*](../journey/01-power-on.md) in the Journey runs the app
  constructors on the root thread in a debugger.
- `../../user/src/newlib_stubs.cc` contains the libc stubs.
