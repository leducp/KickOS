/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * Linker script for the STM32F103C8, Cortex-M3: 64 KiB flash / 20 KiB SRAM, the
 * genuine medium-density part. A low-density clone (32 KiB / 10 KiB, DEV_ID 0x412)
 * is a DIFFERENT part and would need its own chip entry, not a floor smuggled in
 * here: at 20 KiB the initial SP lands past a 10 KiB die and the first push
 * hardfaults before main, which is the honest failure for a part we do not claim.
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

    /* Global ctors SPLIT: .init_array keeps ONLY the closed KickOS-owned set
       (kernel/arch/chip/lib), run early by Reset_Handler; other (app/lib) ctors land
       in .kickos_app_init_array and run from root_entry with the kernel live. A
       monolithic .init_array instead runs app ctors before kmain AND leaves the weak
       app-ctor bounds in kmain.cc at 0, so the root_entry walk is skipped SILENTLY.
       Partition by input ARCHIVE, never by section name: init_priority(N) emits
       .init_array.0NNNN, which no archive selector matches. */
    .init_array :
    {
        __init_array_start = .;
        KEEP(*libkickos_kernel.a:*(SORT(.init_array.*) .init_array))
        KEEP(*libkickos_arch_armv7m.a:*(SORT(.init_array.*) .init_array))
        KEEP(*libkickos_chip_stm32f103.a:*(SORT(.init_array.*) .init_array))
        KEEP(*libkickos_lib.a:*(SORT(.init_array.*) .init_array))
        __init_array_end = .;
    } > FLASH

    /* .fini_array, app side, ASSERTed empty below; nothing walks it. See
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

#include <boot_arena.ld.h>
KICKOS_BOOT_ARENA_ASSERT(__kickos_ram_start, __kickos_ram_end)

/* Static destructors are unsupported: nothing walks .fini_array. Fail the link
   rather than silently drop a destructor. */
ASSERT(__kickos_app_fini_array_end == __kickos_app_fini_array_start,
       "KickOS: .fini_array is non-empty -- a global with a non-trivial destructor? Static destructors are not supported; nothing would ever run it")
/* Opt-in link-time heap floor (undefined by default). */
#ifdef KICKOS_HEAP_MIN
ASSERT(_kickos_heap_limit - _kickos_heap_start >= KICKOS_HEAP_MIN,
       "KickOS: heap below KICKOS_HEAP_MIN")
#endif
