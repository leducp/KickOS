<!--
SPDX-License-Identifier: CECILL-C
Copyright (c) 2026 Philippe Leduc
-->
# Where your RAM goes: the full-C++ memory floor and splitting kernel from app in the linker

> A Chapter-7 companion, downstream of the C++ runtime chapter 0.6
> ([`whats-under-include-libc-and-the-cxx-runtime.md`](whats-under-include-libc-and-the-cxx-runtime.md))
> and the runtime-memory chapter
> ([*Exceptions and RTTI under memory protection*](exceptions-and-rtti-under-memory-protection.md),
> which established *what* writable state the runtime keeps). This chapter answers two
> practical questions a porter and an app author both ask: **how much writable RAM does a
> full-C++ app cost, and why**, and **how is one linked image cut into a kernel side and
> an app side so a memory-protection unit can fence them apart**. For the exact contract
> it binds into the Reference: [`../reference/architecture.md`](../reference/architecture.md)
> (the region-set model and budget) and [`../reference/porting.md`](../reference/porting.md)
> (the RISC-V `gp` anchor); the worked linker example is
> [`../../arch/arm/chip/mk64f/mk64f.ld`](../../arch/arm/chip/mk64f/mk64f.ld). This chapter is
> the LAYOUT: which side of the wall each byte lands on.

A freestanding KickOS app pays almost nothing for its writable state: a few globals, its
stack. Flip on full C++ -- `-fexceptions -frtti`, the toolchain's `libstdc++`/`libsupc++`
in the link -- and a floor appears under the app's RAM that was not there before. This
chapter is about that floor: what sits on it, why it is roughly the size it is, and then
the linker mechanics that decide which side of the protection boundary every byte of it
lands on.

*Further reading: Tanenbaum, Modern Operating Systems, ch.3 (memory management -- how an
address space is laid out and protected). The linker script here is exactly the tool that
authors that layout by hand on a system with no loader to do it at runtime.*

## The writable floor: what actually sits on it

The runtime-memory chapter listed the writable pieces; here is what they *cost*. A
full-C++ app's writable RAM has two contributors, and they are smaller than intuition
suggests:

- **The heap** -- what `malloc`/`operator new` hand out. This is *dynamic*: it is whatever
  the app allocates and has not returned to `malloc`'s free list. It is not a fixed buffer
  the OS provisions up front; it is bounded only by the RAM left over for it (the next
  section). A useful anchor for the ceiling: a full-EH/STL `cxxtest` run -- exceptions
  thrown and caught, RTTI, `std::string`/container churn -- peaks near **6 KiB** of live
  heap. Most freestanding-plus-a-little-STL apps sit well under that.
- **The runtime's own writable globals** -- newlib's reent (`_impure_ptr`) and malloc
  bins, libsupc++'s `eh_globals` + emergency pool + handler pointers, and (DWARF arches
  only) the FDE registry heads and node. Call it the *runtime writable tax*: a few KiB,
  fixed, paid the moment `libstdc++`/`libsupc++` join the link.

So the floor is modest: a few KiB of fixed tax plus a working-set heap that, even for a
heavy C++ test, tops out in single-digit KiB. What makes it a *layout* problem rather than
a sizing problem is not how big it is -- it is that every byte of it must land on the app
side of the protection wall, and the heap must have somewhere to grow.

## The EH cost is per-toolchain, not per-board

A porter's instinct is that exceptions cost the same everywhere. They do not -- the cost,
and *where* it lands, is a property of the exception model (Chapter above), which is a
property of the toolchain, not the chip:

- **EHABI (ARM: K64F, XMC, RP2040)** -- cheapest. The unwind tables (`.ARM.exidx`/`.extab`)
  are compact read-only data in the code region; there is no FDE registry and no writable
  registration state. The writable tax is just `eh_globals` + the emergency pool.
- **SjLj (RX: RX72M)** -- the read-only `.gcc_except_table` (LSDA) in ROM, plus a writable
  SjLj context chain in the app window. No `.eh_frame`, no registry.
- **DWARF (RISC-V, Xtensa: ESP32-C6)** -- the heaviest on the *code* side. `.eh_frame` is
  folded **into the code region** and it is bulky: on ESP32-C6 that folding is exactly why
  [`../../arch/riscv/chip/esp32c6/esp32c6.ld`](../../arch/riscv/chip/esp32c6/esp32c6.ld)
  bumps its code region `64K -> 128K`. It adds writable state too (the FDE registry), but
  the headline cost is the read-only `.eh_frame` inflating the code grant.

The lesson for sizing a new board: the writable floor is roughly toolchain-independent
(a few KiB of tax plus the app's live heap), but the **code-region** size a full-C++ app
needs depends on the EH model -- DWARF arches must budget extra code space for `.eh_frame`,
ARM and RX do not.

## The window is the budget; the heap is the remainder

There is no separate heap-size number to provision. The app's dynamic memory is simply
**whatever RAM is left inside the app's data window after its statics** -- so the single
per-board knob is the *window*, and the heap falls out of it.

On an enforcement (MPU) chip that knob is **`KICKOS_APPDATA_SIZE`**, and it does double
duty: it is the *isolation boundary* -- the granted, MPU-shaped data region an unprivileged
thread may touch -- **and** the whole writable RAM budget for the app side. The app's
`.data`/`.bss` are laid down at the bottom of the window; everything above them, up to the
window's end, is the heap:

- `_kickos_heap_start = ALIGN(_appdata_used_end, 8)` -- just past the app's statics.
- `_kickos_heap_limit = __kickos_appdata_end` -- the window's end (its padded, grantable
  edge on the pow2 arches).

`_sbrk` ([`../../user/src/newlib_sbrk.cc`](../../user/src/newlib_sbrk.cc)) bumps a single
break pointer between that pair and never frees back to the OS, so `malloc`'s own free list
reuses within the pad. The heap is the pad. Grow the window and the heap grows with it; add
statics and the heap shrinks by exactly that much.

On a **non-MPU** chip there is no window and no isolation boundary, so the heap cannot be
"leftover window pad" -- there is no window. Instead the linker reserves an explicit
`.userheap (NOLOAD)` section, sized by **`KICKOS_USER_HEAP_SIZE`**, carved out of RAM
*before* the thread-stack arena. The same `_kickos_heap_start`/`_kickos_heap_limit` symbols
bracket it, so `_sbrk` is identical on both paths -- only where the bounds come from
differs. A board that provisions no heap at all defines neither symbol, and an app that
pulls in `malloc` then fails at **link** with an undefined reference to `_kickos_heap_start`
-- fail-loud, and only for an app that actually allocates.

### The two failure modes

Because the heap is a remainder, the two ways to get it wrong are distinct and land at
different times:

- **Statics too big for the window.** The app's `.data`/`.bss` overflow
  `KICKOS_APPDATA_SIZE` before the heap even begins. This is a **link error** -- the script
  `ASSERT`s `_appdata_used_end <= window_end` and prints "raise it to the next pow2." The
  fix is to grow the window (or shrink the statics), never to truncate.
- **A heap-hungry app on a too-tight window.** The statics fit, but at runtime the app
  allocates past `_kickos_heap_limit`; `_sbrk` returns `-1` and `malloc` returns NULL. This
  is a **runtime** condition, not a link error -- because the linker cannot know an app's
  peak working set. A board that wants this caught deterministically opts into
  **`KICKOS_HEAP_MIN`**: when set, the script `ASSERT`s the available heap span is at least
  that many bytes, turning a too-tight window into a link failure. It is off by default so
  a board only pays the determinism it asks for.

The boot banner makes the outcome visible without a debugger: it prints
`heap  N KiB available` from the live `_kickos_heap_start`/`_kickos_heap_limit` span, or
`heap  none` when no heap was provisioned. That line is the quickest check that a window
change did what you meant.

### Sizing it in practice

Anchor the window on the *measured* peak, not a round guess. A full-EH/STL `cxxtest`
working set peaks near 6 KiB of heap, so a window whose pad leaves on the order of **16 KiB**
above the statics is comfortable for essentially any app in the fleet, with headroom.
Provisioning far above the measured peak just burns RAM: the pad is zeroed at boot (below)
and, on a pow2 arch, rounding the window up wastes the gap to the next power of two -- so an
oversized window hurts twice, and on a small-RAM board it simply will not link.

## Now the linker: one image, cut in two

Everything above is about *how much* writable RAM and where the heap comes from. The rest
of this chapter is about *which side of the protection wall* each byte lands on -- and that
is decided entirely in the chip linker script. KickOS links the kernel, the arch/chip
backends, the app, and the whole toolchain runtime into **one ELF image**. A
memory-protection unit then needs that one image partitioned so an unprivileged thread
reaches its own code and data but never the kernel's. The linker script is where that
partition is authored. Read
[`../../arch/arm/chip/mk64f/mk64f.ld`](../../arch/arm/chip/mk64f/mk64f.ld) alongside this
section; it is the worked example this chapter quotes, and every other chip's script is
recognisably the same scheme. Where they differ is the granularity of the protected
window, which the next sections show changing the shape of one check.

### The inverted colon selector: capture the kernel, let the rest fall through

The core move is how writable state is sorted kernel-side vs app-side. The naive design is
"match the app's stuff and put it in the app window." KickOS does the **inverted**
thing: match the **closed, known set of KickOS-owned archives** first and pin it
kernel-side; let a catch-all take *everything else* into the app window.

```
.data : ALIGN(4)
{
    _sdata = .;
    *libkickos_kernel.a:*(.data .data.*)
    *libkickos_arch_armv7m.a:*(.data .data.*)
    *libkickos_chip_mk64f.a:*(.data .data.*)
    *libkickos_lib.a:*(.data .data.*)
    . = ALIGN(4);
    _edata = .;
} > RAM AT > FLASH

.appdata : ALIGN(32)
{
    __kickos_appdata_start = .;
    *(.data .data.*)                 /* everything not captured above */
    . = ALIGN(4);
    __kickos_appdata_load_end = .;
} > RAM AT > FLASH
```

Two properties make this the right design:

- **It is fail-safe.** The KickOS-owned set (`kernel`/`arch`/`chip`/`lib`) is *closed* and
  known; everything else -- the app's own objects, the toolchain runtime (newlib, libgcc,
  libstdc++, libsupc++), KickCAT, and any **unknown new archive** someone links tomorrow
  -- falls into the `.appdata` catch-all. An unmatched archive therefore lands **app-side**
  (reachable, it works), never kernel-side (where it would fault the unprivileged thread).
  The dangerous failure would be silently *leaking kernel writable state into the app
  window*; that shows up as an empty kernel `.bss`, which the script's
  `ASSERT(_ebss > _sbss, ...)` catches loudly.
- **It selects by `archive:member`, not `EXCLUDE_FILE`.** The design intended "exclude the
  kernel from the app window" via `EXCLUDE_FILE`, but this binutils (arm-none-eabi 15.3)
  **does not match archive members inside `EXCLUDE_FILE`** -- a bare `*libkickos_kernel.a`
  there matches nothing. And a bare `*user*` substring matches only an object's basename,
  so it misses the toolchain runtime (which has no "user" in any name). The mechanism that
  works is the colon form `*libkickos_kernel.a:*(...)`, which selects by archive. So the
  plan's "exclude the kernel" became "**include the kernel first, let the rest fall
  through**" -- same isolation, opposite selector, and path/name-substring independent.

The `.bss`/`.appbss` pair mirrors this exactly (the closed set into kernel `.bss`, the rest
into `.appbss`), and the app window is padded to `_appdata_size` so the granted region has
a fixed, grantable size.

### The catch-all has a second customer: things the kernel writes and the app reads

Everything above is about where an object *ends up*. There is a class of object for which the
answer is not incidental but a correctness requirement, and it is easy to miss because the object
is usually tiny.

Consider the argument handoff from the kernel's bring-up to the first application thread: the
kernel fills in `argc`/`argv`, and that thread reads them as very nearly its first act, before it
runs `main`. Write that handoff as a **local variable** in the boot function and it lives on the
kernel's boot stack -- which is outside the arena, outside the app window, and outside every region
an unprivileged thread is granted. An unprivileged reader would then fault on the handoff before
executing a single statement of its own, on every enforcing board. And the fix is not a bigger
grant, because widening a grant to reach the kernel's boot stack would hand the app a window over
kernel memory to solve a data-placement problem.

The fix is placement. The handoff is a named object defined in the userspace library archive, which
means the `.appdata`/`.appbss` catch-all sweeps it into the app window -- a region every
unprivileged thread already has. Nothing about the object's type or its declaration changes; only
where its bytes live.

It also has to be in the *right* archive, and for a reason that has nothing to do with protection.
The definition sits in the general userspace library rather than in the archive of whichever target
supplies the init entry point, because a build may name its own init provider -- and then that
archive leaves the link while the kernel's unconditional reference to the handoff stays, turning a
supported customisation into an undefined symbol. So placement answers to two independent masters:
**the linker script decides which side of the protection wall an object lands on, and the build
graph decides whether the archive holding it can be substituted away.** A design that only thinks
about the first one is one build option away from failing to link.

The transferable rule is that an interface crossing the protection wall needs its *storage* placed,
not merely its type declared. The declaration says what the bytes mean; the section and the archive
decide who is allowed to read them and whether they will be there at all. Ask it at design time:
which side writes this, which side reads it, and is the reader privileged?

One honest caveat, since this chapter is about a real memory map: placing the handoff *struct*
app-side does not place what it points *at*. On the hosted sim, `argv` points into the host
process's own argument vector, which no grant covers -- and the sim does not enforce non-arena
regions, so that is precisely the case its coverage misses. Chapter 7.1,
*[Alignment across the syscall boundary](alignment-across-the-syscall-boundary.md)*, works through
what the sim can and cannot witness about caller-supplied memory.

### The heap lives in the window's leftover pad

The heap-is-the-remainder model of the previous section is authored right here, at the tail
of the app window. Once the app's `.appbss` has been laid down, the script marks the end of
the real statics, pads up to the granted window size, and hands the gap to the heap:

```
    . = ALIGN(4);
    _appdata_used_end = .;                 /* end of the app's real .data/.bss */
    _appdata_fits = ASSERT(_appdata_used_end <= __kickos_appdata_start + _appdata_size,
           "KickOS: app .data/.bss overflow _appdata_size (raise it to the next pow2)");
    . = __kickos_appdata_start + _appdata_size;   /* pad the granted window */
    __kickos_appdata_end = .;
    /* The newlib heap IS the window pad: [ALIGN(used_end,8), window end). */
    _kickos_heap_start = ALIGN(_appdata_used_end, 8);
    _kickos_heap_limit = __kickos_appdata_end;
```

That `_appdata_fits` line is the pow2-window chips' form, and the placement is the lesson.
The `ASSERT` is bound to a symbol purely so it is *evaluated inside the section*, before
the pad assignment: a bare pad to an already-overflowed window makes the location counter
move backwards, and `ld` reports that in its own cryptic terms rather than in yours. Put
the check after the section and the useful message never gets the chance to print. The
SYSMPU chip quoted throughout this chapter states the same bound at the bottom of the
script instead of inside the section, which is the arrangement that loses the message, so
copy the in-section form when authoring a new one.

On a non-enforcement chip there is no window, and the same symbol pair instead brackets
a standalone `.userheap` block reserved ahead of the thread-stack arena:

```
    .userheap (NOLOAD) :
    {
        . = ALIGN(8);
        _kickos_heap_start = .;
        . += KICKOS_USER_HEAP_SIZE;
        _kickos_heap_limit = .;
    } > RAM
```

Either way `_sbrk` sees one contiguous `[_kickos_heap_start, _kickos_heap_limit)` span, and
the optional `KICKOS_HEAP_MIN` `ASSERT` at the bottom of the script -- `#ifdef`-gated so it
costs nothing when unset -- turns a too-small span into a link error instead of a runtime
`malloc` NULL.

### The ctor split: kernel constructors early, app constructors with the kernel live

Global constructors (`.init_array`) cannot all run at the same time on a freestanding OS,
and the linker is where they are separated. KickOS splits `.init_array` in two:

```
.init_array :
{
    __init_array_start = .;
    KEEP(*libkickos_kernel.a:*(SORT(.init_array.*) .init_array))
    KEEP(*libkickos_arch_armv7m.a:*(SORT(.init_array.*) .init_array))
    KEEP(*libkickos_chip_mk64f.a:*(SORT(.init_array.*) .init_array))
    KEEP(*libkickos_lib.a:*(SORT(.init_array.*) .init_array))
    __init_array_end = .;
} > FLASH

.kickos_app_init_array :
{
    __kickos_app_init_array_start = .;
    KEEP(*(SORT(.init_array.*)))     /* app + libstdc++ + KickCAT ctors */
    KEEP(*(.init_array))
    __kickos_app_init_array_end = .;
} > FLASH
```

The same closed-set colon selector routes the kernel/arch/chip/lib constructors into
`.init_array`, which `Reset_Handler` runs **early**, before `kmain` -- so the kernel
singletons `kmain` needs are constructed first. Every *other* constructor (the app's,
`libstdc++`/`libsupc++`'s, KickCAT's) lands in `.kickos_app_init_array` and runs later
from `root_entry`, the kernel's first thread, with the scheduler and clock fully live --
because such a constructor may call a KickOS syscall (`kos_clock_now`), which needs a
current thread to exist. The full "why" is in the boot-order section of Chapter 0.6; the
linker's job is just to give the two groups separate homes with bracketing symbols the C
runtime can walk.

### EH-table homing: read-only tables into the code grant

The exception tables are placed **explicitly** in the code region so the unprivileged
thread reads them through its RX code grant, rather than left orphaned to land wherever.
On mk64f the EHABI tables go into FLASH:

```
.ARM.extab : { *(.ARM.extab* .gnu.linkonce.armextab.*) } > FLASH

.ARM.exidx :
{
    __exidx_start = .;
    *(.ARM.exidx* .gnu.linkonce.armexidx.*)
    __exidx_end = .;
} > FLASH
```

The other models home their tables the same way in their own scripts: RX puts
`.gcc_except_table` in ROM ([`rx72m.ld`](../../arch/rx/chip/rx72m/rx72m.ld)), RISC-V folds
`.eh_frame` + `.gcc_except_table` into the `.text` code region and `KEEP`s them with a
`LONG(0)` terminator ([`esp32c6.ld`](../../arch/riscv/chip/esp32c6/esp32c6.ld)). In every
case the placement is deliberate: the tables must sit **inside** the code region the
unprivileged thread was granted, or the unwinder faults reading them.

### The copy/zero init tables: standing up RAM before C runs

A freestanding image has no loader to initialize RAM, so the script builds small
**tables** in flash that a generic C-runtime init (`kickos_ranges_init`) walks: copy
triples `{src, dst, len}` for initialized data, zero pairs `{dst, len}` for `.bss`.

```
.kickos_init_tables : ALIGN(4)
{
    __kickos_copy_table_start = .;
#if KICKOS_HAVE_MPU
    LONG(LOADADDR(.appdata)); LONG(ADDR(.appdata)); LONG(SIZEOF(.appdata));
#endif
    LONG(LOADADDR(.data));    LONG(ADDR(.data));    LONG(SIZEOF(.data));
    __kickos_copy_table_end = .;
    __kickos_zero_table_start = .;
#if KICKOS_HAVE_MPU
    LONG(__kickos_appdata_load_end);
    LONG(__kickos_appdata_end - __kickos_appdata_load_end);
#endif
    LONG(ADDR(.bss));    LONG(SIZEOF(.bss));
    __kickos_zero_table_end = .;
} > FLASH
```

Under enforcement the app window gets its own entries: `.appdata` is copied from its flash
load address to its RAM home just like `.data`, and the whole granted window past the
loaded `.appdata` (the alignment gap, `.appbss`, and the pad) is zeroed. That pad is the
heap, so zeroing it is also what hands `_sbrk` a clean, defined heap -- and it guarantees
no stale read-back anywhere in a region the app can read. The RISC-V script adds one wrinkle
its `Reset_Handler` must honor: `.appdata`'s load address and run address differ, so it
must copy `.appdata` before zeroing `.appbss`, or `malloc`'s bins read uninitialized
memory.

### The RISC-V gp anchor

One arch needs a step the others do not. On RISC-V the `gp` small-data window must be
anchored **inside** the app's granted data region, not kernel-side, or an unprivileged
throw faults reading its own `gp`-relative runtime globals. The full account -- why, and
why "empty the kernel `gp` side and move the anchor" beats every alternative -- is the
companion chapter [*Exceptions and RTTI under memory protection*](exceptions-and-rtti-under-memory-protection.md).
In the linker script it is a `PROVIDE(__global_pointer$ = ...)` placed within the app-data block
plus the KickOS libs compiled `-msmall-data-limit=0` so they vacate the window. ARM and RX
scripts have no `gp` and skip this entirely.

## How to read (and author) one of these scripts

Pulling the mk64f worked example together, a chip linker script for enforcement is read
top-to-bottom as a sequence of decisions:

1. **Where does code live, and is the EH table inside it?** Find `.text` and the explicit
   `.ARM.exidx`/`.gcc_except_table`/`.eh_frame` placement. The unprivileged thread's code
   grant must cover all of it.
2. **What is the closed kernel set, and does the app catch-all follow it?** Find the
   `archive:member` colon selectors for `kernel`/`arch`/`chip`/`lib`, then the bare
   `*(.data .data.*)` catch-all. Anything not in the closed set is app-side; that is the
   fail-safe.
3. **Are the two ctor groups separated?** `.init_array` (closed set, early) vs
   `.kickos_app_init_array` (everything else, from a thread).
4. **Is the app window a grantable shape, and where does the heap fall?** `_appdata_size`
   power-of-two and aligned on PMSA/PMP; exact-sizable on SYSMPU/RX. The heap is the pad
   between `_appdata_used_end` and the window end (or, off enforcement, the `.userheap`
   block). The `ASSERT`s at the bottom are the guardrails -- a `.data`/`.bss` overflow means
   the statics do not fit and the window must grow; an empty kernel `.bss` means a selector
   matched nothing (a renamed lib); a `KICKOS_HEAP_MIN` failure means the pad left for the
   heap is too thin.
5. **Does RAM get stood up before C runs?** The copy/zero tables (the zeroed pad is the
   heap), and (RISC-V) the `.appdata` LMA->VMA copy in `Reset_Handler`.

To *author* a new chip's script, copy mk64f.ld (ARM SYSMPU/PMSA), rx72m.ld (RX MPU, SjLj,
exact-size), or esp32c6.ld (RISC-V PMP, DWARF, `gp` anchor) as the nearest template,
change the `MEMORY` map and the `chip` archive name in the colon selectors, size
`_appdata_size`/`_code_size` for the part, and keep the `ASSERT`s. The isolation is only
ever as good as the selector list being complete and the window being a shape the unit can
grant -- both of which the `ASSERT`s check at link time so a mistake is a build failure,
not a silent leak.

## Where to go next

- What the runtime *does* with this memory (the four pieces, the three exception models):
  [*Exceptions and RTTI under memory protection*](exceptions-and-rtti-under-memory-protection.md).
- The bottom-edge stubs and the boot-order ctor story:
  [`whats-under-include-libc-and-the-cxx-runtime.md`](whats-under-include-libc-and-the-cxx-runtime.md).
- The region-set model and the C++-under-MPU budget:
  [`../reference/architecture.md`](../reference/architecture.md) ("Memory domains", "C++ decisions").
- The RISC-V `gp` anchor contract: [`../reference/porting.md`](../reference/porting.md) (RISC-V arch).
- Memory protection and how regions are granted at spawn: Chapter 7, *Memory protection*.
