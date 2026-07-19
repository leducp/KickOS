<!--
SPDX-License-Identifier: CECILL-C
Copyright (c) 2026 Philippe Leduc
-->
# Exceptions and RTTI under memory protection

> **Status: DRAFT.** A Chapter-7 companion (prereq: the C++ runtime chapter 0.4,
> [`whats-under-include-libc-and-the-cxx-runtime.md`](whats-under-include-libc-and-the-cxx-runtime.md),
> and the memory-protection idea of Chapter 7). That chapter taught *what* the C++
> runtime is made of -- three stacked libraries -- and ended on a one-paragraph "MPU
> twist". This chapter is that twist in full: what a `throw`/`catch`/`dynamic_cast`
> actually touches *at runtime*, why a memory-protection unit makes it hard, and how an
> UNPRIVILEGED thread reaches all of it without a fault. Points into
> [`../design-cxx-under-mpu.md`](../design-cxx-under-mpu.md) (the region-set design and
> its proofs) and [`../design-riscv-gp-split.md`](../design-riscv-gp-split.md) (the
> RISC-V `gp` fix) for the authoritative contract; this chapter explains, those bind.

You write `throw std::runtime_error("no")` in a worker thread, a frame or two up a
`catch` grabs it, and on the way out every local destructor runs. On a hosted desktop
this is invisible. On KickOS the worker is an *unprivileged* thread, fenced by a memory
protection unit (MPU on ARM/RX, PMP on RISC-V) into a handful of granted regions -- its
code, its data, its stack -- and a read or write one byte outside them is a hardware
fault. So the question this chapter answers is sharp: **when that thread throws, every
piece of memory the C++ runtime touches to unwind the stack and match the catch must
already sit inside a region the thread was granted.** Miss one and the throw does not
propagate -- it faults.

*Further reading: Tanenbaum, Modern Operating Systems, ch.1 (the OS as protection
boundary; user vs kernel mode) and ch.3 (memory protection / MMU-MPU). The C++ machinery
here is application of those ideas: an exception is a controlled non-local transfer, and
under protection every address it reaches is subject to the same grant check as any other
load or store.*

## What a full-C++ runtime needs at runtime

Strip a `throw`/`catch`/`dynamic_cast` down to the memory it reads and writes while it
runs. There are four things, and the split that governs everything below is **read-only
vs writable**:

- **The heap.** `operator new`, `std::vector` growth, `std::string`, and
  `__cxa_allocate_exception` (the exception object itself is heap-allocated) all carve
  from the libc arena. **Writable.**
- **The exception unwind tables.** Per-function descriptions of how to unwind each frame
  and where its landing pads and cleanups are: `.eh_frame` + `.gcc_except_table` (DWARF),
  `.ARM.exidx` + `.ARM.extab` (ARM EHABI), or `.gcc_except_table` alone (SjLj). The
  compiler emits them once, at build time; the unwinder only ever *reads* them.
  **Read-only.**
- **The unwinder's own working state.** This is the piece that surprises people. Walking
  frames and running a two-phase search-then-cleanup is not pure computation -- it keeps
  live state: libsupc++'s `eh_globals` (the per-context in-flight-exception chain,
  `__cxa_eh_globals`), the emergency pool that lets `throw std::bad_alloc` succeed when
  the heap is empty, `__new_handler`/`__terminate_handler`, and on DWARF arches the FDE
  registry (`seen_objects`/`unseen_objects` list heads plus a heap node). **Writable.**
- **RTTI `type_info`.** `typeid`, `dynamic_cast`, and the catch-type match all compare
  `std::type_info` objects; those, and the vtables that point at them, are constants in
  `.rodata`. **Read-only.**

Hold that split, because it is the whole design under protection: **the read-only two
(tables + RTTI) ride the app's code region for free; the writable two (heap + unwinder
state) must be placed, deliberately, in the app's granted data region.** Chapter 0.4
named these; here they earn their placement.

## Three exception models, and how each is reached unprivileged

There is no single "the exception runtime." The mechanism is a property of the
*toolchain*, and KickOS's fleet spans three of them. What matters for protection is what
each model needs *at runtime* -- specifically, whether it has writable state and whether
it must be *registered* before it works.

### EHABI (ARM) -- no registration, tables found by symbol

The Arm GNU toolchain uses the ARM Exception Handling ABI. Per-function unwind
instructions live in `.ARM.exidx` (an index) and `.ARM.extab` (overflow), and the
unwinder finds them through **linker-defined symbols** `__exidx_start`/`__exidx_end`
that bracket the index. There is **no runtime registration step at all** -- the linker
bakes the bounds in, and the tables are pure read-only data. For an unprivileged thread
this is the easy case: the tables sit in the code region (see
[`arch/arm/chip/xmc4800/xmc4800.ld`](../../arch/arm/chip/xmc4800/xmc4800.ld), where
`.ARM.exidx`/`.ARM.extab` are placed explicitly in FLASH so the app reads them via its RX
code grant), and the only writable state is `eh_globals` + the emergency pool in the app
data region. This is the model on **K64F, XMC, and RP2040**.

### SjLj (RX) -- a runtime context chain, no frame registration

The Renesas GNURX toolchain uses **setjmp/longjmp** exceptions. Instead of walking the
machine stack from unwind tables, each `try` region registers a context on entry (a
`setjmp`-style save) into a runtime chain, and `throw` `longjmp`s back down that chain,
consulting `.gcc_except_table` (the LSDA -- language-specific data, landing pads and type
filters) to decide where to land. Two consequences for protection: the LSDA is
**read-only** and homes in ROM (see
[`arch/rx/chip/rx72m/rx72m.ld`](../../arch/rx/chip/rx72m/rx72m.ld), which places
`.gcc_except_table` explicitly in ROM so it is not swallowed by the fixed 0x80-byte reset
vector table, and so the app reads it via its ROM code grant), and the SjLj context chain
head is **writable** and lives with the rest of the runtime's writable state in the app
data region. There is **no `__register_frame`** -- SjLj has no `.eh_frame` and no FDE
registry to register. This is the model on **RX72M**.

### DWARF (RISC-V, Xtensa) -- .eh_frame plus a boot-time registration hook

The RISC-V and Xtensa toolchains use table-driven DWARF unwinding: `.eh_frame` holds
frame-description entries (FDEs) that the unwinder walks, `.gcc_except_table` holds the
LSDA. Unlike EHABI, DWARF `.eh_frame` is **not** found by a magic linker symbol -- it
must be **registered at runtime** by calling `__register_frame(&__eh_frame_start)`, which
mallocs a `struct object` node and links it onto the FDE registry. On a hosted system the
C runtime's `crtbegin`/`frame_dummy` does this before `main`; a freestanding KickOS image
linked `-nostartfiles` has no `frame_dummy`, so **KickOS registers the frame itself, at
boot, in the privileged reset path** (a weak `__register_frame` called only in a
`FULL_CXX` link -- see
[`arch/riscv/chip/esp32c6/chip_esp32c6.cc`](../../arch/riscv/chip/esp32c6/chip_esp32c6.cc)
and the `.eh_frame` homing in
[`arch/riscv/chip/esp32c6/esp32c6.ld`](../../arch/riscv/chip/esp32c6/esp32c6.ld), which
`KEEP`s the table in the code region and appends a `LONG(0)` CIE terminator to stand in
for the `crtend` `__FRAME_END__` that is not linked). The registry's list heads
(`seen_objects`/`unseen_objects`) and the malloc'd node are **writable**; the `.eh_frame`
table itself is **read-only**. This is the model on **ESP32-C6** (and any future Xtensa).

The registration *timing* is the subtle part, and it is why the boot hook must run
privileged: because `__register_frame` mallocs, and the arena it mallocs from is the app
window, the node lands in the granted region **only if** it is allocated while the FDE
registry code has reach -- doing it once at boot, before any drop to unprivileged, puts
the node where the unprivileged unwinder will later look for it.

## Why a protection unit makes this hard

Without protection, all four pieces (heap, tables, unwinder state, RTTI) are just
addresses in one flat image and everything works. Protection splits the image into a
kernel side and an app side and gives the unprivileged thread reach into only its own
regions. Now the placement of every writable byte is load-bearing:

```
   unprivileged thread's grants          the C++ runtime touches
   ---------------------------           -----------------------
   code region (RX)  ----------------->  .eh_frame / .exidx / .gcc_except_table   (read)
                                         type_info, vtables (.rodata)             (read)
                                         landing pads (.text)                     (execute)
   data region (RW)  ----------------->  s_heap (libc arena)                      (write)
                                         _impure_ptr, malloc bins (newlib reent)  (write)
                                         eh_globals, emergency pool               (write)
                                         FDE registry heads + node (DWARF only)   (write)
   stack region (RW) ----------------->  local dtors run during cleanup           (read/write)
```

The read-only tables and RTTI **already** live inside the per-app code region that is
granted read+execute, so unwinding reads them at no extra cost -- **zero additional
regions**. The trap is the writable column: if any of `s_heap`, the newlib reent, or the
libsupc++ globals lands **kernel-side**, the unprivileged thread faults the first time the
runtime touches it -- and the first touch is early (the reent is read by almost any stdio
or math call, the heap by the first `new`). The engineering is therefore not "add regions
for C++"; it is "make sure every writable runtime global lands in the *one* app data
region the thread already has." How that placement is expressed in the linker is the
subject of the companion chapter,
[*Where your RAM goes*](where-your-ram-goes-full-cxx-memory-floor-and-the-linker-split.md);
the invariant it must satisfy is this one.

The budget payoff, spelled out in [`../design-cxx-under-mpu.md`](../design-cxx-under-mpu.md):
the full EH + RTTI + heap footprint costs **+0 protection regions** on every arch. Tables
and RTTI fold into the code grant, all writable state folds into the one app-data grant.
A full-C++ thread needs the same region count as a bare freestanding one -- three (code +
data + stack), plus one per granted domain/MMIO region.

## The RISC-V gp wrinkle -- when placing app globals breaks unwinding

Three of the four validated arches (EHABI x3, SjLj x1) reach the picture above with only
linker placement. RISC-V does not, and the reason is a collision between two mechanisms
that both want the same register.

RISC-V's **small-data optimization** addresses small globals (<= 8 bytes by default) as
`gp + immediate`, relative to a single `__global_pointer$` anchor set once at boot. That
window holds the runtime's own small globals -- `eh_globals`, `_impure_ptr`, the FDE
registry heads -- and by default it links **kernel-side**, outside the app grant. The
obvious fix, "compile the app `-msmall-data-limit=0` so its globals leave the `gp` window
and fall into the app-side `.data`/`.bss` catch-all," works for the app's *own* globals
but **breaks unwinding**: a `-fexceptions` translation unit's DWARF EH references
(`DW.ref.*`, the LSDA datarel encodings) are themselves resolved `gp`-relative, so moving
them away from `gp` corrupts the lookup and `__cxa_throw` hangs in an unbounded FDE walk.
The very flag that places app globals in the granted region is incompatible with
exceptions.

The fix (Option 4 in [`../design-riscv-gp-split.md`](../design-riscv-gp-split.md), proven
on qemu-riscv) is to **stop fighting `gp` and move it**: keep the app compiled *with*
small-data so unwinding works, compile the **KickOS libs** `-msmall-data-limit=0` so they
emit no small-data and vacate the `gp` window entirely (measurement in that doc: the
kernel's own scheduler state -- `g_arch_current`, the CLINT doorbell -- lives in `gp`
small-data today, so this step is load-bearing, not cosmetic; leaving it there and
granting the window would hand an unprivileged thread the scheduler), and anchor
`__global_pointer$` **inside** the app's granted data region. Now the single `gp` window
holds only app + C++-runtime small-data and is covered by the app grant, `gp` stays one
link-time constant (no context-switch cost, `switch.S` untouched), and it folds into the
existing app-data region at +0 protection entries. That study also rules out the tempting
"two `gp` bases, reload on the kernel<->user boundary" split: a single-image link has
exactly one `__global_pointer$`, so two runtime `gp` values would make one reference set
address wild memory. ARM and RX have no `gp` small-data model, so none of this applies to
them.

## The payoff -- and exactly what is proven

Here is the honest scope, because the distinction is itself the lesson. There are TWO claims;
the first is RUN-PROVEN under enforcement on qemu-riscv, the second is silicon-proven for
coexistence and build-verified for U-mode.

**Claim 1 -- a confined UNPRIVILEGED thread throws under the MPU (RUN-PROVEN on qemu-riscv).**
This is the stronger claim the design is built for, and the one the chapter above describes. The
committed `cxxtest` spawns an unprivileged worker (`kos::thread::spawn(cxx_worker, ...,
privileged=false)`, 8 KB app-arena stack) that throws, catches, unwinds a non-trivial local
destructor, `dynamic_cast`s, and `typeid`s -- all under the MPU. On qemu-riscv (rv32imac PMP)
`qemu_riscv_cxxtest` runs it to ALL PASS from the U-mode worker: the nm-verified layout (heap +
`eh_globals` + the FDE registry + unwinder state inside the app's granted data region) plus the
RISC-V gp-in-appdata fix (Option 4) are EXERCISED by a running confined test, not merely
layout-inspected.

**Claim 2 -- the full-C++ runtime coexists with enforcement (silicon-proven, 5 arches).** With
the MPU/PMP turned on, a full-C++ app throws, catches, unwinds, `dynamic_cast`s, and `typeid`s,
and it all runs -- across three exception models:

- **K64F, XMC, RP2040** -- ARM EHABI. Tables found by linker symbol, no registration, no
  `gp`. `cxxtest` runs under SYSMPU (K64F) and PMSA (XMC, RP2040).
- **RX72M** -- GNURX SjLj. `.gcc_except_table` in ROM, a writable context chain in the app
  window, no `__register_frame`. Runs under the RX MPU.
- **ESP32-C6** (DWARF/RISC-V) -- the boot `__register_frame` hook, the
  `.eh_frame`-folded-into-code layout, and the `gp`-in-appdata move (Option 4). Runs under PMP.

The standing silicon ALL-PASS logs were the OLD cxxtest running in the **PRIVILEGED root thread**:
they prove the runtime + the EH-table-homing layout BOOT AND RUN with enforcement active. cxxtest
is now U-mode on those boards too, but only BUILD-VERIFIED here (it links clean under enforcement);
a bench re-flash of the now-U-mode cxxtest is the outstanding silicon U-mode proof.

Same runtime, same four pieces of memory, three exception models, one invariant: **read-only
tables ride the code grant, writable state rides the data grant, and a confined thread must never
reach past either** -- run-proven on qemu-riscv (U-mode) and proven to COEXIST with enforcement on
five silicon arches, with the silicon U-mode throw build-verified and pending a bench re-flash.

## Where to go next

- The region-set design and the qemu-riscv + silicon proofs:
  [`../design-cxx-under-mpu.md`](../design-cxx-under-mpu.md).
- The RISC-V `gp` option space and why "gp-in-appdata" is the right fix:
  [`../design-riscv-gp-split.md`](../design-riscv-gp-split.md).
- What the C++ runtime *is* (the three stacked libraries) and the boot-order story:
  [`whats-under-include-libc-and-the-cxx-runtime.md`](whats-under-include-libc-and-the-cxx-runtime.md).
- Where the writable floor comes from and how the linker partitions one image:
  [*Where your RAM goes*](where-your-ram-goes-full-cxx-memory-floor-and-the-linker-split.md).
- Memory protection itself: Chapter 7, *Memory protection (M2)*.
