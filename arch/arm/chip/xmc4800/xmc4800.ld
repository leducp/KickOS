/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * Linker script for the Infineon XMC4800-F144x2048, Cortex-M4F.
 *   Flash : 2 MiB  @ 0x0800_0000  (cached alias; uncached alias is 0x0C00_0000)
 *   SRAM  : 128 KiB @ 0x2000_0000  (DSRAM1; DSRAM2 @ 0x2002_0000 left unused)
 */

ENTRY(Reset_Handler)

MEMORY
{
    FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 2048K
    RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 128K
}

_estack = ORIGIN(RAM) + LENGTH(RAM);
_kernel_stack_size = 8K;

/* App-owned globals get their own PMSA region so an unprivileged thread reaches its
   own .data/.bss but not the kernel's. PMSA needs the region pow2-sized AND aligned
   to its size, so the app data is padded up to this pow2 and placed at ORIGIN(RAM)
   (already 4K-aligned); an app whose globals exceed it trips the ASSERT. Power of
   two only. Gated on enforcement: a no-MPU build reserves nothing (zero-overhead). */
#if KICKOS_HAVE_MPU
_appdata_size = 4K;
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
        KEEP(*libkickos_chip_xmc4800.a:*(SORT(.init_array.*) .init_array))
        KEEP(*libkickos_lib.a:*(SORT(.init_array.*) .init_array))
        __init_array_end = .;
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
        LONG(ADDR(.appbss)); LONG(SIZEOF(.appbss));
#endif
        LONG(ADDR(.bss));    LONG(SIZEOF(.bss));
        __kickos_zero_table_end = .;
    } > FLASH

#if KICKOS_HAVE_MPU
    /* App-owned globals as a pow2 PMSA region at the base of RAM (ORIGIN is 4K-
       aligned). Grouped by path (objects under a user/ dir = the app object + user
       runtime); placed BEFORE the kernel .data/.bss so those input sections are
       consumed here and the kernel catch-all below gets only kernel/arch/lib.
       .appdata is loaded from flash; .appbss + the pad are zeroed (no stale
       read-back through the grant). Gated on enforcement (zero-overhead). */
    .appdata ORIGIN(RAM) : ALIGN(4)
    {
        __kickos_appdata_start = .;
        *user*(.data .data.*)
        . = ALIGN(4);
    } > RAM AT > FLASH

    .appbss (NOLOAD) : ALIGN(4)
    {
        *user*(.bss .bss.*)
        *user*(COMMON)
        . = ALIGN(4);
        _appdata_used_end = .;
        . = __kickos_appdata_start + _appdata_size; /* pad the block up to pow2 */
        __kickos_appdata_end = .;
    } > RAM
#endif

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

    . = ALIGN(32);
    __kickos_ram_start = .;
    __kickos_ram_end = _estack - _kernel_stack_size;

    /* App code region for unprivileged threads (RX): the whole flash, a single
       power-of-two-aligned PMSA region (2 MiB @ 0x0800_0000). Lets an
       unprivileged thread fetch its instructions + read flash-resident rodata
       (string literals, const tables). Whole-flash means kernel code is also
       unprivileged-readable (confidentiality, not escalation) -- a tighter
       app-only .text region is a later refinement. */
    __kickos_code_start = ORIGIN(FLASH);
    __kickos_code_end = ORIGIN(FLASH) + LENGTH(FLASH);

    /DISCARD/ : { *(.ARM.attributes) }
}

ASSERT(__kickos_ram_start <= __kickos_ram_end,
       "KickOS: static data overflows RAM (no room for the user-RAM pool + stack)")

/* App globals must fit the pow2 region. If this trips, raise _appdata_size to the
   next power of two (it is the app's MPU-granted .data/.bss window). A zero-sized
   used-end means the user-path selector matched nothing -- check the build layout. */
#if KICKOS_HAVE_MPU
ASSERT(_appdata_used_end <= __kickos_appdata_start + _appdata_size,
       "KickOS: app .data/.bss overflow _appdata_size (raise it to the next pow2)")
#endif
