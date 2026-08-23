/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * Linker script for the Nordic nRF51822 (BBC micro:bit v1), Cortex-M0. This is
 * the runnable armv6m CI/verification target under QEMU (-M microbit).
 *   Flash : 256 KiB @ 0x0000_0000
 *   SRAM  :  16 KiB @ 0x2000_0000   (tiny; provisioning is sized down to match)
 */

#include <sections.ld.h>

ENTRY(Reset_Handler)

/* 32 KiB AND NOT THE 16 A BBC micro:bit v1 HAS. That part is EOL and nothing in this tree
   flashes one; what this board is FOR is being the fleet's only executable armv6m target,
   and 16 KiB made it a target that could not run what every other board runs. The nRF51822
   ships in a 32 KiB variant (QFAC), so this is a real part, just not the one on a micro:bit.

   QEMU HAS TO BE TOLD. Its microbit machine defaults to 16 KiB and ignores -m; the size is a
   QOM property, so every invocation passes -global nrf51-soc.sram-size=32768
   (cmake/kickos.cmake, kickos_add_qemu_test). Without it an image linked here locks up on
   its first push, before any vector table is live, as "can't escalate 3 to HardFault". */
MEMORY
{
    FLASH (rx)  : ORIGIN = 0x00000000, LENGTH = 256K
    RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 32K
}

_estack = ORIGIN(RAM) + LENGTH(RAM);
_kernel_stack_size = 2K;

SECTIONS
{
    .text :
    {
        KEEP(*(.isr_vector))
        *(.text .text.*)
        *(.rodata .rodata.*)
        *(.glue_7) *(.glue_7t)
        KICKOS_CODE_DEBRIS_SECTIONS
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
        KEEP(*libkickos_arch_armv6m.a:*(SORT(.init_array.*) .init_array))
        KEEP(*libkickos_chip_nrf51.a:*(SORT(.init_array.*) .init_array))
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

    KICKOS_TLS_TEMPLATE(FLASH)

    .kickos_app_init_array :
    {
        __kickos_app_init_array_start = .;
        KEEP(*(SORT(.init_array.*)))
        KEEP(*(.init_array))
        __kickos_app_init_array_end = .;
    } > FLASH

    /* EHABI unwind instructions + LSDA, homed in the code window so a full-C++ image does
       not orphan them into whatever gap follows the vector table. libstdc++ emits
       .gcc_except_table even on EHABI. */
    .ARM.extab :
    {
        *(.ARM.extab* .gnu.linkonce.armextab.*)
    } > FLASH

    .gcc_except_table :
    {
        *(.gcc_except_table .gcc_except_table.*)
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

    /* Kept even at the zero length this board configures: _sbrk is force-linked into
       every image (-u,_exit shares its TU), so its strong reference to
       _kickos_heap_start would break every link here. An allocating app fails at
       RUNTIME (malloc returns NULL), not at link. */
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

    /* Chip memory extents read by the non-enforcing user-pointer admission
       (arch_ram_common.cc). Symbols only, no layout effect. */
    __kickos_rom_start = ORIGIN(FLASH);
    __kickos_rom_end = ORIGIN(FLASH) + LENGTH(FLASH);
    __kickos_sram_start = ORIGIN(RAM);

    /DISCARD/ : { *(.ARM.attributes) }

    KICKOS_NONALLOC_SECTIONS
}

KICKOS_STATIC_RELOC_ASSERT()
KICKOS_TLS_ALIGN_ASSERT()

ASSERT(__kickos_ram_start <= __kickos_ram_end,
       "KickOS: static data overflows RAM (no room for the user-RAM pool + stack)")

#include <boot_arena.ld.h>
KICKOS_BOOT_ARENA_ASSERT(__kickos_ram_start, __kickos_ram_end)

KICKOS_POOL_ARENA_ASSERT(__kickos_ram_start, __kickos_ram_end)
ASSERT(__kickos_app_fini_array_end == __kickos_app_fini_array_start,
       "KickOS: .fini_array is non-empty: a global with a non-trivial destructor? Static destructors are not supported; nothing would ever run it")

/* Opt-in link-time heap floor (undefined by default). The default heap here is empty,
   so any floor fails the link unless KICKOS_USER_HEAP_SIZE is raised to match. */
#ifdef KICKOS_HEAP_MIN
ASSERT(_kickos_heap_limit - _kickos_heap_start >= KICKOS_HEAP_MIN,
       "KickOS: heap below KICKOS_HEAP_MIN")
#endif
