/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * Linker script for the STM32F103, Cortex-M3. This is the CHIP DEFAULT, sized to
 * the LOW-DENSITY floor (10 KiB SRAM) because "Blue Pill" boards are very often
 * low-density parts/clones (32 KiB flash / 10 KiB SRAM, DEV_ID 0x412). At 20 KiB
 * the initial SP (_estack = top of RAM) lands past a 10 KiB die -> the first push
 * hardfaults before main. 10 KiB runs on both variants (a real C8 just wastes the
 * upper half). A board that KNOWS it has the full 20 KiB C8 overrides this with
 * its own linker script (see boards/bluepill-c8/stm32f103.ld).
 *   Flash : 32 KiB @ 0x0800_0000  (aliased to 0x0 at boot; vectors live here)
 *   SRAM  : 10 KiB @ 0x2000_0000
 * (both are the low-density floor: a genuine C8 has 64 KiB/20 KiB and overrides
 *  via boards/bluepill-c8/stm32f103.ld -- so this catches a >32 KiB image at
 *  link time rather than letting it fail to fit an LD clone at flash time.)
 */

ENTRY(Reset_Handler)

MEMORY
{
    FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 32K
    RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 10K
}

_estack = ORIGIN(RAM) + LENGTH(RAM);
_kernel_stack_size = 2K;

/* Newlib heap: an explicit .userheap section carved from RAM before the thread arena
   (no MPU .appdata window on this chip). Tiny on the 10 KiB low-density floor; 0 is
   legal for a board with no malloc app. A too-big value overflows RAM at link (loud). */
#ifndef KICKOS_USER_HEAP_SIZE
#define KICKOS_USER_HEAP_SIZE 2K
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

    /* Global ctors SPLIT: .init_array keeps ONLY the closed KickOS-owned set
       (kernel/arch/chip/lib), run early by Reset_Handler; other (app/lib) ctors
       land in .kickos_app_init_array and run from root_entry with the kernel live. */
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
