<!--
SPDX-License-Identifier: CECILL-C
Copyright (c) 2026 Philippe Leduc
-->
# Where your RAM goes: the full-C++ memory floor and splitting kernel from app in the linker

> **Status: DRAFT.** A Chapter-7 companion, downstream of the C++ runtime chapter 0.4
> ([`whats-under-include-libc-and-the-cxx-runtime.md`](whats-under-include-libc-and-the-cxx-runtime.md))
> and the runtime-memory chapter
> ([*Exceptions and RTTI under memory protection*](exceptions-and-rtti-under-memory-protection.md),
> which established *what* writable state the runtime keeps). This chapter answers two
> practical questions a porter and an app author both ask: **how much writable RAM does a
> full-C++ app cost, and why**, and **how is one linked image cut into a kernel side and
> an app side so a memory-protection unit can fence them apart**. Points into
> [`../design-cxx-under-mpu.md`](../design-cxx-under-mpu.md) for the sizing measurements
> and [`../design-riscv-gp-split.md`](../design-riscv-gp-split.md) for the RISC-V `gp`
> anchor; the worked linker example is
> [`../../arch/arm/chip/mk64f/mk64f.ld`](../../arch/arm/chip/mk64f/mk64f.ld). This chapter is
> the LAYOUT: which side of the wall each byte lands on. The proof status of full C++ under
> enforcement is scoped in the companion -- the confined UNPRIVILEGED throw is RUN-PROVEN on
> qemu-riscv (the committed `cxxtest` spawns an unprivileged worker; `qemu_riscv_cxxtest` ALL
> PASS under PMP), and the runtime is silicon-validated to RUN with enforcement active (the old
> privileged-root cxxtest) on five arches, with the silicon U-mode throw build-verified pending a
> bench re-flash.

A freestanding KickOS app pays almost nothing for its writable state: a few globals, its
stack. Flip on full C++ -- `-fexceptions -frtti`, the toolchain's `libstdc++`/`libsupc++`
in the link -- and a floor appears under the app's RAM that was not there before. This
chapter is about that floor: what sits on it, why it is roughly the size it is, and then
the linker mechanics that decide which side of the protection boundary every byte of it
lands on.

*Further reading: Tanenbaum, Modern Operating Systems, ch.3 (memory management -- how an
address space is laid out and protected). The linker script here is exactly the tool that
authors that layout by hand on a system with no loader to do it at runtime.*

## The writable floor, and why it is about 32K

The runtime-memory chapter listed the writable pieces; here is what they *cost*. A
full-C++ app's writable RAM is dominated by two contributors:

- **The heap arena** (`s_heap` in [`../../user/src/newlib_stubs.cc`](../../user/src/newlib_stubs.cc)),
  a single static buffer that `malloc`/`operator new` carve from. This is a *provisioning*
  number, not a law -- `KICKOS_HEAP_SIZE`, default 64 KiB but set to **16 KiB** on the
  small-RAM boards (XMC, RP2040, ESP32-C6). It is usually the largest single item.
- **The runtime's own writable globals** -- newlib's reent (`_impure_ptr`) and malloc
  bins, libsupc++'s `eh_globals` + emergency pool + handler pointers, and (DWARF arches
  only) the FDE registry heads and node. Call it the *runtime writable tax*: a few KiB,
  fixed, paid the moment `libstdc++`/`libsupc++` join the link.

Add them and round to what a protection unit can grant, and you land near **32 KiB**.
That is not a guess -- it is exactly how the reference full-C++ board is provisioned:
[`../../arch/arm/chip/xmc4800/xmc4800.ld`](../../arch/arm/chip/xmc4800/xmc4800.ld) sizes
its app window at `32K` with the note "holds a full-C++ app's 16K heap (KICKOS_HEAP_SIZE)
+ libstdc++ writable state." So the floor is: **your heap, plus a few KiB of fixed runtime
tax, rounded up to a grantable window.** A freestanding app uses a fraction of it and (on
the pow2 arches) pays the padding.

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
(heap + a few KiB), but the **code-region** size a full-C++ app needs depends on the EH
model -- DWARF arches must budget extra code space for `.eh_frame`, ARM and RX do not.

## The two knobs

Two board-config values govern the floor; both are overridable per board (or per app):

- **`KICKOS_HEAP_SIZE`** -- the size of `s_heap`, the libc arena. Default 64 KiB
  ([`../../user/src/newlib_stubs.cc`](../../user/src/newlib_stubs.cc)); 16 KiB on the
  small-RAM boards. This is the app's total dynamic-allocation budget; there is no `free`
  back to the OS, so `malloc`'s own free list reuses within it.
- **`KICKOS_APPDATA_SIZE`** -- the size of the *granted data window* that must hold the
  heap **and** all the runtime writable tax. It must be `>= KICKOS_HEAP_SIZE +
  runtime-writable-state`. On PMSA (ARM v7-M) and PMP (RISC-V) it must be a **power of
  two** and the window is padded up to it (so a 16 KiB heap plus a few KiB of tax rounds to
  a 32 KiB window); on SYSMPU (K64F, 32-byte granular) and the RX MPU (16-byte granular)
  it can be sized more exactly. A linker `ASSERT` fires if the app's real `.data`/`.bss`
  overflow the window -- the fix is to raise `KICKOS_APPDATA_SIZE` to the next power of
  two, not to silently truncate.

The relationship is the thing to remember: `KICKOS_HEAP_SIZE` sizes the arena;
`KICKOS_APPDATA_SIZE` sizes the *region that arena lives in*, and the region must have room
for the arena plus everything else the runtime writes.

## Now the linker: one image, cut in two

Everything above is about *how much* writable RAM. The rest of this chapter is about
*which side of the protection wall* each byte lands on -- and that is decided entirely in
the chip linker script. KickOS links the kernel, the arch/chip backends, the app, and the
whole toolchain runtime into **one ELF image**. A memory-protection unit then needs that
one image partitioned so an unprivileged thread reaches its own code and data but never
the kernel's. The linker script is where that partition is authored. Read
[`../../arch/arm/chip/mk64f/mk64f.ld`](../../arch/arm/chip/mk64f/mk64f.ld) alongside this
section; it is the reference scheme and every other chip mirrors it.

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
  so it misses `libkickos_user.a`'s `s_heap` and the entire toolchain runtime (which has no
  "user" in any name). The mechanism that works is the colon form
  `*libkickos_kernel.a:*(...)`, which selects by archive. So the plan's "exclude the
  kernel" became "**include the kernel first, let the rest fall through**" -- same
  isolation, opposite selector, and path/name-substring independent.

The `.bss`/`.appbss` pair mirrors this exactly (the closed set into kernel `.bss`, the rest
into `.appbss`), and the app window is padded to `_appdata_size` so the granted region has
a fixed size.

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
current thread to exist. The full "why" is in the boot-order section of Chapter 0.4; the
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
loaded `.appdata` (the alignment gap, `.appbss`, and the pad) is zeroed -- so there is no
stale read-back anywhere in a region the app can read. The RISC-V script adds one wrinkle
its `Reset_Handler` must honor: `.appdata`'s load address and run address differ, so it
must copy `.appdata` before zeroing `.appbss`, or `malloc`'s bins read uninitialized
memory.

### The RISC-V gp anchor

One arch needs a step the others do not. On RISC-V the `gp` small-data window must be
anchored **inside** the app's granted data region, not kernel-side, or an unprivileged
throw faults reading its own `gp`-relative runtime globals. The full account -- why, and
why "empty the kernel `gp` side and move the anchor" beats every alternative -- is the
companion chapter and [`../design-riscv-gp-split.md`](../design-riscv-gp-split.md). In the
linker script it is a `PROVIDE(__global_pointer$ = ...)` placed within the app-data block
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
4. **Is the app window a grantable shape?** `_appdata_size` power-of-two and aligned on
   PMSA/PMP; exact-sizable on SYSMPU/RX. The `ASSERT`s at the bottom are the guardrails --
   an overflow means raise the knob, an empty kernel `.bss` means a selector matched
   nothing (a renamed lib).
5. **Does RAM get stood up before C runs?** The copy/zero tables, and (RISC-V) the
   `.appdata` LMA->VMA copy in `Reset_Handler`.

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
- The sizing measurements and the region-set design:
  [`../design-cxx-under-mpu.md`](../design-cxx-under-mpu.md).
- The RISC-V `gp` split in full: [`../design-riscv-gp-split.md`](../design-riscv-gp-split.md).
- Memory protection and how regions are granted at spawn: Chapter 7, *Memory protection (M2)*.
