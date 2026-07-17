/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * Linker script for the NXP MK64FN1M0VLL12 (FRDM-K64F), Cortex-M4F.
 * (K64 Sub-Family Reference Manual K64P144M120SF5RM.)
 *
 *   Flash : 1 MiB  @ 0x0000_0000
 *   SRAM  : 256 KiB, contiguous: SRAM_L 64 KiB @ 0x1FFF_0000 + SRAM_U 192 KiB
 *           @ 0x2000_0000  ->  0x1FFF_0000 .. 0x2003_0000
 *
 * CRITICAL: the 16-byte Flash Configuration Field at 0x0000_0400 (FSEC/FOPT/
 * FPROT) is loaded at reset. A wrong/zeroed FSEC secures or mass-erase-disables
 * the part (bricks debug access), so .fcf is placed here explicitly and KEPT.
 */

ENTRY(Reset_Handler)

/* SRAM is two banks joined at 0x2000_0000 (SRAM_L below, SRAM_U above). Aligned
 * CPU word accesses cross it fine; unaligned/burst accesses across the seam are
 * unsupported (RM 3.5.3.1) -- see -mno-unaligned-access in the toolchain, and
 * split this region if a DMA/burst buffer ever needs to straddle 0x2000_0000. */
MEMORY
{
    FLASH (rx)  : ORIGIN = 0x00000000, LENGTH = 1024K
    RAM   (rwx) : ORIGIN = 0x1FFF0000, LENGTH = 256K
}

_estack = ORIGIN(RAM) + LENGTH(RAM);   /* initial MSP: top of SRAM_U */
_kernel_stack_size = 8K;               /* boot/handler (MSP) stack */

/* App-owned globals get their own MPU region so an unprivileged thread reaches its
   own .data/.bss but not the kernel's. The region MUST be pow2-sized + pow2-aligned
   for the PMSA/NAPOT backends (SYSMPU is byte-granular but shares the block), so the
   app data is padded up to this pow2 size; a real app whose globals exceed it trips
   the ASSERT below. Keep it a power of two. */
#if KICKOS_HAVE_MPU
_appdata_size = 4K;
#endif

SECTIONS
{
    .isr_vector ORIGIN(FLASH) :
    {
        KEEP(*(.isr_vector))
    } > FLASH

    /* Flash Configuration Field at a fixed 0x400; the vector table (102 words =
       0x198) fits below it, code follows above 0x410. */
    .fcf 0x00000400 :
    {
        KEEP(*(.fcf))
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

    .init_array :
    {
        __init_array_start = .;
        KEEP(*(SORT(.init_array.*)))
        KEEP(*(.init_array))
        __init_array_end = .;
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

    /* App-owned globals as a pow2 MPU region at the base of RAM (ORIGIN is already
       64 KiB-aligned). Grouped by path (objects under a user/ dir = the app object +
       user runtime); placed BEFORE the kernel .data/.bss so those input sections are
       consumed here and the kernel catch-all below gets only kernel/arch/lib.
       .appdata is loaded from flash; .appbss + the pad are zeroed (no stale
       read-back through the grant). */
#if KICKOS_HAVE_MPU
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

    /* App code region for unprivileged threads (RX): the whole flash. SYSMPU is
       byte/32-granular (no pow2 rule), so this is start=0/end=1M-1. Lets an
       unprivileged thread fetch its code + read flash rodata via the core's code
       bus (crossbar master 0). Cleanly disjoint from the RAM arena/stacks. */
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
