/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * Linker script for the Raspberry Pi RP2350 (Cortex-M33) executing in place (XIP)
 * from external QSPI flash at 0x1000_0000. This board carries 16 MiB QSPI.
 *
 * BOOT (datasheet 5.9.5): unlike the RP2040 there is NO 256-byte checksummed boot2
 * and no separate XIP-setup stage. The RP2350 bootrom sets up QSPI XIP itself, then
 * scans the first 4 KiB of the image for an IMAGE_DEF block-loop; with no
 * ENTRY_POINT/VECTOR_TABLE item it reads the initial SP/PC from the vector table at
 * the image base. So the vector table is PINNED at the flash origin (0x1000_0000)
 * and the IMAGE_DEF block (startup.S, .image_def) is homed immediately after it --
 * both inside the 4 KiB scan window. This is the RP2040 boot2/vector-slot lesson
 * (rp2040.ld pinned .text) applied to the new boot format: the vector VMA is fixed
 * by section placement, not left to whatever the linker packs ahead of it.
 *
 * SRAM (datasheet 2.2.3): 520 KiB total. The lower 512 KiB (0x2000_0000..0x2008_0000)
 * is the striped region used here as main RAM; the two 4 KiB non-striped banks at
 * 0x2008_0000/0x2008_1000 are left reserved (future per-core stacks).
 *
 * ENFORCEMENT (KICKOS_HAVE_MPU): the armv8-m / PMSAv8 MPU backend
 * (arch/arm/common/arch_arm_pmsav8.cc, docs/design-rp2350-mpu-armv8m.md). PMSAv8 uses
 * RBAR/RLAR base+limit at a 32-byte granule (NON-pow2, like SYSMPU), so the app-data
 * window is just two 32-byte-aligned symbols -- NONE of the pow2 .appdata machinery of
 * the v6-M/v7-M PMSA chips (rp2040.ld ALIGN(pow2)) is needed. The writable state is
 * split KERNEL vs app exactly as mk64f.ld (the other byte-granular backend). A
 * non-enforcement build (KICKOS_HAVE_MPU=0) reserves no window and is unchanged.
 */

ENTRY(Reset_Handler)

MEMORY
{
    FLASH (rx)  : ORIGIN = 0x10000000, LENGTH = 16M
    RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 512K
}

_estack = ORIGIN(RAM) + LENGTH(RAM);
_kernel_stack_size = 8K;

/* App-owned globals get their own MPU region so an unprivileged thread reaches its own
   .data/.bss but not the kernel's. PMSAv8 is 32-byte granular with an arbitrary base
   (no pow2 rule), so the window base need only be 32-byte aligned and the size a
   32-byte multiple; the app data is padded up to _appdata_size for a fixed grant.
   Overridable (a full-C++ build needs a larger heap + libstdc++/unwind window). */
#if KICKOS_HAVE_MPU
#ifndef KICKOS_APPDATA_SIZE
#define KICKOS_APPDATA_SIZE 128K
#endif
_appdata_size = KICKOS_APPDATA_SIZE;
#endif

SECTIONS
{
    /* .text VMA is PINNED to the flash origin: the vector table is the first thing
       here (bootrom reads SP/PC from image base), and the IMAGE_DEF block follows
       it (still within the bootrom's first-4-KiB scan window). */
    .text ORIGIN(FLASH) :
    {
        KEEP(*(.isr_vector))   /* vector table at 0x1000_0000 */
        KEEP(*(.image_def))    /* boot IMAGE_DEF block, right after the vectors */
        *(.text .text.*)
        *(.rodata .rodata.*)
        *(.glue_7) *(.glue_7t)
        KEEP(*(.init))
        KEEP(*(.fini))
        . = ALIGN(4);
    } > FLASH

    /* EHABI unwind instructions + LSDA (read-only), homed in the flash code window
       right after .text so they never orphan into the vector slot. M33 is EHABI;
       libstdc++ may emit .gcc_except_table even on EHABI, so it is homed too. */
    .ARM.extab :
    {
        *(.ARM.extab* .gnu.linkonce.armextab.*)
    } > FLASH

    .gcc_except_table :
    {
        *(.gcc_except_table .gcc_except_table.*)
    } > FLASH

    /* Global ctors SPLIT: .init_array keeps ONLY the closed KickOS-owned set
       (kernel/arch/chip/lib), run early by Reset_Handler; other (app/lib) ctors
       land in .kickos_app_init_array and run from root_entry with the kernel live. */
    .init_array :
    {
        __init_array_start = .;
        KEEP(*libkickos_kernel.a:*(SORT(.init_array.*) .init_array))
        KEEP(*libkickos_arch_armv7m.a:*(SORT(.init_array.*) .init_array))
        KEEP(*libkickos_chip_rp2350.a:*(SORT(.init_array.*) .init_array))
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
       {dst,len}. */
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
        /* Zero the whole granted window past the loaded .appdata (the pre-.appbss
           alignment gap, .appbss, and the pad) so no stale read-back in the app grant. */
        LONG(__kickos_appdata_load_end); LONG(__kickos_appdata_end - __kickos_appdata_load_end);
#endif
        LONG(ADDR(.bss));    LONG(SIZEOF(.bss));
        __kickos_zero_table_end = .;
    } > FLASH

    _sidata = LOADADDR(.data);

#if KICKOS_HAVE_MPU
    /* Enforcement layout (mirrors mk64f.ld, the other byte-granular backend). Writable
       state is split KERNEL vs app with an INVERTED selector: the CLOSED KickOS-owned
       set (kernel/arch/chip/lib) is captured FIRST into kernel .data/.bss; the appdata
       catch-alls that follow take EVERYTHING ELSE (app objects + toolchain runtime), so
       an unmatched archive lands app-side (reachable), never kernel-side (faults). The
       `archive:member` colon syntax is used (this binutils does not match archive
       members inside EXCLUDE_FILE). PMSAv8 is 32-byte granular with an arbitrary base,
       so the app window needs no pow2 base -- it is simply placed above kernel data. */
    .data : ALIGN(4)
    {
        _sdata = .;
        *libkickos_kernel.a:*(.data .data.*)
        *libkickos_arch_armv7m.a:*(.data .data.*)
        *libkickos_chip_rp2350.a:*(.data .data.*)
        *libkickos_lib.a:*(.data .data.*)
        . = ALIGN(4);
        _edata = .;
    } > RAM AT > FLASH

    .bss (NOLOAD) : ALIGN(4)
    {
        _sbss = .;
        *libkickos_kernel.a:*(.bss .bss.*)
        *libkickos_arch_armv7m.a:*(.bss .bss.*)
        *libkickos_chip_rp2350.a:*(.bss .bss.*)
        *libkickos_lib.a:*(.bss .bss.*)
        *libkickos_kernel.a:*(COMMON)
        *libkickos_arch_armv7m.a:*(COMMON)
        *libkickos_chip_rp2350.a:*(COMMON)
        *libkickos_lib.a:*(COMMON)
        . = ALIGN(4);
        _ebss = .;
    } > RAM

    /* App-granted RW window: all remaining .data/.bss/COMMON (app + runtime). .appdata
       is loaded from flash; .appbss + the pad are zeroed (no stale read-back). */
    .appdata : ALIGN(32)
    {
        __kickos_appdata_start = .;
        *(.data .data.*)
        . = ALIGN(4);
        __kickos_appdata_load_end = .;
    } > RAM AT > FLASH

    /* Pin the base to the window (start + SIZEOF .appdata), NOT the location counter: an
       EMPTY .appdata (an app with no initialized globals) does not carry the section
       ALIGN forward, so a defaulted .appbss would fall back to kernel-.bss-end. */
    .appbss (__kickos_appdata_start + SIZEOF(.appdata)) (NOLOAD) : ALIGN(4)
    {
        *(.bss .bss.* COMMON)
        . = ALIGN(4);
        _appdata_used_end = .;
        . = __kickos_appdata_start + _appdata_size; /* pad the granted window */
        __kickos_appdata_end = .;
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

    . = ALIGN(32);
    __kickos_ram_start = .;
    __kickos_ram_end = _estack - _kernel_stack_size;

    /* App code region for unprivileged threads (RX): the whole flash window. Lets an
       unprivileged thread fetch its code + read flash rodata (XIP). Consumed by the
       (deferred) PMSAv8 backend; kept here so the enforcement seam has the bounds. */
    __kickos_code_start = ORIGIN(FLASH);
    __kickos_code_end = ORIGIN(FLASH) + LENGTH(FLASH);

    /DISCARD/ : { *(.ARM.attributes) }
}

ASSERT(__kickos_ram_start <= __kickos_ram_end,
       "KickOS: static data overflows RAM (no room for the user-RAM pool + stack)")

#if KICKOS_HAVE_MPU
/* App globals must fit the granted window. If this trips, raise KICKOS_APPDATA_SIZE. */
ASSERT(_appdata_used_end <= __kickos_appdata_start + _appdata_size,
       "KickOS: app .data/.bss overflow _appdata_size (raise KICKOS_APPDATA_SIZE)")
/* Lower bound: an empty .appdata dropping its ALIGN would place .appbss below the base. */
ASSERT(ADDR(.appbss) >= __kickos_appdata_start,
       "KickOS: .appbss placed below the app-data window base (empty .appdata alignment-drop regression)")
/* Selector-mismatch guard: the inverted scheme leaves an EMPTY KERNEL side if the
   closed-set archive selector matched nothing (renamed lib?). An empty .appdata is
   legitimate (a minimal app), so the loud signal is an empty kernel .bss. */
ASSERT(_ebss > _sbss,
       "KickOS: kernel .bss empty -- closed-set archive selector matched nothing (renamed lib?)")
#endif

/* The vector table (and thus the initial SP/PC the bootrom reads) must sit at the
   flash image base, and the IMAGE_DEF block must be within the first 4 KiB. */
ASSERT(ADDR(.text) == ORIGIN(FLASH),
       "KickOS: RP2350 vector table must be at the flash image base 0x1000_0000")
ASSERT(g_image_def < ORIGIN(FLASH) + 0x1000,
       "KickOS: RP2350 IMAGE_DEF must be within the bootrom's first-4-KiB scan window")
