/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * Linker script for the ESP32 (WROOM-32 module), Xtensa LX6. The image links
 * ENTIRELY into internal SRAM (no flash cache / XIP), so the ROM loader copies
 * every segment to its RAM load address and jumps: no second stage, no MMU
 * setup (see startup.S). Addresses are from the ESP32 TRM
 * 1.3.2 "Embedded Memory" (Table 1-3, internal SRAM address mapping):
 *
 *   Internal SRAM0 (IRAM, exec): 0x4007_0000 .. 0x4009_FFFF (192 KiB; the low
 *                                64 KiB is the cache region), so code links at
 *                                0x4008_0000.
 *   Internal SRAM2 (DRAM, data): 0x3FFA_E000 .. 0x3FFD_FFFF (200 KiB), so data
 *                                links at 0x3FFB_0000.
 *
 * IRAM is word-access only, so .rodata (byte-addressable strings/const data) lives
 * in DRAM, not with .text.
 *
 * Windowed ABI: the mandatory WindowOverflow/Underflow vectors are part of the
 * .vectors table placed at VECBASE (startup.S); they spill/fill to each thread's
 * OWN stack, so no dedicated exception stack is reserved here.
 */

#include <sections.ld.h>

ENTRY(_start)

MEMORY
{
    IRAM (rx)  : ORIGIN = 0x40080000, LENGTH = 128K
    DRAM (rw)  : ORIGIN = 0x3FFB0000, LENGTH = 192K
}

_estack = ORIGIN(DRAM) + LENGTH(DRAM); /* core 0's boot stack top: top of DRAM */
_kernel_stack_size = 8K;               /* boot/interrupt stack reserve, PER CORE */

/* ONE BOOT STACK PER CORE, stacked downward from the top of DRAM. The reserve below
 * __kickos_ram_end grows with the count, which makes __kickos_ram_start <= __kickos_ram_end
 * the check that the second stack FITS. */
#if KICKOS_NUM_CORES > 1
_estack_core1 = _estack - _kernel_stack_size;
#endif

SECTIONS
{
    /* Xtensa exception/interrupt vectors at VECBASE. Must be 1 KiB aligned; the
       startup sets wsr.vecbase to _kickos_vectors. Placed at IRAM origin. */
    .vectors ORIGIN(IRAM) :
    {
        _kickos_vectors = .;
        KEEP(*(.vectors))
        . = ALIGN(4);
    } > IRAM

    .text : ALIGN(4)
    {
        *(.literal .literal.*)
        *(.text .text.*)
        KICKOS_CODE_DEBRIS_SECTIONS
        . = ALIGN(4);
    } > IRAM

    .rodata : ALIGN(4)
    {
        *(.rodata .rodata.*)
        *(.rodata_wlog .rodata_wlog.*)
        . = ALIGN(4);
    } > DRAM

    /* Global ctors SPLIT: .init_array keeps ONLY the closed KickOS-owned set
       (kernel/arch/chip/lib), run early by Reset_Handler; other (app/lib) ctors
       land in .kickos_app_init_array and run from root_entry with the kernel live. */
    .init_array : ALIGN(4)
    {
        __init_array_start = .;
        KEEP(*libkickos_kernel.a:*(SORT(.init_array.*) .init_array))
        KEEP(*libkickos_arch_lx6.a:*(SORT(.init_array.*) .init_array))
        KEEP(*libkickos_chip_esp32.a:*(SORT(.init_array.*) .init_array))
        KEEP(*libkickos_lib.a:*(SORT(.init_array.*) .init_array))
        __init_array_end = .;
    } > DRAM

    /* .fini_array, app side, ASSERTed empty below; nothing walks it. See
       arch/arm/chip/stm32f411/stm32f411.ld. */
    .kickos_app_fini_array : ALIGN(4)
    {
        __kickos_app_fini_array_start = .;
        KEEP(*(SORT(.fini_array.*)))
        KEEP(*(.fini_array))
        __kickos_app_fini_array_end = .;
    } > DRAM

    KICKOS_TLS_TEMPLATE(DRAM)

    .kickos_app_init_array : ALIGN(4)
    {
        __kickos_app_init_array_start = .;
        KEEP(*(SORT(.init_array.*)))
        KEEP(*(.init_array))
        __kickos_app_init_array_end = .;
    } > DRAM

    _sidata = LOADADDR(.data);

    /* NO `AT` clause: the ROM first-stage loader places each segment at its VMA, so a
       load address that differs from the VMA is never written by anything and the
       Reset_Handler copy would read uninitialized SRAM. Pinned by an ASSERT below. */
    .data : ALIGN(4)
    {
        _sdata = .;
        *(.data .data.*)
        . = ALIGN(4);
        _edata = .;
    } > DRAM

    .bss (NOLOAD) : ALIGN(4)
    {
        _sbss = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(4);
        _ebss = .;
    } > DRAM

    .userheap (NOLOAD) :
    {
        . = ALIGN(8);
        _kickos_heap_start = .;
        . += KICKOS_USER_HEAP_SIZE;
        _kickos_heap_limit = .;
    } > DRAM

    /* User-RAM pool (arch_ram_alloc) between static data and the reserved stack. */
    . = ALIGN(16);
    __kickos_ram_start = .;
    __kickos_ram_end = _estack - KICKOS_NUM_CORES * _kernel_stack_size;

    /* Chip memory extents read by the non-enforcing user-pointer admission
       (arch_ram_common.cc). Symbols only, no layout effect. */
    __kickos_rom_start = ORIGIN(IRAM);
    __kickos_rom_end = ORIGIN(IRAM) + LENGTH(IRAM);
    __kickos_sram_start = ORIGIN(DRAM);

    /DISCARD/ : { *(.xtensa.info) *(.comment) *(.xt.*) }

    KICKOS_NONALLOC_SECTIONS
}

KICKOS_STATIC_RELOC_ASSERT()
KICKOS_TLS_ALIGN_ASSERT()
KICKOS_TLS_FIT_ASSERT()

ASSERT(__kickos_ram_start <= __kickos_ram_end,
       "KickOS: static data overflows DRAM (no room for the user-RAM pool + stack)")

#include <boot_arena.ld.h>
KICKOS_BOOT_ARENA_ASSERT(__kickos_ram_start, __kickos_ram_end)

KICKOS_POOL_ARENA_ASSERT(__kickos_ram_start, __kickos_ram_end)
/* The bootable image is built from section VMAs (esptool elf2image) and the ROM
   first-stage loader places every segment at its VMA, so a load address that differs
   from the VMA is never written by anything. Reset_Handler's LMA->VMA copy would then
   read uninitialized SRAM and overwrite the correctly loaded initialized data, silently
   corrupting whatever global lands there. Any `AT` clause on a loadable section trips
   this. */
ASSERT(_sidata == _sdata,
       "KickOS: .data LMA != VMA: the ROM loader places segments at their VMAs, so the LMA holds no image bytes (drop the AT clause)")

ASSERT(__kickos_app_fini_array_end == __kickos_app_fini_array_start,
       "KickOS: .fini_array is non-empty: a global with a non-trivial destructor? Static destructors are not supported; nothing would ever run it")

/* Opt-in link-time heap floor (undefined by default). */
#ifdef KICKOS_HEAP_MIN
ASSERT(_kickos_heap_limit - _kickos_heap_start >= KICKOS_HEAP_MIN,
       "KickOS: heap below KICKOS_HEAP_MIN")
#endif

/* --- The coherency condition, as a refusal ---------------------------------------------
 * Both CPUs reach embedded memory with no cache in the path: the two 32 KiB caches are
 * carved out of SRAM0 and serve the external flash and PSRAM path alone, and the TRM
 * documents no invalidate anywhere and no flush but a whole-cache one that DISCARDS dirty
 * data. Shared kernel state is coherent across the two CPUs only while it is IN internal
 * SRAM. Unconditional, not gated on the core count.
 *
 * The bounds are the SoC's, from TRM v5.8 Table 3.3-2, and NOT this script's own regions:
 * an assert reading ORIGIN(DRAM) against LENGTH(DRAM) passes wherever the region is
 * repointed.
 *
 * Data bus: SRAM2 opens at 0x3FFA_E000 and SRAM1 closes at 0x3FFF_FFFF, contiguous.
 * Instruction bus: the low 64 KiB of SRAM0 IS the cache pool, so the floor is 0x4008_0000
 * rather than SRAM0's own 0x4007_0000, and SRAM1's instruction alias closes at 0x400B_FFFF.
 */
#define KICKOS_ESP32_ISRAM_DATA_FLOOR  0x3FFAE000
#define KICKOS_ESP32_ISRAM_DATA_CEIL   0x40000000
#define KICKOS_ESP32_ISRAM_TEXT_FLOOR  0x40080000
#define KICKOS_ESP32_ISRAM_TEXT_CEIL   0x400C0000

ASSERT(ADDR(.rodata) >= KICKOS_ESP32_ISRAM_DATA_FLOOR && _estack <= KICKOS_ESP32_ISRAM_DATA_CEIL,
       "KickOS: kernel data leaves internal SRAM. Run queues, control blocks, the capability table and the kernel lock word are coherent across the two CPUs only by there being no cache over them, and the caches sit on the external path; a shared kernel over this placement loses writes with no fault")

ASSERT(__kickos_ram_end >= KICKOS_ESP32_ISRAM_DATA_FLOOR && _estack <= KICKOS_ESP32_ISRAM_DATA_CEIL,
       "KickOS: a boot stack leaves internal SRAM. Every core's boot and interrupt stack is carved below _estack, and the level-1 entry spills the interruptee's register windows onto whichever stack it is standing on; behind a cache neither core can invalidate, that spill is lost rather than faulted")

ASSERT(_kickos_vectors >= KICKOS_ESP32_ISRAM_TEXT_FLOOR && ADDR(.text) + SIZEOF(.text) <= KICKOS_ESP32_ISRAM_TEXT_CEIL,
       "KickOS: kernel text leaves internal SRAM, or reaches into the SRAM0 cache pool. Both CPUs fetch it directly and neither can invalidate the other's copy; the only flush this part has discards dirty data")
