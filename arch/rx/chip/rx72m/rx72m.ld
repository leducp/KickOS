/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * Linker script for the Renesas RX72M (RX72M Group, RXv3 core).
 * (RX72M Group User's Manual: Hardware, r01uh0804ej0120, Rev.1.20.)
 *
 *   Code flash : 4 MiB  @ 0xFFC0_0000 .. 0xFFFF_FFFF (UM memory map)
 *   SRAM       : 512 KiB @ 0x0000_0000 (main on-chip RAM; expansion RAM omitted)
 *   OFSM       : option-setting memory @ 0xFE7F_5D00 (MDE/OFS0/OFS1); data in
 *                the image, not code (UM sec.7). Determines endian + WDT/LVD state.
 *
 * Two vector tables (UM sec.2.6):
 *   - Fixed exception table at 0xFFFF_FF80 (EXTB reset value). Its last word,
 *     0xFFFF_FFFC, is the RESET vector (fetched by hardware at power-on).
 *   - Relocatable interrupt table (INTB), 256 words in ROM; startup sets INTB.
 *
 * RX section names: GCC emits .text/.rodata/.data/.bss for KickOS TUs, but the
 * GNU RX libc/libgcc use the RX-native short names P (code), C/C_1/C_2/W/L
 * (const), D/D_1/D_2 (data), B/B_1/B_2 (bss); all matched below.
 *
 * RX symbol names: the RX psABI user-label-prefix is '_', so a C `extern _sidata`
 * resolves to `__sidata` and `extern __kickos_ram_start` to `___kickos_ram_start`.
 * The link-time symbols below carry that extra leading underscore to match; asm
 * symbols (e.g. _istack) are used verbatim and take no extra prefix.
 */

#include <chip_layout.h>
#include "rx72m_image.ld.h"
#include <kernel_data_reserve.ld.h>

ENTRY(_start)

MEMORY
{
    KICKOS_LINK_RAM
    KICKOS_LINK_OFSM
    KICKOS_LINK_ROM
    KICKOS_LINK_FVECT
}

_istack = ORIGIN(RAM) + LENGTH(RAM);   /* boot/interrupt stack (ISP): top of SRAM */
_kernel_stack_size = 8K;               /* ISP (boot/ISR) stack reservation */

/* Kernel .data/.bss below the images' windows (kernel_data_reserve.ld.h). The kernel's
   .data/.bss passes 32K by about a kilobyte: the per-thread kernel stacks and the kernel
   singleton. */
#if KICKOS_HAVE_MPU
#ifndef KICKOS_KERNEL_DATA_RESERVE
#define KICKOS_KERNEL_DATA_RESERVE 64K
#endif
KICKOS_KERNEL_DATA_RESERVE_DECL(ORIGIN(RAM))
#endif

SECTIONS
{
    /* Option-setting memory: little-endian + WDT/LVD off (all-ones defaults;
       UM sec.7.2.5 MDE little-endian = 0xFFFFFFFF, sec.7.2.3/4 OFS0/OFS1). */
    .ofsm : { KEEP(*(.ofsm)) } > OFSM

    .rvectors : ALIGN(4)
    {
        KEEP(*(.rvectors))     /* 256-word relocatable interrupt table (INTB) */
    } > ROM

    .text : ALIGN(4)
    {
        KICKOS_KERNEL_SECTIONS(.text .text.*)
        KICKOS_CODE_DEBRIS_SECTIONS
        KICKOS_KERNEL_SECTIONS(P P_*)          /* RX-native code (libc/libgcc) */
        KICKOS_KERNEL_SECTIONS(.rodata .rodata.*)
        KICKOS_KERNEL_SECTIONS(C C_1 C_2 C$*)  /* RX-native const */
        KICKOS_KERNEL_SECTIONS(W W_1 W_2)      /* RX-native const (16/8-bit addressed) */
        KICKOS_KERNEL_SECTIONS(L)              /* literal pool */
        KEEP(*(.init))
        KEEP(*(.fini))
        . = ALIGN(4);
    } > ROM

    /* SjLj exception landing-pad tables (read-only). The RX toolchain uses
       setjmp/longjmp EH, so unwinding reads .gcc_except_table, not DWARF. Placed
       explicitly in ROM: left orphaned it falls into the fixed 0x80-byte FVECT and
       overflows it. The kernel's here, each image's in its own code span. */
    .gcc_except_table : ALIGN(4)
    {
        KICKOS_KERNEL_SECTIONS(.gcc_except_table .gcc_except_table.*)
        . = ALIGN(4);
    } > ROM

    /* Global ctors SPLIT: .init_array keeps ONLY the closed KickOS-owned set
       (kernel/arch/chip/lib), run early by Reset_Handler; other (app/lib) ctors
       land in each image's constructor window and run from its startup with the kernel live.
       RX prepends one underscore, so the C `__kickos_app_init_array_*` in kmain
       resolves to the triple-underscore linker symbols below. */
    .init_array : ALIGN(4)
    {
        ___init_array_start = .;
        KEEP(*libkickos_kernel.a:*(SORT(.init_array.*) .init_array))
        KEEP(*libkickos_arch_rxv3.a:*(SORT(.init_array.*) .init_array))
        KEEP(*libkickos_chip_rx72m.a:*(SORT(.init_array.*) .init_array))
        KEEP(*libkickos_lib.a:*(SORT(.init_array.*) .init_array))
        ___init_array_end = .;
    } > ROM

    /* .fini_array, app side, ASSERTed empty below; nothing walks it. */
    .kickos_app_fini_array : ALIGN(4)
    {
        ___kickos_app_fini_array_start = .;
        KEEP(*(SORT(.fini_array.*)))
        KEEP(*(.fini_array))
        ___kickos_app_fini_array_end = .;
    } > ROM

    KICKOS_INIT_TABLES(ROM)

    __sidata = LOADADDR(.data);

#if KICKOS_HAVE_MPU
    /* Enforcement layout. Writable state SPLIT kernel-side vs app-side with an INVERTED
       selection: the kernel's inputs are captured FIRST into the kernel .data/.bss at the RAM
       base; .appdata/.appbss take the app image's, .initdata/.initbss the init's where it is
       an image of its own. Both sides must carry BOTH the GCC .data/.bss AND
       the RX-native D/B names, or a KickOS TU that emits an RX-native section escapes the
       kernel side. The RX MPU takes arbitrary 16-byte bounds, but the shared linker path
       shapes the app window as a pow2 aligned above the kernel data by a top-level ALIGN. */
    .data : ALIGN(4)
    {
        __sdata = .;
        KICKOS_KERNEL_SECTIONS(.data .data.* D D_1 D_2)
        . = ALIGN(4);
        __edata = .;
    } > RAM AT > ROM

    .bss (NOLOAD) : ALIGN(4)
    {
        __sbss = .;
        KICKOS_KERNEL_SECTIONS(.bss .bss.* B B_1 B_2)
        KICKOS_KERNEL_SECTIONS(COMMON)
        . = ALIGN(4);
        __ebss = .;
    } > RAM
#else
    .data : ALIGN(4)
    {
        __sdata = .;
        KICKOS_KERNEL_SECTIONS(.data .data.* D D_1 D_2)
        . = ALIGN(4);
        __edata = .;
    } > RAM AT > ROM

    .bss (NOLOAD) : ALIGN(4)
    {
        __sbss = .;
        KICKOS_KERNEL_SECTIONS(.bss .bss.* B B_1 B_2)
        KICKOS_KERNEL_SECTIONS(COMMON)
        . = ALIGN(4);
        __ebss = .;
    } > RAM
#endif

    KICKOS_IMAGE_TABLE(ROM, LONG)
    ___kickos_kernel_code_end = .;

    /* The thread-local template of every image, then every image's code span and every image's
       window, which tools/image-link.py writes per link (KICKOS_RX_IMAGE_CODE, KICKOS_RX_IMAGE),
       image 1's named as the app's below. */
    KICKOS_TLS_TEMPLATE(ROM)

    /* The RX psABI prefixes a C identifier with an extra leading underscore, so C's
       __kickos_tdata_start is asm ___kickos_tdata_start. The shared template spells the
       asm names every other backend needs; these four are what the RX compiler emits. */
    ___kickos_tdata_start = __kickos_tdata_start;
    ___kickos_tdata_end = __kickos_tdata_end;
    ___kickos_tbss_start = __kickos_tbss_start;
    ___kickos_tbss_end = __kickos_tbss_end;

    INCLUDE kickos_images_code.ld
    INCLUDE kickos_images.ld
    ___kickos_app_init_array_start = KICKOS_LD_IMAGE_SYM(1, ctor_start);
    ___kickos_app_init_array_end = KICKOS_LD_IMAGE_SYM(1, ctor_end);
#if KICKOS_HAVE_MPU
    ___kickos_appdata_start = KICKOS_LD_IMAGE_SYM(1, window_start);
    ___kickos_appdata_end = KICKOS_LD_IMAGE_SYM(1, window_end);
#endif
    _appdata_used_end = KICKOS_LD_IMAGE_SYM(1, bss_end);
    __kickos_heap_start = KICKOS_LD_IMAGE_SYM(1, heap_start);
    __kickos_heap_limit = KICKOS_LD_IMAGE_SYM(1, heap_end);
    ___kickos_emutls_v_start = KICKOS_LD_IMAGE_SYM(1, emutls_start);
    ___kickos_emutls_v_end = KICKOS_LD_IMAGE_SYM(1, emutls_end);
    KICKOS_IMAGE0_TLS()
    KICKOS_IMAGE0_NAMES()

    . = ALIGN(32);
    ___kickos_ram_start = .;
    ___kickos_ram_end = _istack - _kernel_stack_size;

    /* Chip memory extents read by the non-enforcing user-pointer admission
       (arch_ram_common.cc). Symbols only, no layout effect. ROM and FVECT are
       contiguous, so one range spans both. */
    ___kickos_rom_start = ORIGIN(ROM);
    ___kickos_rom_end = ORIGIN(FVECT) + LENGTH(FVECT);
    ___kickos_sram_start = ORIGIN(RAM);

    /* App code region for unprivileged threads (RX + read): the code flash. The RX
       MPU takes arbitrary 16-byte-page bounds, so this need not be pow2. Lets an
       unprivileged thread fetch its code + read flash rodata. END = ORIGIN+LENGTH
       (0xFFFFFF80, the start of the fixed vectors) and NOT 0xFFC00000+4M: the
       latter is 0x1_0000_0000, which wraps to 0 in a 32-bit uintptr_t and would
       make domain.cc's `end > start` false, silently dropping the code region and
       faulting the thread on its first instruction fetch. The fixed vectors are
       never read by user code, so excluding the top 0x80 is harmless. */
    ___kickos_code_start = ORIGIN(ROM);
    ___kickos_code_end = ORIGIN(ROM) + LENGTH(ROM);
    KICKOS_IMAGE_BOUNDS(ORIGIN(ROM), ORIGIN(ROM) + LENGTH(ROM),
                        ORIGIN(ROM), ___kickos_kernel_code_end,
                        ORIGIN(RAM), ORIGIN(RAM) + LENGTH(RAM),
                        __sdata, __ebss, ___kickos_ram_end, _istack, 0)

    /* Fixed exception vector table incl the reset vector at 0xFFFFFFFC. */
    .fvectors : { KEEP(*(.fvectors)) } > FVECT

    /* rx-elf ld's relocatable script gives an image's own link an empty .stack. */
    /DISCARD/ : { *(.ARM.attributes) *(.comment) *(.stack) }

    KICKOS_NONALLOC_SECTIONS
}

KICKOS_STATIC_RELOC_ASSERT()
KICKOS_TLS_ALIGN_ASSERT()
KICKOS_TLS_FIT_ASSERT()
KICKOS_TLS_EMUTLS_ASSERT(___kickos_emutls_v_start, ___kickos_emutls_v_end)

ASSERT(___kickos_ram_start <= ___kickos_ram_end,
       "KickOS: static data overflows RAM (no room for the user-RAM pool + ISP stack)")

#include <boot_arena.ld.h>
KICKOS_BOOT_ARENA_ASSERT(___kickos_ram_start, ___kickos_ram_end)

KICKOS_POOL_ARENA_ASSERT(___kickos_ram_start, ___kickos_ram_end)
KICKOS_FINI_EMPTY_ASSERT()

#if KICKOS_HAVE_MPU
/* Selector-mismatch guard. The inverted scheme captures the CLOSED KickOS-owned set
   into kernel .data/.bss and lets .appdata/.appbss catch the rest, so a broken selector
   shows up as an EMPTY KERNEL side (kernel writable state silently leaked into the app
   window). libkickos_kernel.a always contributes .bss, so an empty kernel .bss is the
   loud signal the colon selectors matched nothing (a renamed lib). NOT app-side: an
   empty .appdata is legitimate (a minimal freestanding app --gc-sections'd bare). */
ASSERT(__ebss > __sbss,
       "KickOS: kernel .bss empty: closed-set archive selector matched nothing (renamed lib?)")
#endif
INCLUDE kickos_images_asserts.ld

KICKOS_HEAP_MIN_ASSERT()

/* The app windows this script does not carve, stated EMPTY rather than left absent:
   the kernel's references are strong, so an omitted bound is a link error. */
#if !KICKOS_HAVE_MPU
KICKOS_APP_DATA_WINDOW_NONE()
#endif
KICKOS_APP_IMAGE_SPLIT_NONE()
