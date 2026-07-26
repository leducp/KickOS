/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * Per-board linker override for a GENUINE STM32F103C8 "Blue Pill" (medium
 * density): 64 KiB flash / 20 KiB SRAM. Overrides the chip default
 * (arch/arm/chip/stm32f103/stm32f103.ld), which is sized to the 10 KiB
 * low-density floor for clones. Selected by CMake when KICKOS_BOARD=bluepill-c8.
 * HW-UNTESTED (only a low-density clone has been run on hardware).
 *   Flash : 64 KiB @ 0x0800_0000  (aliased to 0x0 at boot; vectors live here)
 *   SRAM  : 20 KiB @ 0x2000_0000
 */

ENTRY(Reset_Handler)

MEMORY
{
    FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 64K
    RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 20K
}

_estack = ORIGIN(RAM) + LENGTH(RAM);
_kernel_stack_size = 2K;

/* Newlib heap: an explicit .userheap section carved from RAM before the thread arena
   (no MPU .appdata window on this chip). 0 is legal for a board with no malloc app. */
#ifndef KICKOS_USER_HEAP_SIZE
#define KICKOS_USER_HEAP_SIZE 8K
#endif

SECTIONS
{
    .text :
    {
        KEEP(*(.isr_vector))
        *(.text .text.*)
        *(.rodata .rodata.*)
        *(.glue_7) *(.glue_7t)
        KEEP(*(.init))
        KEEP(*(.fini))
        . = ALIGN(4);
    } > FLASH

    /* Global ctors SPLIT -- this board override MUST mirror the chip default
       (arch/arm/chip/stm32f103/stm32f103.ld). It previously had a single monolithic
       .init_array, which is a two-fold bug: (1) every APP ctor ran from
       Reset_Handler, before kmain, so a ctor issuing a syscall (kos_clock_now needs
       ktime_init plus a current thread) had no kernel to call; and (2) the app-ctor
       bounds root_entry walks are declared WEAK in kmain.cc, so with no
       .kickos_app_init_array section they resolved to 0 and the walk was skipped
       ENTIRELY -- silently, with no diagnostic. Partition by input ARCHIVE, never by
       section name: an app TU using __attribute__((init_priority(N))) emits
       .init_array.0NNNN, which matches none of the four archive selectors and so
       falls through to the app bucket below. */
    .init_array :
    {
        __init_array_start = .;
        KEEP(*libkickos_kernel.a:*(SORT(.init_array.*) .init_array))
        KEEP(*libkickos_arch_armv7m.a:*(SORT(.init_array.*) .init_array))
        KEEP(*libkickos_chip_stm32f103.a:*(SORT(.init_array.*) .init_array))
        KEEP(*libkickos_lib.a:*(SORT(.init_array.*) .init_array))
        __init_array_end = .;
    } > FLASH

    /* Teardown mirror of the ctor split: place .fini_array on the APP side (never the
       privileged bucket) so an unplaced one cannot be ORPHANED outside every stated
       boundary, and ASSERT it empty -- nothing walks it. Full rationale in
       arch/arm/chip/stm32f411/stm32f411.ld. */
    .kickos_app_fini_array :
    {
        __kickos_app_fini_array_start = .;
        KEEP(*(SORT(.fini_array.*)))
        KEEP(*(.fini_array))
        __kickos_app_fini_array_end = .;
    } > FLASH

    .kickos_app_init_array :
    {
        __kickos_app_init_array_start = .;
        KEEP(*(SORT(.init_array.*)))
        KEEP(*(.init_array))
        __kickos_app_init_array_end = .;
    } > FLASH

    .ARM.exidx :
    {
        __exidx_start = .;
        *(.ARM.exidx* .gnu.linkonce.armexidx.*)
        __exidx_end = .;
    } > FLASH

    _sidata = LOADADDR(.data);

    .data : ALIGN(4)
    {
        _sdata = .;
        *(.data .data.*)
        . = ALIGN(4);
        _edata = .;
    } > RAM AT > FLASH

    .bss (NOLOAD) : ALIGN(4)
    {
        _sbss = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(4);
        _ebss = .;
    } > RAM

    .userheap (NOLOAD) :
    {
        . = ALIGN(8);
        _kickos_heap_start = .;
        . += KICKOS_USER_HEAP_SIZE;
        _kickos_heap_limit = .;
    } > RAM

    . = ALIGN(32);
    __kickos_ram_start = .;
    __kickos_ram_end = _estack - _kernel_stack_size;

    /DISCARD/ : { *(.ARM.attributes) }
}

ASSERT(__kickos_ram_start <= __kickos_ram_end,
       "KickOS: static data overflows RAM (no room for the user-RAM pool + stack)")

/* Static destructors are unsupported: nothing walks .fini_array (see the section
   comment), so a global with a non-trivial destructor is silently dropped rather than
   run. Fail the link instead of shipping a destructor that never fires. */
ASSERT(__kickos_app_fini_array_end == __kickos_app_fini_array_start,
       "KickOS: .fini_array is non-empty -- a global with a non-trivial destructor? Static destructors are not supported; nothing would ever run it")
/* Opt-in link-time heap floor (undefined by default). */
#ifdef KICKOS_HEAP_MIN
ASSERT(_kickos_heap_limit - _kickos_heap_start >= KICKOS_HEAP_MIN,
       "KickOS: heap below KICKOS_HEAP_MIN")
#endif
