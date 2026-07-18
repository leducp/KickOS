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
   own .data/.bss but not the kernel's. SYSMPU is byte/32-granular: the region base
   need only be 32-byte aligned and the size a 32-byte multiple (not pow2). The app
   data is padded up to _appdata_size so the granted window has a fixed size; a real
   app whose globals exceed it trips the ASSERT below. */
#if KICKOS_HAVE_MPU
_appdata_size = 128K;
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

    /* EHABI unwind instructions (read-only). Placed explicitly in the FLASH code
       window so the unprivileged app domain reads them via its RX code grant; left
       orphaned they happen to land here, but the placement is now guaranteed. */
    .ARM.extab :
    {
        *(.ARM.extab* .gnu.linkonce.armextab.*)
    } > FLASH

    /* Global ctors are SPLIT. .init_array keeps ONLY the closed KickOS-owned set
       (kernel/arch/chip/lib): Reset_Handler runs these before kmain, so the kernel
       instance is constructed before kmain uses it. Every OTHER ctor (app, libstdc++/
       libsupc++, KickCAT, newlib) lands in .kickos_app_init_array below and runs from
       root_entry -- in a thread, kernel live -- because a ctor may call a KickOS
       syscall (kos_clock_now), which needs ktime_init + a current thread. Colon
       selectors (not EXCLUDE_FILE) per the .appdata note above. */
    .init_array :
    {
        __init_array_start = .;
        KEEP(*libkickos_kernel.a:*(SORT(.init_array.*) .init_array))
        KEEP(*libkickos_arch_armv7m.a:*(SORT(.init_array.*) .init_array))
        KEEP(*libkickos_chip_mk64f.a:*(SORT(.init_array.*) .init_array))
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
        /* Zero the whole granted window past the loaded .appdata: the pre-.appbss
           alignment gap (forced by an over-aligned .appbss object), .appbss, and the
           pad -- so no stale read-back anywhere in the app grant. */
        LONG(__kickos_appdata_load_end); LONG(__kickos_appdata_end - __kickos_appdata_load_end);
#endif
        LONG(ADDR(.bss));    LONG(SIZEOF(.bss));
        __kickos_zero_table_end = .;
    } > FLASH

    _sidata = LOADADDR(.data);

#if KICKOS_HAVE_MPU
    /* Enforcement layout. Writable state is split into a KERNEL side and an app side.
       Selection is INVERTED so that the app window is fail-safe: the CLOSED
       KickOS-owned set (kernel/arch/chip/lib) is captured FIRST, in script order,
       into the kernel .data/.bss below; the appdata catch-alls that follow then take
       EVERYTHING ELSE -- the app's own objects, the toolchain runtime (libc/libgcc/
       libstdc++/libsupc++), KickCAT, and any unknown NEW archive. An unmatched
       archive therefore lands app-side (reachable, works), never kernel-side (faults).

       The set is selected by the `archive:member` colon syntax, NOT EXCLUDE_FILE:
       this binutils does not match archive members inside EXCLUDE_FILE (a bare
       `*libkickos_kernel.a` there matches nothing -- verified with nm), whereas
       `*libkickos_kernel.a:*(...)` does. A bare `*user*` likewise matched only an
       archive member's basename, missing libkickos_user.a's s_heap and the whole
       toolchain runtime; the colon inversion is path- and name-substring-independent.
       Kernel data sits at the RAM base; the app window is placed above it (SYSMPU is
       32-byte granular with an arbitrary base, so the window needs no pow2 base). */
    .data : ALIGN(4)
    {
        _sdata = .;
        *libkickos_kernel.a:*(.data .data.*)
        *libkickos_arch_armv7m.a:*(.data .data.*)
        *libkickos_chip_mk64f.a:*(.data .data.*)
        *libkickos_lib.a:*(.data .data.*)
        . = ALIGN(4);
        _edata = .;
    } > RAM AT > FLASH

    .bss (NOLOAD) : ALIGN(4)
    {
        _sbss = .;
        *libkickos_kernel.a:*(.bss .bss.*)
        *libkickos_arch_armv7m.a:*(.bss .bss.*)
        *libkickos_chip_mk64f.a:*(.bss .bss.*)
        *libkickos_lib.a:*(.bss .bss.*)
        *libkickos_kernel.a:*(COMMON)
        *libkickos_arch_armv7m.a:*(COMMON)
        *libkickos_chip_mk64f.a:*(COMMON)
        *libkickos_lib.a:*(COMMON)
        . = ALIGN(4);
        _ebss = .;
    } > RAM

    /* App-granted RW window: all remaining .data/.bss/COMMON (app + runtime + KickCAT).
       .appdata is loaded from flash; .appbss + the pad are zeroed (no stale read-back
       through the grant). */
    .appdata : ALIGN(32)
    {
        __kickos_appdata_start = .;
        *(.data .data.*)
        . = ALIGN(4);
        __kickos_appdata_load_end = .;
    } > RAM AT > FLASH

    .appbss (NOLOAD) : ALIGN(4)
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
/* Selector-mismatch guard. The inverted scheme captures the CLOSED KickOS-owned set
   (kernel/arch/chip/lib) into kernel .data/.bss and lets .appdata/.appbss catch the
   rest, so a broken selector shows up as an EMPTY KERNEL side (kernel writable state
   silently leaked into the app window) -- that is the failure to catch, and an empty
   .bss is the loud signal. NOT app-side: an empty .appdata is legitimate (a minimal
   freestanding app whose only app-side state was --gc-sections'd away), so it must
   not assert. The tight invariant (a kernel sentinel below __kickos_appdata_start)
   is a CI nm check. */
ASSERT(_ebss > _sbss,
       "KickOS: kernel .bss empty -- closed-set archive selector matched nothing (renamed lib?)")
#endif
