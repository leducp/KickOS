/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * Linker script for the QEMU `mps2-an386` (ARM MPS2 FPGA, Cortex-M4F), the
 * runnable armv7m CI/verification target. QEMU maps a code region at
 * 0x00000000 (vectors read at reset, VTOR=0) and SRAM at 0x20000000.
 *
 * The same map serves `qemu-m7` (mps2-an500) and `qemu-m3` (mps2-an385): those
 * images differ in core, not memory map. `qemu-m33` (mps2-an505) needs the
 * override in boards/qemu-m33/mps2.ld.
 *
 * Chip<->arch contract: define __kickos_ram_start/__kickos_ram_end (the
 * arch_ram_alloc pool) and _estack (initial MSP = vector[0]).
 */

#include <sections.ld.h>

ENTRY(Reset_Handler)

MEMORY
{
    FLASH (rx)  : ORIGIN = 0x00000000, LENGTH = 4M
    RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 4M
}

_estack = ORIGIN(RAM) + LENGTH(RAM);   /* initial MSP: top of RAM */
_kernel_stack_size = 16K;              /* boot/handler (MSP) stack */

/* App-owned globals get their own PMSA region so an unprivileged thread reaches its
   own .data/.bss but not the kernel's. PMSA needs the region pow2-sized AND aligned
   to its size, so the app window is padded to this pow2 and section-aligned to its
   size above the kernel data; an app whose globals exceed it trips the ASSERT.
   The window pad IS the newlib heap. 4 MiB of RAM here affords a 64K pad, which covers
   the selftest image's app-side .bss plus the ~5.9K measured cxxtest heap peak. Power of
   two only. */
#if KICKOS_HAVE_MPU
#ifndef KICKOS_APPDATA_SIZE
#define KICKOS_APPDATA_SIZE 64K
#endif
_appdata_size = KICKOS_APPDATA_SIZE;
#endif

SECTIONS
{
    .isr_vector ORIGIN(FLASH) :
    {
        KEEP(*(.isr_vector))
    } > FLASH

    .text :
    {
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
        KEEP(*libkickos_arch_armv7m.a:*(SORT(.init_array.*) .init_array))
        KEEP(*libkickos_chip_mps2.a:*(SORT(.init_array.*) .init_array))
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

    /* Copy/zero tables for the generic C-runtime init (kickos_ranges_init): flash
       rodata, read before .data exists. Copy triples {src,dst,len}; zero pairs
       {dst,len}. App ranges first, kernel ranges second (order is immaterial). */
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
        /* Zero the whole granted window past the loaded .appdata: the pre-.appbss
           alignment gap (forced by an over-aligned .appbss object), .appbss, and the
           and the pad, so no stale read-back anywhere in the app grant. */
        LONG(__kickos_appdata_load_end); LONG(__kickos_appdata_end - __kickos_appdata_load_end);
#endif
        LONG(ADDR(.bss));    LONG(SIZEOF(.bss));
        __kickos_zero_table_end = .;
    } > FLASH

    _sidata = LOADADDR(.data);

#if KICKOS_HAVE_MPU
    /* Enforcement layout (mirrors stm32f411.ld / mk64f.ld). Writable state is SPLIT
       kernel-side vs app-side with an INVERTED selection: the CLOSED KickOS-owned set
       (kernel/arch/chip/lib) is captured FIRST, in script order, via the
       `archive:member` colon syntax into the kernel .data/.bss at the RAM base; the
       .appdata/.appbss catch-alls that follow then take EVERYTHING ELSE: the app's
       own objects, the toolchain runtime (libc/libgcc/libstdc++/libsupc++), KickCAT,
       libkickos_user.a's heap, and any unknown NEW archive. An unmatched archive lands
       app-side (reachable), never kernel-side (faults). */
    .data : ALIGN(4)
    {
        _sdata = .;
        *libkickos_kernel.a:*(.data .data.*)
        *libkickos_arch_armv7m.a:*(.data .data.*)
        *libkickos_chip_mps2.a:*(.data .data.*)
        *libkickos_lib.a:*(.data .data.*)
        . = ALIGN(4);
        _edata = .;
    } > RAM AT > FLASH

    .bss (NOLOAD) : ALIGN(4)
    {
        _sbss = .;
        *libkickos_kernel.a:*(.bss .bss.*)
        *libkickos_arch_armv7m.a:*(.bss .bss.*)
        *libkickos_chip_mps2.a:*(.bss .bss.*)
        *libkickos_lib.a:*(.bss .bss.*)
        *libkickos_kernel.a:*(COMMON)
        *libkickos_arch_armv7m.a:*(COMMON)
        *libkickos_chip_mps2.a:*(COMMON)
        *libkickos_lib.a:*(COMMON)
        . = ALIGN(4);
        _ebss = .;
    } > RAM

    /* App-granted RW window: pow2 PMSA region, base aligned to its size by the section-
       level ALIGN (a top-level `. = ALIGN()` is swallowed by the region pointer under
       `> RAM`). All remaining .data/.bss/COMMON (app + runtime + KickCAT). .appdata is
       loaded from flash; the gap past it + .appbss + the pad are zeroed. */
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
        /* ld evaluates ASSERTs after layout, so a bare `. = start + size` that moved
           backwards would fail the link ahead of the overflow ASSERT below. MAX keeps
           that ASSERT the thing that prints. */
        . = MAX(., __kickos_appdata_start + _appdata_size); /* pad the granted window */
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

    /* Everything from here up to _kernel_stack_size below _estack is the
       arch_ram_alloc pool (runtime bump-allocated, so just symbols; no section
       to reserve). The top _kernel_stack_size bytes are the MSP stack. */
    . = ALIGN(32);
    __kickos_ram_start = .;
    __kickos_ram_end = _estack - _kernel_stack_size;

    /* Chip memory extents read by the non-enforcing user-pointer admission
       (arch_ram_common.cc). Symbols only, no layout effect. */
    __kickos_rom_start = ORIGIN(FLASH);
    __kickos_rom_end = ORIGIN(FLASH) + LENGTH(FLASH);
    __kickos_sram_start = ORIGIN(RAM);

#if KICKOS_HAVE_MPU
    /* App code region for unprivileged threads (RX): the whole flash, a pow2 PMSA
       region (4 MiB @ 0x0000_0000, base aligned to its size). Lets an unprivileged
       thread fetch its code + read flash rodata. Kernel code is also unprivileged-
       readable (confidentiality, not escalation). */
    __kickos_code_start = ORIGIN(FLASH);
    __kickos_code_end = ORIGIN(FLASH) + LENGTH(FLASH);
#endif

    /DISCARD/ : { *(.ARM.attributes) }

    KICKOS_NONALLOC_SECTIONS
}

KICKOS_STATIC_RELOC_ASSERT()

/* Static data (.data+.bss) must not eat into the user-RAM pool / MSP stack.
   Link-time guard: arch_ram_size() = end - start; a negative diff would wrap to
   a huge pool. Critical on small-SRAM parts (this .ld is the item-10 template). */
ASSERT(__kickos_ram_start <= __kickos_ram_end,
       "KickOS: static data overflows RAM (no room for the user-RAM pool + stack)")

#include <boot_arena.ld.h>
KICKOS_BOOT_ARENA_ASSERT(__kickos_ram_start, __kickos_ram_end)

KICKOS_POOL_ARENA_ASSERT(__kickos_ram_start, __kickos_ram_end)
ASSERT(__kickos_app_fini_array_end == __kickos_app_fini_array_start,
       "KickOS: .fini_array is non-empty: a global with a non-trivial destructor? Static destructors are not supported; nothing would ever run it")

/* App globals must fit the pow2 region. If this trips, raise _appdata_size to the
   next power of two (it is the app's MPU-granted .data/.bss window). */
#if KICKOS_HAVE_MPU
ASSERT(_appdata_used_end <= __kickos_appdata_start + _appdata_size,
       "KickOS: app .data/.bss overflow _appdata_size (raise it to the next pow2)")
/* Lower bound: an empty .appdata dropping its ALIGN would place .appbss below the base. */
ASSERT(ADDR(.appbss) >= __kickos_appdata_start,
       "KickOS: .appbss placed below the app-data window base (empty .appdata alignment-drop regression)")
/* Selector-mismatch guard. The inverted scheme captures the CLOSED KickOS-owned set
   into kernel .data/.bss and lets .appdata/.appbss catch the rest, so a broken selector
   shows up as an EMPTY KERNEL side (kernel writable state silently leaked into the app
   window). libkickos_kernel.a always contributes .bss, so an empty kernel .bss is the
   loud signal the colon selectors matched nothing (a renamed lib). Only the kernel side
   is asserted: an empty .appdata is legitimate, a minimal freestanding app being
   --gc-sections'd bare. */
ASSERT(_ebss > _sbss,
       "KickOS: kernel .bss empty: closed-set archive selector matched nothing (renamed lib?)")
#endif

/* Opt-in link-time heap floor (undefined by default). */
#ifdef KICKOS_HEAP_MIN
ASSERT(_kickos_heap_limit - _kickos_heap_start >= KICKOS_HEAP_MIN,
       "KickOS: heap below KICKOS_HEAP_MIN")
#endif
