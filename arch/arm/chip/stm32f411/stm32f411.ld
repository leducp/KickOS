/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * Linker script for the STM32F411VE (STM32F411E-DISCO), Cortex-M4F.
 *   Flash : 512 KiB @ 0x0800_0000  (aliased to 0x0 at boot; vectors live here)
 *   SRAM  : 128 KiB @ 0x2000_0000
 */

ENTRY(Reset_Handler)

MEMORY
{
    FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 512K
    RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 128K
}

_estack = ORIGIN(RAM) + LENGTH(RAM);
_kernel_stack_size = 8K;

/* App-owned globals get their own PMSA region so an unprivileged thread reaches its
   own .data/.bss but not the kernel's. PMSA needs the region pow2-sized AND aligned
   to its size, so the app window is padded to this pow2 and section-aligned to its
   size above the kernel data; an app whose globals exceed it trips the ASSERT.
   Power of two only. */
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
        KEEP(*libkickos_chip_stm32f411.a:*(SORT(.init_array.*) .init_array))
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
    /* Enforcement layout (mirrors mk64f.ld). Writable state is SPLIT kernel-side vs
       app-side with an INVERTED selection: the CLOSED KickOS-owned set (kernel/arch/
       chip/lib) is captured FIRST, in script order, via the `archive:member` colon
       syntax into the kernel .data/.bss at the RAM base; the .appdata/.appbss catch-alls
       that follow then take EVERYTHING ELSE -- the app's own objects, the toolchain
       runtime (libc/libgcc/libstdc++/libsupc++), KickCAT, libkickos_user.a's heap, and
       any unknown NEW archive. An unmatched archive lands app-side (reachable), never
       kernel-side (faults). Colon syntax, NOT EXCLUDE_FILE: this binutils does not match
       archive members inside EXCLUDE_FILE, and a bare `*user*` missed the toolchain
       runtime + the heap arena. PMSA needs the app window pow2-sized AND aligned to its
       size: the kernel data sits at the RAM base and a section-level
       ALIGN(_appdata_size) on .appdata aligns the window base above it (a top-level
       `. = ALIGN()` is swallowed by the region pointer under `> RAM`). */
    .data : ALIGN(4)
    {
        _sdata = .;
        *libkickos_kernel.a:*(.data .data.*)
        *libkickos_arch_armv7m.a:*(.data .data.*)
        *libkickos_chip_stm32f411.a:*(.data .data.*)
        *libkickos_lib.a:*(.data .data.*)
        . = ALIGN(4);
        _edata = .;
    } > RAM AT > FLASH

    .bss (NOLOAD) : ALIGN(4)
    {
        _sbss = .;
        *libkickos_kernel.a:*(.bss .bss.*)
        *libkickos_arch_armv7m.a:*(.bss .bss.*)
        *libkickos_chip_stm32f411.a:*(.bss .bss.*)
        *libkickos_lib.a:*(.bss .bss.*)
        *libkickos_kernel.a:*(COMMON)
        *libkickos_arch_armv7m.a:*(COMMON)
        *libkickos_chip_stm32f411.a:*(COMMON)
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
       counter: an EMPTY .appdata (an app with no initialized globals -- the bench) does
       not carry its ALIGN(_appdata_size) forward, so a defaulted .appbss falls back to
       kernel-.bss-end, BELOW the granted window, and every app-global access faults. */
    .appbss (__kickos_appdata_start + SIZEOF(.appdata)) (NOLOAD) : ALIGN(4)
    {
        *(.bss .bss.* COMMON)
        . = ALIGN(4);
        _appdata_used_end = .;
        _appdata_fits = ASSERT(_appdata_used_end <= __kickos_appdata_start + _appdata_size,
               "KickOS: app .data/.bss overflow _appdata_size (raise it to the next pow2)");
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

    /* App code region for unprivileged threads (RX): the whole flash, a pow2 PMSA
       region (512 KiB @ 0x0800_0000, base aligned to its size). Lets an unprivileged
       thread fetch its code + read flash rodata. Kernel code is also unprivileged-
       readable (confidentiality, not escalation) -- a tighter app-only .text region
       is a later refinement, as on the other PMSA chips. */
    __kickos_code_start = ORIGIN(FLASH);
    __kickos_code_end = ORIGIN(FLASH) + LENGTH(FLASH);

    /DISCARD/ : { *(.ARM.attributes) }
}

ASSERT(__kickos_ram_start <= __kickos_ram_end,
       "KickOS: static data overflows RAM (no room for the user-RAM pool + stack)")

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
   loud signal the colon selectors matched nothing (a renamed lib). NOT app-side: an
   empty .appdata is legitimate (a minimal freestanding app --gc-sections'd bare). */
ASSERT(_ebss > _sbss,
       "KickOS: kernel .bss empty -- closed-set archive selector matched nothing (renamed lib?)")
#endif
