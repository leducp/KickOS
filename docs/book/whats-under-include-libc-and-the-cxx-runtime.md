<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# What's under #include: the C library and the C++ runtime

> A Part-0 concept chapter (prereq: minimal C/C++ + the
> compile/link/flash pipeline of 0.5). It teaches *what a C library actually is* --
> a bag of functions plus a bottom edge -- and *what the C++ runtime is made of* --
> three stacked libraries -- so a reader understands why "just `#include <vector>`"
> is a decision with a cost, not a given. KickOS is the worked example. Points into
> `../reference/architecture.md` ("C++ decisions", "Toolchain-libc lesson from NuttX")
> for the authoritative in-repo statement; this chapter explains, that section binds.

You write `#include <cstdio>`, call `printf`, and something prints. You write
`#include <vector>`, push a million ints, and it grows. On a hosted desktop this is
invisible plumbing. On a microcontroller with 64 KiB of RAM and no operating system
underneath *until you write one*, the plumbing is the whole story: every one of those
lines lands, eventually, on a function *you* have to supply. This chapter is a map of
that plumbing -- the C library's bottom edge, the three layers of the C++ runtime, and
the choice KickOS makes about how much of it an app pays for.

*Further reading: Tanenbaum, Modern Operating Systems, ch.1 -- the operating system as
an extended machine and a resource manager, the system-call interface, and why the C
library is the layer that turns library calls into system calls. Chapter 1 is the spine
for this whole chapter; each section names the idea it draws from.*

## A C library is two things: a bag of functions and a bottom edge

Split the standard C library in half by *what it needs to work*.

- **The self-contained half.** `memcpy`, `strlen`, `strtol`, `snprintf` into a
  caller's buffer, `qsort`, the `<math.h>` functions -- pure computation. Given a
  compiler they run on bare metal with no OS at all. This is the half the C standard
  calls **freestanding**: the language plus the headers that demand nothing of an
  environment (`<stddef.h>`, `<stdint.h>`, `<limits.h>`, `<stdbool.h>`, and friends).
- **The half with a bottom edge.** `malloc` needs memory *from somewhere*. `printf`
  to `stdout` needs a *sink*. `exit` needs somewhere to *go*. `fopen` needs *files*.
  These cannot be pure computation -- they are requests to an operating system. The C
  standard calls a library that provides *all* of it, stdio and heap and `exit` and
  the rest, **hosted**. (Tanenbaum ch.1: these are exactly the system-call surface --
  process control, file I/O, memory.)

The crucial fact for anyone porting a libc to a new system: that second half is not
re-implemented per platform. A portable libc (newlib is the canonical one) implements
`malloc`, `printf`, `fprintf`, buffering, all of it, *once*, in terms of a **tiny set
of primitive calls it expects the platform to supply**. Porting the library to KickOS
is porting that set -- nothing more:

| Primitive | What the library needs it for |
|---|---|
| `_sbrk(incr)` | grow the heap arena; `malloc`/`operator new` carve from it |
| `_write(fd, buf, len)` | the sink for `printf`/`fputs`/`std::cout` |
| `_read` `_close` `_lseek` `_fstat` `_isatty` | the file model stdio is written against |
| `_exit` `_kill` `_getpid` | process control that `exit`/`abort` bottom out in |
| `errno` via `_impure_ptr` (newlib's reent) | per-context error state stdio and math set |
| `__malloc_lock` / `__malloc_unlock` | bracket every arena mutation *when threaded* |

That is the entire porting seam. Everything else in the C library is written above it,
platform-independent. Get these right and `#include <cstdio>` works; the library never
knew it moved.

## KickOS's bottom edge

The seam lives in `user/src/newlib_stubs.cc`, with one deliberate exile in
`user/src/newlib_sbrk.cc` that the heap bullet below explains. It is small on purpose --
each stub is the thinnest possible bridge from a newlib expectation to a KickOS syscall:

- **`_write` -> the debug console.** Pre-driver, all stdio (any `fd`) falls to
  `kos_kconsole_write`; KickOS has no `fd` namespace yet, so `fd` is ignored. Once a
  userspace console driver exists, stdout routes to it instead.
- **`_sbrk` over a bump arena whose bounds are linker symbols.** The heap is not a
  static array. `_kickos_heap_start` and `_kickos_heap_limit` are defined by the chip's
  linker script, and `_sbrk` bumps a pointer between them, refusing (`-1`) when the next
  bump would leave the span. There is no `free` back to the OS; `malloc`'s own free list
  reuses within the arena. Where those two symbols come from is the whole story, and
  Chapter 7.3 tells it: on a protected chip the heap is the *unused pad* of the one
  window already granted to the app, so a bigger heap is a bigger grant rather than a
  bigger array. That is also why this one stub lives in its own translation unit, and the
  reasoning is worth following because it is a design *choice* about where a heapless
  board should hear about it. The stubs file proper is force-linked into every image, so
  a strong reference to `_kickos_heap_start` from there would resolve-or-fail in every
  link, heap or no heap. Kept apart, `_sbrk`'s object is pulled only by a real allocator
  reference, which leaves the diagnosis to the linker script: a script that defines
  neither bound turns an allocating app into a **link** error on an undefined
  `_kickos_heap_start`, and a script that defines an empty span lets the same app link
  and take a NULL from `malloc` at run time. The chip scripts in this tree all take the
  second option, the zero-heap board explicitly so
  (`../../arch/arm/chip/nrf51/nrf51.ld` keeps a zero-length `.userheap` section for no
  other reason than to define the two symbols). Either answer is defensible; what the
  separate translation unit buys is that the board gets to pick one, rather than having
  the link error forced on every image or made unreachable for all of them.
- **`__malloc_lock` as a no-op -- and the footgun.** newlib brackets every arena
  mutation with `__malloc_lock`/`__malloc_unlock`. The pinned vendor toolchains are all
  built single-thread (`--disable-threads`), so KickOS provides **weak, empty** stubs:
  a full-C++ app that allocates from one thread links and runs with zero locking
  overhead, and a future thread-safe libc can override the weak symbols. The footgun,
  called out in the file: an app that spawns threads and heap-allocates from *more than
  one* gets no reentrancy guard -- concurrent `malloc` silently corrupts the arena.
  Real stubs (an `IrqLock` or a per-domain arena) are the prerequisite for
  multi-threaded full-C++ allocation; without them, keep such apps single-alloc-thread.

The rest (`_read` returns 0, `_close`/`_kill` return `-1`, `_isatty` returns 1) are
honest stubs for a system with no filesystem: enough for the library to be *coherent*,
not enough to pretend files exist. The sim never compiles this file -- host glibc
already owns these symbols.

## Why "newlib" is the through-line of the whole fleet

KickOS runs one uniform design across every ISA in the fleet on **pinned vendor toolchains
that are all newlib** -- Arm GNU Toolchain (ARM), RISCStar (RISC-V), GNURX (RX) and one per
remaining family, each pinned in exactly one place ([`../reference/porting.md`](../reference/porting.md)).
That is not a
coincidence to shrug at -- it is what keeps the seam *singular*. Because every
toolchain's libc is newlib, the bottom edge above is the **same set of symbols on every
arch**: one `newlib_stubs.cc` serves the whole fleet, with no per-toolchain libc
variant to maintain. And each toolchain ships *its own* newlib-built `libstdc++`, so the
C++ runtime always rides the libc it was compiled against (the point of the NuttX
section below). One seam, many chips -- the uniform-fleet thesis applied to the libc.

## The C++ runtime is three stacked libraries

A C++ program that throws, allocates, and uses the STL does not link against one "C++
library." It links against **three**, bottom to top, each depending only on the ones
below it:

1. **libgcc -- the unwinder and compiler runtime.** The bottom layer, and it is
   *libc-independent*. It provides the stack unwinder (`_Unwind_RaiseException`,
   `_Unwind_Resume`, the `_Unwind_*` family) that walks frames when an exception
   propagates, plus the frame-description-entry (FDE) machinery that says how to unwind
   each frame, plus integer/float helpers the compiler emits calls to. It knows nothing
   about `std::` or even about C++ types -- it is the mechanism, not the policy.
2. **libsupc++ -- the C++ ABI support layer.** The `__cxa_*` runtime that the *language*
   compiles down to: `__cxa_throw`, `__cxa_begin_catch`, `__cxa_allocate_exception`,
   the `eh_globals` (per-context in-flight-exception state), the **emergency pool** that
   lets `throw std::bad_alloc` work when the heap is exhausted, `__new_handler`, and the
   default `operator new`/`operator delete`. It sits on libgcc's unwinder and turns raw
   frame-walking into typed C++ exception semantics (matching a `catch` to a thrown
   type, running destructors on the way out) and RTTI (`typeid`, `dynamic_cast`).
3. **libstdc++ -- the standard library.** The part you actually `#include`: containers,
   `<algorithm>`, iostreams, `std::string`, locale. It is built *on top of* libsupc++
   (it throws, so it needs the ABI layer) and the C library (it allocates and does
   I/O). This is the biggest layer and the one whose optional pieces -- locale in
   particular -- cost the most code.

The dependency direction is strict and worth holding: **libstdc++ -> libsupc++ ->
libgcc -> libc**. Exceptions are a *libgcc + libsupc++* feature; the STL is
libstdc++; RTTI is libsupc++. You can have the bottom layers without the top (that is
freestanding C++, next), but never the reverse.

## Freestanding C++ vs full C++ -- and what full costs

C++ has the same freestanding/hosted split as C, and KickOS uses it as the axis of a
deliberate cost decision.

- **Freestanding C++ is the default, on every arch, at zero overhead.** Kernel, arch,
  lib, and every plain app compile `-fno-exceptions -fno-rtti` and link `-nostdlib++`
  (the exact set is `KICKOS_FREESTANDING_CXX_FLAGS` in `cmake/kickos.cmake`:
  `-fno-exceptions -fno-rtti -fno-threadsafe-statics -fno-use-cxa-atexit`). You get
  classes, templates, RAII, constexpr -- the whole *language* minus the two features
  that need a runtime. No `.eh_frame`, no unwind tables, no libsupc++, no libstdc++.
  The freestanding subset of the headers (`<type_traits>`, `<utility>`, `<array>`, ...)
  is still available because it needs no runtime.
- **Full C++ is a per-app opt-in, and it is not free.** An app built `FULL_CXX` compiles
  its C++ TUs `-fexceptions -frtti` and *drops* the `-nostdlib++`, so the toolchain's
  own `libstdc++`/`libsupc++` join the link. Now you have exceptions, RTTI, and the STL
  -- and you pay for them: the EH/unwind tables (`.eh_frame` and `.gcc_except_table` on
  DWARF arches, `.ARM.exidx`/`.ARM.extab` on ARM) run **~10-15 KiB** of read-only data,
  plus the libsupc++/libstdc++ code, plus locale if something (`std::stringstream`,
  `std::to_string`) drags it in. That cost is libc-independent -- it is the price of the
  feature, not of a brand.

The plumbing that flips between the two is which target the app links. The exported
package ships three interface targets over a posture-neutral `kickos_core`: an app links
`kickos` for the freestanding default (`-fno-exceptions -fno-rtti`, `-nostdlib++`) or
`kickos_cxx` for full C++ (`-fexceptions -frtti`, `libstdc++`/`libsupc++` kept). A
misspelled leaf is a hard link error, not a silent freestanding downgrade. The kernel and
libs are clamped freestanding directly via `kickos_apply_freestanding()` and are never
consumers. (`kickos_add_application(... FULL_CXX)` remains as sugar and just selects the leaf.)

## The cross-libc trap: never host toolchain C++ on your own libc

Here is the mistake this layering is built to avoid. It is tempting to write your *own* small
libc (KickOS has one, for the freestanding default) and then link the *toolchain's*
`libstdc++` on top of it -- one libc to maintain, full C++ for free. It does not work,
and the reason is instructive.

The toolchain's `libstdc++` was **compiled against the toolchain's own libc** (newlib).
Its headers and the libc's headers agree on subtle shared types and declarations --
`div_t`/`ldiv_t`, what `std::abs` in `<cmath>` resolves to, the locale facets, the
`_impure_ptr` reent that `vterminate` reaches for. Host that pre-built `libstdc++` over
a *different* libc whose headers disagree even slightly, and you get a steady stream of
**header/type/namespace ABI mismatches** -- `<cstdlib>` fighting your `<stdlib.h>` over
`div_t`, `<cmath>` failing to find the C math it expects. NuttX ships its own
deliberately-non-newlib libc and offers `CONFIG_LIBCXXTOOLCHAIN` for the toolchain
runtime; that configuration is where the mismatch class shows up, which is how it is
documented. (This is the "Toolchain-libc lesson from NuttX" in
`../reference/architecture.md`.)

**KickOS's rule: never put toolchain C++ on our own libc.** A full-C++ app links
`libstdc++`/`libsupc++` over the **libc they were built against** -- newlib, the same
newlib on every arch. Zero cross-libc shim, zero ABI mismatch, because the runtime rides
its native libc. Our own freestanding libc is kept newlib-*family*-compatible for a
different reason -- so the sim can ride host `libstdc++` cleanly -- but it never hosts
the toolchain C++ runtime.

### The boundary discipline, and how it fails safe

Mixing `-fexceptions` app code with `-fno-exceptions` kernel/lib/arch code is sound not
by luck but by *how it fails*. Only the app is `-fexceptions`; everything below has no
unwind tables at all. The only kernel-to-app entries are the app `main` and the thread
trampoline, and the IRQ model is wait-on-semaphore (no kernel-invoked app callbacks), so
there is no throw path threading back through an ISR. When an exception *escapes* an app
entry -- an uncaught throw, a `bad_alloc`, a throwing global constructor -- it reaches a
frame with **no FDE / no `.exidx` entry**. Both the DWARF and the ARM-EHABI unwinders
fail in *phase 1* (the search phase), **before any frame is popped**, so `__cxa_throw`
goes straight to `std::terminate -> abort -> kos_exit` with no half-unwound kernel state
left behind. Consequences an app author must know: catch your own exceptions; an escape
from a worker thread terminates the *whole image*, not just the thread; static
destructors and `atexit` do not run on the normal return path.

### The terminate handler: what an uncaught throw drags in, and trimming it

When an uncaught exception reaches `std::terminate`, libstdc++'s *default* terminate
handler is `__gnu_cxx::__verbose_terminate_handler` -- and it tries to be helpful in a
way that only makes sense on a hosted system: it **demangles** the exception's type name
and prints it, along with `what()`, before aborting. That politeness is expensive on a
freestanding target. Pulling in that one handler drags in `__cxa_demangle` -- a full
Itanium-ABI name demangler -- and, through it, newlib's floating-point `printf`/`dtoa`:
tens of kilobytes of code the embedded image never calls for any purpose other than
pretty-printing the message of a crash it is about to `abort` on. A microcontroller has a
debugger on the fault, or it has nothing; either way it has no reader for that line.

The lever that removes it is a property of **how a linker pulls objects out of an
archive**: a member is extracted only to satisfy a symbol that is otherwise *undefined*.
`__verbose_terminate_handler` lives in its own object (`vterminate.o`) inside
`libstdc++.a`. If the program already provides a **strong definition** of that symbol,
the reference is satisfied before the archive is even scanned, the member is never
extracted, and the demangler + float-`dtoa` it would have pulled in never enter the link.
So a freestanding runtime supplies its own **lean** handler -- print the *mangled* type
name and `what()`, then `abort` -- and reclaims the whole demangler/`dtoa` tail. Exceptions
still throw and catch exactly as before; only the message printed on an *uncaught* abort
changes, from a demangled type name to a mangled one. On the tightest parts that reclaimed
tail is the difference between a full-EH+RTTI+STL image fitting in flash and not. It is the
same principle as the freestanding/full split that opened this section, applied one symbol
at a time: pay only for the runtime you actually use, and a demangled abort message is a
line item a headless system can decline.

## When the runtime starts: global constructors and boot order

The C++ runtime does not only sit *on* the OS -- it assumes the OS is already *running*
when it starts. Global constructors (the functions the compiler collects in
`.init_array`) run before `main`, and the language guarantees they run in a live world:
on a hosted system the C runtime start-up brings libc up first, then walks `.init_array`,
then calls `main`. Static initialization never sees a half-built environment.

A freestanding OS gets no such guarantee for free -- it has to *reconstruct* that
ordering by hand, and KickOS learned it the hard way. Its reset path ran `.init_array`
straight out of `Reset_Handler`, *before* `kmain` brought up `arch_init`, `ktime_init`,
and the scheduler. That is harmless for a constructor that only touches memory, and a
trap for one that touches the OS. KickCAT has exactly such a constructor:

```
static nanoseconds start_time = now();
```

`now()` bottoms out in a KickOS clock syscall (`kos_clock_now`, an `SVC`). The syscall
floor bound-checks its out-pointer against the *current thread* (the confused-deputy
floor) -- but at static-init time, at boot, there is no current thread yet. The result
was a fault *before* `main`, before even the banner printed: silent, and baffling until
you see that a library global ran an OS call before the OS existed.

The fix is to **split `.init_array`** so the runtime starts in the order the language
expects. Kernel/arch/chip/lib constructors -- the ones `kmain` itself needs -- stay in
`.init_array` and run early from `Reset_Handler`. *Every other* constructor (the app's,
libstdc++/libsupc++'s, KickCAT's) is routed by the linker into a separate
`.kickos_app_init_array` section and run later from `root_entry` -- the kernel's first
thread, with the scheduler and clock fully live -- just before `kickos_app_main`. That
restores the normal C++ contract: runtime up, then static init, then `main`. The split is
fanned out across every chip linker, and the bounds `root_entry` walks are a *strong*
reference: every target has to state its window, so a chip whose script forgot to
partition `.init_array` fails to link instead of quietly skipping the walk. The sim host
-- whose own runtime already sequences `.init_array` before `main` -- states an
explicitly empty one, which the walk steps over in zero iterations. See
`../../kernel/init/kmain.cc` (`root_entry`), `../../arch/sim/start.cc`,
`../../user/include/kickos/app.h`, and the `.init_array` / `.kickos_app_init_array` split
in `../../arch/arm/chip/mk64f/mk64f.ld`.

This is the exact mirror of the bottom-edge story this chapter opened with. There, the
runtime needs the OS *underneath* it -- `malloc` needs `_sbrk`, `printf` needs `_write`.
Here, the OS must *sequence* the runtime above it: a freestanding kernel has to stand its
own syscalls, clock, and thread context up before it runs the library and app
constructors that may call into them. Get the bottom edge wrong and the library cannot
reach the OS; get the boot order wrong and the OS is not there yet when the library
reaches for it.

## The MPU twist: the runtime's writable state must be granted

Under per-task MPU isolation (Chapter 7), one extra fact matters: the C++ runtime's
**writable state must land in a region the isolated thread was granted**, while its
read-only tables come along for free. An unprivileged thread running the full runtime --
exceptions, RTTI, the STL all live -- reaches all of it only because every writable
runtime global was placed inside one of its granted regions; a full-C++ app (linking
`kickos_cxx`) runs out of its own granted regions with the protection unit enforcing on
every access.

- **Writable, must be in the granted data region:** the libc heap arena, the
  newlib malloc bins and reent (`_impure_*`), libgcc's FDE registry list heads, and
  libsupc++'s `eh_globals` + emergency pool. If any of these land kernel-side, the
  unprivileged thread faults the first time the runtime touches them.
- **Read-only, rides the code region for free:** the EH tables (`.eh_frame` /
  `.gcc_except_table`, or `.ARM.exidx` / `.ARM.extab`), RTTI `type_info` and vtables
  (`.rodata`), and landing pads (`.text`). All of these already sit inside the per-app
  code region that is granted read+execute, so unwinding *reads* them with no extra
  region. The full EH+RTTI+heap footprint costs **zero additional MPU regions**.

How that split is *expressed* is one linker file, `../../arch/arm/chip/mk64f/mk64f.ld`,
and it carries a gotcha worth the ink. The scheme is **inverted and fail-safe**: the
closed KickOS-owned archive set (`libkickos_{kernel,arch,chip,lib}.a`) is captured
*first* into the kernel's `.data`/`.bss`; the `.appdata`/`.appbss` catch-alls that follow
take *everything else* -- the app's own objects, the toolchain runtime (newlib, libgcc,
libstdc++, libsupc++), and KickCAT. An unmatched archive therefore lands app-side
(reachable, it works), never kernel-side (where it would fault). The app-granted
`.appdata` window ends up holding exactly the runtime's writable state -- the heap,
newlib's `_impure_ptr`/`__malloc_av_`, and libsupc++'s `eh_globals` + emergency pool --
while the read-only EH tables (`.ARM.exidx`/`.ARM.extab`), `type_info`, and vtables ride
flash. Isolation is checkable with `nm`: no kernel writable symbol may sit inside the app
window. The gotcha: `EXCLUDE_FILE` looks like the natural tool for the split, but **this
binutils (arm-none-eabi 15.3) does not match archive members inside `EXCLUDE_FILE`** -- a
bare `*libkickos_kernel.a` there matches nothing. The mechanism that *does* work is
colon-inclusion, `*libkickos_kernel.a:*(...)`, which selects members by archive; so
"exclude the kernel from the app window" becomes "include the kernel first, let the
rest fall through." Same isolation, opposite selector.

Two arch wrinkles worth naming:
- **DWARF vs ARM-EHABI FDE registration.** On the DWARF arches the `.eh_frame`
  table must be *registered* at runtime via `__register_frame` -- KickOS does it at boot
  in the privileged reset handler, so the registry node is allocated while privileged and
  lands in the granted heap. That is the RISC-V story, and it is the story any other
  DWARF-unwinding arch would take; RX is not a case of it at all, because the GNURX
  toolchain unwinds by setjmp/longjmp and has no `.eh_frame` to register
  (Chapter 7.2 has all three models side by side). ARM-EHABI needs **no
  runtime registration at all**:
  `.ARM.exidx` is found through linker-defined symbols, sidestepping the whole issue --
  the K64F proof leaned on exactly this, with no boot `__register_frame` in the picture.
- **The RISC-V `gp` wrinkle.** RISC-V's small-data optimization addresses globals relative
  to the `gp` register, and the flag used to route app globals out of the kernel-side `gp`
  window (`-msmall-data-limit=0`) *breaks* DWARF unwinding on a `-fexceptions` TU. The fix
  is to stop fighting `gp` and move it: anchor `__global_pointer$` inside the granted data
  region so small-data lands in-region and unwinding keeps working. This is RISC-V-only;
  ARM and RX have no `gp` small-data model. The full account is the companion chapter
  [*Exceptions and RTTI under memory protection*](exceptions-and-rtti-under-memory-protection.md);
  the porting contract is `../reference/porting.md` (RISC-V arch, `gp` anchor).

## Where to go next

- The authoritative in-repo contract: `../reference/architecture.md` -- the "C++ decisions",
  "Memory domains", and "Toolchain-libc lesson from NuttX" sections.
- Full C++ under memory protection, in depth: the companion chapters
  [*Exceptions and RTTI under memory protection*](exceptions-and-rtti-under-memory-protection.md)
  and [*Where your RAM goes*](where-your-ram-goes-full-cxx-memory-floor-and-the-linker-split.md).
- The libc strategy and the newlib seam: `../reference/architecture.md` ("C++ decisions").
- The actual bottom-edge stubs: `../../user/src/newlib_stubs.cc`.
- Memory protection, which decides where the runtime's writable state must live:
  Chapter 7, *Memory protection*.
