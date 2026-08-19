<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->

# Flash and RAM footprint: the decisions

> **Status: LANDED.** A decision list, not a measurement record. The numbers these rulings rest on
> are a dated capture and live in `archive/M4.5_footprint_meas.md`, which also holds the method, the
> per-board sweep and the traps. Quote a figure from there with its date, never from here.
> Two items below are open records rather than shipped work: R3 and the LTO defect.

The build-type recovery is taken: every configure preset sets `CMAKE_BUILD_TYPE=MinSizeRel` and `-g`
is re-added under it (`CMakeLists.txt`, the `MinSizeRel` `add_compile_options` line), because an image with no debug info cannot be witnessed
on silicon. What follows is what was decided around that, and what is still open.

## R2. It has to be `-Os`, not `-O1` and not `-O2`

Both alternatives were measured and both were rejected: on a 64 KiB part the difference is spent
margin. `-O2` gives back 2,716 bytes of the recovery on `bluepill-c8` `hello` and 4,324 on its
`selftest` image. `-O1` captures 79 to 87 percent (median 84) of the `-Os` recovery while costing
about the same as `-O2` per tight image, so it buys nothing.

Falsifier: a board class where `-Os` costs measurable throughput on a path that matters. No cycle or
instruction-cache measurement has ever been taken, so that case is open and unargued in either
direction.

## R3. The 64-bit division helper, 800 bytes. OPEN

`libgcc.a(_udivmoddi4.o)` is the only toolchain passenger on the freestanding leaf, 4.2 percent of
the `bluepill-c8` `hello` image and 860 bytes on `xmc4800-relax`. Three call sites, all reached by
disassembly: `arch_init`, `arch_timer_arm` (twice) and `emit_uint` in `libkickos_lib.a(fmt.cc.obj)`.
Recoverable only by changing those three to 32-bit arithmetic or shifts, so it is a code change and
not a flag. Not scheduled here.

## R4. The `.userheap` carve is a policy cost, not waste

8,192 bytes of SRAM on `bluepill-c8`, 40 percent of the part's RAM, and no app on the freestanding
leaf references it. It is kept anyway because it buys stdio BUFFERING and `malloc`, and surrendering
it costs exactly those. It is not required for `printf` or `std::cout`: newlib falls back to
unbuffered stdio when the buffer malloc fails (`arch/arm/chip/stm32f302/stm32f302.ld`, the `.userheap` carve), which
is why `nrf51` ships a zero carve and still prints. The standard-API rule therefore survives a
heapless profile; see `reference/porting.md`. The ruling is per board through the existing
`KICKOS_USER_HEAP_SIZE` knob, at the price of the buffering guarantee.

Consequence worth carrying: the tightest arena in the tree is a RAM-policy question, not a code-size
one.

## The `-Warray-bounds` pragma, not `--param=min-pagesize=0`

The RP2040 and RP2350 bootrom-header accessors read a constant address below GCC's assumed unmapped
page, so value-range propagation folds the address and reports the access as out of bounds. Both
fixes silence it and the param addresses the cause more directly, yet the pragma wins on scope:
**the param cannot be scoped in-source at all.** GCC rejects it in `#pragma GCC optimize` and in the
`optimize` function attribute, so its only home is the compile line, where it blinds roughly 450
lines of chip driver to every value-range null-page diagnostic. The pragma covers two one-line
function bodies (the `-Warray-bounds` push/pop around `r8`/`r16` in
`arch/arm/chip/rp2040/chip_rp2040.cc` and `arch/arm/chip/rp2350/chip_rp2350.cc`).

Demonstrated rather than argued: a null-page dereference introduced elsewhere in the same TU is
still caught with the pragma in place and is silently missed under the param.

## LTO does not link. STANDS, no fix attempted

`-flto` fails every app with `(.isr_vector+0x4): undefined reference to Reset_Handler`.
`Reset_Handler` is defined in a C++ TU and referenced only from the vector table in an assembly
object, so the LTO plugin sees no reason to keep the definition. The cause is identified and nothing
was attempted, so LTO is not an available footprint recovery today. It is not on the `-Os` path, so
it gates nothing. Recorded here so it is not rediscovered.
