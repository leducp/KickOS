/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * Linker script for the NXP i.MX RT1062 (Teensy 4.1), Cortex-M7.
 * (i.MX RT1060 Processor Reference Manual, Rev. 3.)
 *
 * Boot: FlexSPI serial-NOR XIP image (RM 9.6.1 / 9.6.3 / 9.7). The boot ROM
 * reads a 512-byte FlexSPI configuration block (FCB) at flash offset 0, then the
 * Image Vector Table at offset 0x1000 (RM Table 9-36: FlexSPI NOR IVT offset =
 * 0x1000), and jumps to IVT.entry. Code executes in place from FlexSPI at
 * 0x6000_0000; writable state lives in on-chip RAM.
 *
 *   FLASH : FlexSPI serial NOR @ 0x6000_0000 (Teensy 4.1: 8 MiB W25Q64)  (RM 3.2)
 *   RAM   : OCRAM2 512 KiB @ 0x2020_0000                                  (RM 3.2)
 *
 * OCRAM2 is a dedicated 512 KiB bank, independent of the configurable FlexRAM
 * (ITCM/DTCM/OCRAM) partition and available at reset with no IOMUXC_GPR setup, so
 * the image needs no FlexRAM programming. Moving
 * writable state (and hot code) to DTCM/ITCM is a performance follow-up; see
 * docs/design-teensy-rt1062.md.
 */

ENTRY(_boot_entry)

MEMORY
{
    FLASH (rx)  : ORIGIN = 0x60000000, LENGTH = 8M
    RAM   (rwx) : ORIGIN = 0x20200000, LENGTH = 512K
}

_estack = ORIGIN(RAM) + LENGTH(RAM);   /* initial MSP: top of OCRAM2 */
_kernel_stack_size = 8K;               /* boot/handler (MSP) stack */

/* App-owned globals get their own MPU region (M2, KICKOS_HAVE_MPU). The ARMv7-M
   PMSA needs a power-of-two region, base ALIGNED to its size (the encoder validates
   pow2 SIZE only; hardware snaps a mis-aligned base DOWN and would grant kernel RAM),
   so _appdata_size must be pow2 AND .appdata is ALIGN(_appdata_size) below. Inert at
   KICKOS_HAVE_MPU=0. */
#if KICKOS_HAVE_MPU
#ifndef KICKOS_APPDATA_SIZE
#define KICKOS_APPDATA_SIZE 128K
#endif
_appdata_size = KICKOS_APPDATA_SIZE;
#endif

SECTIONS
{
    /* FlexSPI serial-NOR configuration block (FCB), 512 B at flash offset 0
       (RM 9.6.3.1 / Table 9-15, Table 9-18). ROM-consumed; nothing references
       it, so KEEP retains it. */
    .boot_fcb ORIGIN(FLASH) :
    {
        KEEP(*(.boot_fcb))
    } > FLASH

    /* Image Vector Table @ 0x1000 and Boot Data @ 0x1020 (RM 9.7.1, Table 9-36:
       FlexSPI NOR IVT offset = 0x1000). Both ROM-consumed; KEEP them. */
    .boot_ivt ORIGIN(FLASH) + 0x1000 :
    {
        KEEP(*(.boot_ivt))
        KEEP(*(.boot_data))
    } > FLASH

    /* Vector table + code start after the 8 KiB boot-header window. VTOR is set
       to this address in _boot_entry (RM: the ROM enters via IVT.entry, not the
       reset vector, so the table need not sit at flash base). */
    .isr_vector ORIGIN(FLASH) + 0x2000 :
    {
        . = ALIGN(1024);   /* M7 VTOR alignment: >= table size, 1 KiB is ample */
        KEEP(*(.isr_vector))
    } > FLASH

    .text :
    {
        *(.text .text.*)
        *(.rodata .rodata.*)
        *(.glue_7) *(.glue_7t)
        KEEP(*(.init))
        KEEP(*(.fini))
        . = ALIGN(4);
    } > FLASH

    .ARM.extab :
    {
        *(.ARM.extab* .gnu.linkonce.armextab.*)
    } > FLASH

    /* Homed right after .ARM.extab (rp2040/rp2350 do): libstdc++ may emit
       .gcc_except_table even on EHABI; left orphaned a future full-C++ teensy image
       would drop it into a wrong slot. Read-only, reached via the app RX code grant. */
    .gcc_except_table :
    {
        *(.gcc_except_table .gcc_except_table.*)
    } > FLASH

    /* Global ctors are SPLIT (see mk64f.ld for the mechanism): the closed
       KickOS-owned set runs before kmain from .init_array; every other ctor runs
       from root_entry via .kickos_app_init_array (a ctor may issue a syscall). */
    .init_array :
    {
        __init_array_start = .;
        KEEP(*libkickos_kernel.a:*(SORT(.init_array.*) .init_array))
        KEEP(*libkickos_arch_armv7m.a:*(SORT(.init_array.*) .init_array))
        KEEP(*libkickos_chip_imxrt1062.a:*(SORT(.init_array.*) .init_array))
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

    /* Copy/zero tables for the generic C-runtime init (kickos_ranges_init). */
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
        LONG(__kickos_appdata_load_end); LONG(__kickos_appdata_end - __kickos_appdata_load_end);
#endif
        LONG(ADDR(.bss));    LONG(SIZEOF(.bss));
        __kickos_zero_table_end = .;
    } > FLASH

    _sidata = LOADADDR(.data);

#if KICKOS_HAVE_MPU
    /* Enforcement layout (see mk64f.ld for the inverted selector). The ARMv7-M PMSA is
       power-of-two granular, so the app window base + size must both be pow2. */
    .data : ALIGN(4)
    {
        _sdata = .;
        *libkickos_kernel.a:*(.data .data.*)
        *libkickos_arch_armv7m.a:*(.data .data.*)
        *libkickos_chip_imxrt1062.a:*(.data .data.*)
        *libkickos_lib.a:*(.data .data.*)
        . = ALIGN(4);
        _edata = .;
    } > RAM AT > FLASH

    .bss (NOLOAD) : ALIGN(4)
    {
        _sbss = .;
        *libkickos_kernel.a:*(.bss .bss.*)
        *libkickos_arch_armv7m.a:*(.bss .bss.*)
        *libkickos_chip_imxrt1062.a:*(.bss .bss.*)
        *libkickos_lib.a:*(.bss .bss.*)
        *libkickos_kernel.a:*(COMMON)
        *libkickos_arch_armv7m.a:*(COMMON)
        *libkickos_chip_imxrt1062.a:*(COMMON)
        *libkickos_lib.a:*(COMMON)
        . = ALIGN(4);
        _ebss = .;
    } > RAM

    .appdata : ALIGN(_appdata_size)
    {
        __kickos_appdata_start = .;
        *(.data .data.*)
        . = ALIGN(4);
        __kickos_appdata_load_end = .;
    } > RAM AT > FLASH

    /* Pin the base to the window (start + SIZEOF .appdata), NOT the default location
       counter: an EMPTY .appdata (an app with no initialized globals, e.g. the bench) does
       not carry its ALIGN(_appdata_size) forward, so a defaulted .appbss falls back to
       kernel-.bss-end, BELOW the granted window, and every app-global access faults. */
    .appbss (__kickos_appdata_start + SIZEOF(.appdata)) (NOLOAD) : ALIGN(4)
    {
        *(.bss .bss.* COMMON)
        . = ALIGN(4);
        _appdata_used_end = .;
        . = __kickos_appdata_start + _appdata_size;
        __kickos_appdata_end = .;
        /* The newlib heap IS the window pad: [ALIGN(used_end,8), window end). */
        _kickos_heap_start = ALIGN(_appdata_used_end, 8);
        _kickos_heap_limit = __kickos_appdata_end;
    } > RAM
#else
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
#endif

#if !KICKOS_HAVE_MPU
    /* Non-enforcement build: no .appdata window, so the newlib heap is an explicit
       section carved from RAM before the thread arena. */
    .userheap (NOLOAD) :
    {
        . = ALIGN(8);
        _kickos_heap_start = .;
        . += KICKOS_USER_HEAP_SIZE;
        _kickos_heap_limit = .;
    } > RAM
#endif

    . = ALIGN(32);
    __kickos_ram_start = .;
    __kickos_ram_end = _estack - _kernel_stack_size;

    /* Chip memory extents read by the non-enforcing user-pointer admission
       (arch_ram_common.cc). Symbols only, no layout effect. */
    __kickos_rom_start = ORIGIN(FLASH);
    __kickos_rom_end = ORIGIN(FLASH) + LENGTH(FLASH);
    __kickos_sram_start = ORIGIN(RAM);

    /* App code region for unprivileged threads (RX): the FlexSPI XIP window. */
    __kickos_code_start = ORIGIN(FLASH);
    __kickos_code_end = ORIGIN(FLASH) + LENGTH(FLASH);

    /* Boot Data.length: the on-flash image extent, from flash base to the end of
       the loaded .data (highest LMA). Referenced by the Boot Data structure. */
    __boot_image_length = LOADADDR(.data) + SIZEOF(.data) - ORIGIN(FLASH);

    /DISCARD/ : { *(.ARM.attributes) }
}

ASSERT(__kickos_ram_start <= __kickos_ram_end,
       "KickOS: static data overflows RAM (no room for the user-RAM pool + stack)")

#include <boot_arena.ld.h>
KICKOS_BOOT_ARENA_ASSERT(__kickos_ram_start, __kickos_ram_end)

KICKOS_POOL_ARENA_ASSERT(__kickos_ram_start, __kickos_ram_end)
ASSERT(__kickos_app_fini_array_end == __kickos_app_fini_array_start,
       "KickOS: .fini_array is non-empty -- a global with a non-trivial destructor? Static destructors are not supported; nothing would ever run it")

#if KICKOS_HAVE_MPU
ASSERT(_appdata_used_end <= __kickos_appdata_start + _appdata_size,
       "KickOS: app .data/.bss overflow _appdata_size (raise it to the next pow2)")
/* Lower bound: an empty .appdata dropping its ALIGN would place .appbss below the base. */
ASSERT(ADDR(.appbss) >= __kickos_appdata_start,
       "KickOS: .appbss placed below the app-data window base (empty .appdata alignment-drop regression)")
ASSERT(_ebss > _sbss,
       "KickOS: kernel .bss empty -- closed-set archive selector matched nothing (renamed lib?)")
#endif

/* Opt-in link-time heap floor (undefined by default). */
#ifdef KICKOS_HEAP_MIN
ASSERT(_kickos_heap_limit - _kickos_heap_start >= KICKOS_HEAP_MIN,
       "KickOS: heap below KICKOS_HEAP_MIN")
#endif
