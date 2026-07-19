/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * Linker script for the QEMU `virt` RISC-V board (RV32IMAC, M-mode bare metal) --
 * the runnable rv32imac verification target. QEMU (`-bios none -kernel <elf>`)
 * loads the image into DRAM at 0x8000_0000 and enters it in machine mode, so the
 * whole image links to run from RAM (LMA == VMA; the Reset_Handler .data copy is a
 * no-op). _start (.text.init) must be first at the RAM origin.
 *
 * Chip<->arch contract: define __kickos_ram_start/__kickos_ram_end (the
 * arch_ram_alloc pool), _estack (boot stack top), and _sidata/.../_ebss (C
 * runtime). __global_pointer$ anchors gp-relative small-data access.
 *
 * Under KICKOS_HAVE_MPU the script also carves the two linker-defined regions every
 * UNPRIVILEGED thread needs under PMP enforcement (see arch_domain_static_regions):
 *   __kickos_code_start/_end     -- app code+rodata as ONE pow2 NAPOT region (RX)
 *   __kickos_appdata_start/_end  -- app .data/.bss as ONE pow2 NAPOT region (RW-NX)
 * There is no separate flash on this all-SRAM target, so the code region is carved
 * from the SRAM image at ORIGIN (2^31-aligned, so any pow2 sub-block is naturally
 * NAPOT-aligned). App globals reach .appdata only when user objects are built with
 * -msmall-data-limit=0 (routes them to .data/.bss, not the gp-relative .sdata/.sbss
 * window that lives with the kernel small-data around __global_pointer$).
 */

ENTRY(_start)

MEMORY
{
    RAM (rwx) : ORIGIN = 0x80000000, LENGTH = 8M   /* QEMU virt DRAM (ample) */
}

_estack = ORIGIN(RAM) + LENGTH(RAM);   /* boot/handler stack: top of RAM */
_kernel_stack_size = 16K;              /* boot/trap stack reservation */

/* Pow2 sizes for the two unprivileged NAPOT regions. Both must be a power of two
   (NAPOT); the code block starts at ORIGIN (2^31-aligned) and the app-data block
   at __kickos_code_end (= ORIGIN + _code_size, still pow2-aligned), so each region
   base is naturally aligned to its size. Raise _code_size / _appdata_size to the
   next pow2 if an ASSERT below trips. */
#if KICKOS_HAVE_MPU
_code_size = 64K;
_appdata_size = 4K;
#endif

SECTIONS
{
    .text ORIGIN(RAM) : ALIGN(4)
    {
#if KICKOS_HAVE_MPU
        __kickos_code_start = .;       /* == ORIGIN(RAM): the code NAPOT region base */
#endif
        KEEP(*(.text.init))            /* _start -- must be at the RAM origin */
        *(.text .text.*)
        *(.rodata .rodata.*)
        *(.srodata .srodata.*)
        /* DWARF EH unwind + landing-pad tables for the full-C++ opt-in (a plain
           freestanding app is built -fno-exceptions and emits none of these, so the
           section is empty bar the terminator). RISC-V uses DWARF .eh_frame (unlike
           ARM's linker-defined .ARM.exidx); with -nostartfiles there is no crtbegin
           frame_dummy, so Reset_Handler registers it at boot via a WEAK __register_frame
           (called only in a FULL_CXX link -- see chip_virt.cc). KEEP so --gc-sections
           cannot drop it, contiguous, with the LONG(0) terminator standing in for
           crtend's __FRAME_END__. */
        *(.gcc_except_table .gcc_except_table.*)
        . = ALIGN(4);
        __eh_frame_start = .;
        KEEP(*(.eh_frame))
        LONG(0)                /* zero-length CIE terminator: crtend is not linked
                                  under -nostartfiles, so the walk needs it here */
        . = ALIGN(4);
    } > RAM

    /* Global ctors SPLIT: .init_array keeps ONLY the closed KickOS-owned set
       (kernel/arch/chip/lib), run early by Reset_Handler; other (app/lib) ctors
       land in .kickos_app_init_array and run from root_entry with the kernel live.
       Both are code, so _code_used_end trails the app section (end of all code). */
    .init_array : ALIGN(4)
    {
        __init_array_start = .;
        KEEP(*libkickos_kernel.a:*(SORT(.init_array.*) .init_array))
        KEEP(*libkickos_arch_rv32imac.a:*(SORT(.init_array.*) .init_array))
        KEEP(*libkickos_chip_virt.a:*(SORT(.init_array.*) .init_array))
        KEEP(*libkickos_lib.a:*(SORT(.init_array.*) .init_array))
        __init_array_end = .;
    } > RAM

    .kickos_app_init_array : ALIGN(4)
    {
        __kickos_app_init_array_start = .;
        KEEP(*(SORT(.init_array.*)))
        KEEP(*(.init_array))
        __kickos_app_init_array_end = .;
#if KICKOS_HAVE_MPU
        . = ALIGN(4);
        _code_used_end = .;            /* end of real code+rodata (checked below) */
#endif
    } > RAM

#if KICKOS_HAVE_MPU
    /* The code NAPOT region spans [ORIGIN, ORIGIN + _code_size). The end is a symbol
       (not a section pad) so the image carries no 64K of zero fill. The gap between
       _code_used_end and __kickos_code_end is unused SRAM covered by the RX grant --
       harmless (an unprivileged thread never branches there). */
    __kickos_code_end = __kickos_code_start + _code_size;

    _sidata = LOADADDR(.data);          /* == VMA here (RAM image), so the copy is a no-op */

    /* Enforcement layout (mirrors mk64f.ld). Writable state SPLIT kernel-side vs app-
       side with an INVERTED selection: the CLOSED KickOS-owned set (kernel/arch/chip/
       lib) is captured FIRST via `archive:member` colon syntax into the kernel .data/
       .bss right after the code region; the .appdata/.appbss catch-alls take EVERYTHING
       ELSE. CRITICAL RISC-V caveat: ONLY .data/.bss/COMMON invert -- the gp-relative
       small-data (.sdata/.sdata2/.sbss, incl. prebuilt libc/libgcc) MUST stay CATCH-ALL
       in the single __global_pointer$ window here, or gp-relative addressing breaks. App
       globals reach .appdata only under -msmall-data-limit=0 (routes them to .data/.bss,
       not the gp window -- see the header note). Kernel .data is pinned at __kickos_code_end;
       the app window is a pow2 NAPOT region aligned above the kernel data by a top-level
       ALIGN (kernel .data/.bss are non-empty and advance the region pointer, so no
       explicit-VMA empty-section fold-back). */
    .data __kickos_code_end : ALIGN(8)
    {
        _sdata = .;
        *libkickos_kernel.a:*(.data .data.*)
        *libkickos_arch_rv32imac.a:*(.data .data.*)
        *libkickos_chip_virt.a:*(.data .data.*)
        *libkickos_lib.a:*(.data .data.*)
        . = ALIGN(8);
        PROVIDE(__global_pointer$ = . + 0x800);  /* gp: middle of the small-data window */
        *(.sdata .sdata.* .sdata2.*)             /* catch-all: all gp small-data stays here */
        . = ALIGN(4);
        _edata = .;
    } > RAM AT > RAM

    .bss (NOLOAD) : ALIGN(4)
    {
        _sbss = .;
        *(.sbss .sbss.*)                         /* catch-all: all gp small-bss stays here */
        __kernel_bss_start = .;
        *libkickos_kernel.a:*(.bss .bss.*)
        *libkickos_arch_rv32imac.a:*(.bss .bss.*)
        *libkickos_chip_virt.a:*(.bss .bss.*)
        *libkickos_lib.a:*(.bss .bss.*)
        *libkickos_kernel.a:*(COMMON)
        *libkickos_arch_rv32imac.a:*(COMMON)
        *libkickos_chip_virt.a:*(COMMON)
        *libkickos_lib.a:*(COMMON)
        __kernel_bss_end = .;
        . = ALIGN(4);
        _ebss = .;
    } > RAM

    /* App-owned globals as a pow2 NAPOT region above the kernel data. The top-level
       ALIGN puts the base at a natural pow2 boundary (NAPOT). .appbss is pinned right
       after .appdata via SIZEOF, so __kickos_appbss_start == the .appdata load end and
       the Reset_Handler's [__kickos_appbss_start, __kickos_appdata_end) zero covers the
       whole grant with no external gap; .appdata rides LMA == VMA (loaded in place). The
       window base is aligned to _appdata_size by the section-level ALIGN (a top-level
       `. = ALIGN()` is swallowed by the region pointer under `> RAM`). */
    .appdata : ALIGN(_appdata_size)
    {
        __kickos_appdata_start = .;
        *(.data .data.*)
        . = ALIGN(4);
    } > RAM AT > RAM

    .appbss (__kickos_appdata_start + SIZEOF(.appdata)) (NOLOAD) : ALIGN(4)
    {
        __kickos_appbss_start = .;
        *(.bss .bss.* COMMON)
        . = ALIGN(4);
        _appdata_used_end = .;
        _appdata_fits = ASSERT(_appdata_used_end <= __kickos_appdata_start + _appdata_size,
               "KickOS: app .data/.bss overflow _appdata_size (raise it to the next pow2)");
        . = __kickos_appdata_start + _appdata_size; /* pad the block up to pow2 */
        __kickos_appdata_end = .;
    } > RAM
#else
    _sidata = LOADADDR(.data);          /* == VMA here (RAM image), so the copy is a no-op */

    .data : ALIGN(8)
    {
        _sdata = .;
        *(.data .data.*)
        . = ALIGN(8);
        PROVIDE(__global_pointer$ = . + 0x800);  /* gp: middle of the small-data window */
        *(.sdata .sdata.* .sdata2.*)
        . = ALIGN(4);
        _edata = .;
    } > RAM AT > RAM

    .bss (NOLOAD) : ALIGN(4)
    {
        _sbss = .;
        *(.sbss .sbss.*)
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(4);
        _ebss = .;
    } > RAM
#endif

    /* From here up to _kernel_stack_size below _estack is the arch_ram_alloc pool
       (runtime bump-allocated -- just symbols, no section to reserve). */
    . = ALIGN(16);
    __kickos_ram_start = .;
    __kickos_ram_end = _estack - _kernel_stack_size;

    /DISCARD/ : { *(.comment) *(.riscv.attributes) }
}

ASSERT(__kickos_ram_start <= __kickos_ram_end,
       "KickOS: static data overflows RAM (no room for the user-RAM pool + boot stack)")

#if KICKOS_HAVE_MPU
/* Real code+rodata must fit the pow2 code region (raise _code_size to the next pow2
   if this trips). */
ASSERT(_code_used_end <= __kickos_code_end,
       "KickOS: app code overflows _code_size (raise it to the next pow2)")
/* App globals must fit the pow2 app-data region (raise _appdata_size to the next pow2
   if this trips). */
ASSERT(_appdata_used_end <= __kickos_appdata_start + _appdata_size,
       "KickOS: app .data/.bss overflow _appdata_size (raise it to the next pow2)")
/* Selector-mismatch guard for the inverted closed-set .data/.bss capture. Only
   .data/.bss/COMMON invert; .sdata/.sbss stay catch-all in the gp window, so the whole
   kernel .bss (_sbss.._ebss) is never empty and _ebss > _sbss would NOT detect a dead
   colon selector. Bracket the closed-set portion instead: libkickos_kernel.a always
   contributes .bss, so an empty bracket means the colon selectors matched nothing (a
   renamed lib), leaking kernel writable state into the app window. */
ASSERT(__kernel_bss_end > __kernel_bss_start,
       "KickOS: kernel .bss empty -- closed-set archive selector matched nothing (renamed lib?)")
#endif
