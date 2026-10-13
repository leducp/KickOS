/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * NOT a C header: the GNU ld body every AArch64 chip script includes. The chip's
 * <chip_layout.h> states KICKOS_LAYOUT_DRAM_BASE and KICKOS_LAYOUT_DRAM_SIZE; an own-image AMP
 * node takes its slice of the partition instead.
 *
 * Loaded with the MMU off at each segment's p_paddr, so `.text.init` links where it loads.
 *
 * The image is split in two. Kernel text and kernel writable state link into the high half
 * TTBR1 translates, with an LMA back in DRAM; the app's text, rodata, data and bss link low,
 * where a per-process TTBR0 root can name them and VMA == LMA.
 *
 * The kernel-side TUs that name the app's half are built -mcmodel=large (kickos_split_image_tu,
 * cmake/kickos.cmake), the halves being 2^40 apart where adrp spans 4 GiB. That leaves the one
 * .got to the app: a GOT slot is reached by adrp, so a GOT with users in both halves could not
 * be placed.
 *
 * TTBR1's level-2 table grants EL0 nothing (boot_tables.inc), so every section placed in KTEXT
 * or RAM is unreachable from EL0.
 */
#ifndef KICKOS_ARCH_ARM64_COMMON_ARMV8A_LD_H
#define KICKOS_ARCH_ARM64_COMMON_ARMV8A_LD_H

#include <sections.ld.h>

/* ENTRY anchors the startup object against --gc-sections; it is in the identity-linked
 * section, so the ELF entry field is a physical address.
 */
ENTRY(_start)

#if KICKOS_AMP_OWN_IMAGE
#include <amp_partition.ld.h>
#define KICKOS_ARM64_DRAM_BASE  KICKOS_AMP_IMAGE_BASE
#define KICKOS_ARM64_DRAM_SIZE  KICKOS_AMP_NODE_SHARE
#else
#include <chip_layout.h>
#define KICKOS_ARM64_DRAM_BASE  KICKOS_LAYOUT_DRAM_BASE
#define KICKOS_ARM64_DRAM_SIZE  KICKOS_LAYOUT_DRAM_SIZE
#endif

/* TCR_EL1.T1SZ is 25 (vmsa.h), which makes the high half the 39-bit range at this base. VA is
 * PA plus this. A multiple of 2^39, so every alignment below is the same either side. */
#define KICKOS_ARM64_VA_BASE    0xFFFFFF8000000000

/* The identity-linked span: one page for the entry code, the EL refusal reporter and the
 * strings it reads, plus the four 4 KiB translation tables of boot_tables.inc. */
#define KICKOS_ARM64_BOOT_SIZE  20K

/* The one level-2 block mapped read-only and executable (boot_tables.inc). Everything fetched
 * fits below this boundary and everything stored to starts at it, on both sides of the split. */
#define KICKOS_ARM64_CODE_BLOCK 2M

/* The kernel's share of the code block; the rest is the app's. */
#define KICKOS_ARM64_KTEXT_SIZE 1M

/* The app's writable window: its .data, .bss and the newlib heap. */
#define KICKOS_ARM64_APP_DATA_SIZE 1M

/* The frame pool the kernel's FrameAllocator describes: page tables and the pages a process
 * holds. */
#define KICKOS_ARM64_FRAME_POOL_SIZE  8M

MEMORY
{
    BOOT    (rx)  : ORIGIN = KICKOS_ARM64_DRAM_BASE,
                    LENGTH = KICKOS_ARM64_BOOT_SIZE
    KTEXT   (rx)  : ORIGIN = KICKOS_ARM64_VA_BASE + KICKOS_ARM64_DRAM_BASE
                             + KICKOS_ARM64_BOOT_SIZE,
                    LENGTH = KICKOS_ARM64_KTEXT_SIZE
    APPTEXT (rx)  : ORIGIN = KICKOS_ARM64_DRAM_BASE + KICKOS_ARM64_BOOT_SIZE
                             + KICKOS_ARM64_KTEXT_SIZE,
                    LENGTH = KICKOS_ARM64_CODE_BLOCK - KICKOS_ARM64_BOOT_SIZE
                             - KICKOS_ARM64_KTEXT_SIZE
    APPDATA (rw)  : ORIGIN = KICKOS_ARM64_DRAM_BASE + KICKOS_ARM64_CODE_BLOCK,
                    LENGTH = KICKOS_ARM64_APP_DATA_SIZE
    RAM     (rwx) : ORIGIN = KICKOS_ARM64_VA_BASE + KICKOS_ARM64_DRAM_BASE
                             + KICKOS_ARM64_CODE_BLOCK + KICKOS_ARM64_APP_DATA_SIZE,
                    LENGTH = KICKOS_ARM64_DRAM_SIZE - KICKOS_ARM64_CODE_BLOCK
                             - KICKOS_ARM64_APP_DATA_SIZE
#if KICKOS_AMP_OWN_IMAGE
    SHARED  (rw)  : ORIGIN = KICKOS_ARM64_VA_BASE + KICKOS_AMP_SHARED_BASE,
                    LENGTH = KICKOS_AMP_SHARED_SIZE
#endif
}

_estack = ORIGIN(RAM) + LENGTH(RAM);
#include <kickos/arch/core_stack.ld.h>
_kernel_stack_size = KICKOS_ARMV8A_CORE_STACK;

/* Read by the startup code to reach a physical address from a high one before the MMU is on. */
__kickos_arm64_va_base = KICKOS_ARM64_VA_BASE;

SECTIONS
{
    /* VMA == LMA: the entry is taken untranslated, and a descriptor names its output by
       physical address. The catch-alls below would take both input sections, so this output
       section stays first in script order. */
    .text.init ORIGIN(BOOT) : ALIGN(8)
    {
        KEEP(*(.text.init))
        . = ALIGN(8);
        KEEP(*(.rodata.init))
        . = ALIGN(4096);
        KEEP(*(.rodata.mmu_ident))
    } > BOOT

    /* The closed kernel set is named here and the app catch-alls below take everything else,
       so an archive nobody listed lands app-side. The `archive:member` colon syntax is
       required: this binutils matches a member basename inside EXCLUDE_FILE, not an archive
       name. */
    .text : AT(ADDR(.text) - KICKOS_ARM64_VA_BASE) ALIGN(8)
    {
        *libkickos_kernel.a:*(.text .text.*)
        *KICKOS_LD_ARCH_LIB:*(.text .text.*)
        *KICKOS_LD_CHIP_LIB:*(.text .text.*)
        KICKOS_CODE_DEBRIS_SECTIONS
        *libkickos_kernel.a:*(.rodata .rodata.*)
        *KICKOS_LD_ARCH_LIB:*(.rodata .rodata.*)
        *KICKOS_LD_CHIP_LIB:*(.rodata .rodata.*)
        . = ALIGN(8);
    } > KTEXT

    /* The selector list is the kernel text list exactly: Reset_Handler walks this array at EL1,
       so a ctor whose text lands app-side would be walked as a privileged fetch of an EL0
       page. Every other ctor lands in .kickos_app_init_array and runs from kickos_root_entry.

       (READONLY) because the input sections carry SHF_WRITE, which would flag the kernel's
       code segment writable as well as executable. */
    .init_array (READONLY) : AT(ADDR(.init_array) - KICKOS_ARM64_VA_BASE) ALIGN(8)
    {
        __init_array_start = .;
        KEEP(*libkickos_kernel.a:*(SORT(.init_array.*) .init_array))
        KEEP(*KICKOS_LD_ARCH_LIB:*(SORT(.init_array.*) .init_array))
        KEEP(*KICKOS_LD_CHIP_LIB:*(SORT(.init_array.*) .init_array))
        __init_array_end = .;
    } > KTEXT

    .kickos_app_fini_array (READONLY) :
        AT(ADDR(.kickos_app_fini_array) - KICKOS_ARM64_VA_BASE) ALIGN(8)
    {
        __kickos_app_fini_array_start = .;
        KEEP(*(SORT(.fini_array.*)))
        KEEP(*(.fini_array))
        __kickos_app_fini_array_end = .;
    } > KTEXT

    /* Read by kernel text, so it stays in the kernel's half. */
    KICKOS_TLS_TEMPLATE(KTEXT)

    /* App-side because kickos_root_entry walks it unprivileged. First in APPTEXT, so the
       window's low bound can name the region origin whether or not it is empty. */
    .kickos_app_init_array (READONLY) : ALIGN(8)
    {
        __kickos_app_init_array_start = .;
        KEEP(*(SORT(.init_array.*)))
        KEEP(*(.init_array))
        __kickos_app_init_array_end = .;
    } > APPTEXT

    /* No AT(): VMA == LMA.

       .apptrap is the EL0 trap leaf (arch/arm64/armv8a/switch.S); `.text.*` would capture it
       kernel-side.

       The .got is the app's alone: AArch64 reaches a weak undefined symbol GOT-indirect
       (R_AARCH64_ADR_GOT_PAGE), and the kernel side emits no GOT reference under
       -mcmodel=large. */
    .apptext : ALIGN(8)
    {
        *(.apptrap)
        *(.text .text.*)
        *(.rodata .rodata.*)
        /* DWARF EH unwind and landing-pad tables for the full-C++ opt-in, app-side because an
           FDE reaches the code it describes with a 32-bit PC-relative field
           (R_AARCH64_PREL32). The LONG(0) is the zero-length CIE terminator crtend would
           supply, and crtend is not linked under -nostartfiles. */
        *(.gcc_except_table .gcc_except_table.*)
        . = ALIGN(8);
        __eh_frame_start = .;
        KEEP(*(.eh_frame))
        LONG(0)
        . = ALIGN(8);
    } > APPTEXT

    /* Every kernel section carries the same VMA-to-LMA delta, so this is _sdata and the copy
       Reset_Handler runs is a self-copy. */
    _sidata = LOADADDR(.data) + KICKOS_ARM64_VA_BASE;

    /* The 2 MiB boundary is the APPDATA origin, not an ALIGN here: with an explicit `> RAM` a
       location-counter assignment moves nothing. */
    .data : AT(ADDR(.data) - KICKOS_ARM64_VA_BASE) ALIGN(8)
    {
        _sdata = .;
        *libkickos_kernel.a:*(.data .data.*)
        *KICKOS_LD_ARCH_LIB:*(.data .data.*)
        *KICKOS_LD_CHIP_LIB:*(.data .data.*)
        . = ALIGN(8);
        _edata = .;
    } > RAM

    .bss (NOLOAD) : AT(ADDR(.bss) - KICKOS_ARM64_VA_BASE) ALIGN(8)
    {
        _sbss = .;
        *libkickos_kernel.a:*(.bss .bss.*)
        *KICKOS_LD_ARCH_LIB:*(.bss .bss.*)
        *KICKOS_LD_CHIP_LIB:*(.bss .bss.*)
        *libkickos_kernel.a:*(COMMON)
        *KICKOS_LD_ARCH_LIB:*(COMMON)
        *KICKOS_LD_CHIP_LIB:*(COMMON)
        . = ALIGN(8);
        _ebss = .;
    } > RAM

    /* Identity-linked like the app's text; Reset_Handler zeroes .appbss off its two symbols. */
    .appdata : ALIGN(8)
    {
        /* Not in .apptext: a writable input section makes the output section writable. */
        *(.got) *(.got.plt)
        __kickos_appdata_load_start = .;
        *(.data .data.*)
        . = ALIGN(8);
        __kickos_appdata_load_end = .;
    } > APPDATA

    .appbss (NOLOAD) : ALIGN(8)
    {
        __kickos_appbss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(8);
        __kickos_appbss_end = .;
    } > APPDATA

    /* App-side: user/src/newlib_sbrk.cc names _kickos_heap_start from app text, which cannot
       adrp to the kernel's half. */
    .userheap (NOLOAD) :
    {
        . = ALIGN(8);
        _kickos_heap_start = .;
        . += KICKOS_USER_HEAP_SIZE;
        _kickos_heap_limit = .;
    } > APPDATA

    /* Page-aligned because a descriptor's output address is. */
    .framepool (NOLOAD) : AT(ADDR(.framepool) - KICKOS_ARM64_VA_BASE) ALIGN(4096)
    {
        __kickos_frame_pool_start = .;
        . += KICKOS_ARM64_FRAME_POOL_SIZE;
        __kickos_frame_pool_end = .;
    } > RAM

    /* The kernel address of output address 0, so the frame allocator answers in the addresses
       a descriptor records. */
    __kickos_frame_pool_delta = KICKOS_ARM64_VA_BASE;

    . = ALIGN(16);
    __kickos_ram_start = .;
    __kickos_ram_end = _estack - _kernel_stack_size;

    /* The extents the user-pointer admission reads (arch_ram_common.cc). The code block is the
       ROM range, refused as an out-pointer, so the sram range starts at the writable boundary.
       __kickos_dram_base is physical, for boot_tables.inc. */
    __kickos_dram_base = KICKOS_ARM64_DRAM_BASE;

    /* The high alias of the DRAM origin, so the boot span is inside the readable range. */
    __kickos_rom_start = KICKOS_ARM64_VA_BASE + KICKOS_ARM64_DRAM_BASE;
    __kickos_rom_end = ADDR(.data);
    __kickos_sram_start = ADDR(.data);

    /* The app's own low window, the second admitted extent: a syscall pointer into app rodata
       or app .data lands here and not in the pair above. */
    __kickos_app_rom_start = ORIGIN(APPTEXT);
    __kickos_app_rom_end = ADDR(.apptext) + SIZEOF(.apptext);
    __kickos_app_sram_start = ADDR(.appdata);
    __kickos_app_sram_end = _kickos_heap_limit;

#if KICKOS_AMP_OWN_IMAGE
    KICKOS_AMP_SHARED_SECTION(SHARED)
#endif

    /DISCARD/ : { *(.comment) *(.note.gnu.build-id) *(.ARM.attributes) }

    KICKOS_NONALLOC_SECTIONS
}

KICKOS_STATIC_RELOC_ASSERT()
KICKOS_TLS_ALIGN_ASSERT()
KICKOS_TLS_FIT_ASSERT()

/* The W^X split, on the load addresses: the two code regions end where the writable side
   begins. */
ASSERT(ORIGIN(APPTEXT) + LENGTH(APPTEXT) == KICKOS_ARM64_DRAM_BASE + KICKOS_ARM64_CODE_BLOCK,
       "KickOS: the two code regions do not end at the 2 MiB boundary the level-2 map splits on")
ASSERT(ADDR(.appdata) - KICKOS_ARM64_DRAM_BASE == KICKOS_ARM64_CODE_BLOCK,
       "KickOS: app .data is not at the 2 MiB boundary the level-2 map splits on; it would be linked into the read-only executable block")
ASSERT(LOADADDR(.data) - KICKOS_ARM64_DRAM_BASE >= KICKOS_ARM64_CODE_BLOCK,
       "KickOS: kernel .data is below the 2 MiB boundary the level-2 map splits on; it would be linked into the read-only executable block")

ASSERT(__kickos_ram_start <= __kickos_ram_end,
       "KickOS: static data overflows RAM (no room for the user-RAM pool + boot stack)")

KICKOS_FINI_EMPTY_ASSERT()

/* An empty kernel .bss means the closed-set selectors matched nothing, kernel writable state
   having been linked into the app's low window; an empty app side is legitimate. */
ASSERT(_ebss > _sbss,
       "KickOS: kernel .bss empty: closed-set archive selector matched nothing (renamed lib?)")

KICKOS_HEAP_MIN_ASSERT()

#if KICKOS_AMP_OWN_IMAGE
/* The bounds are the partition's own constants, not this script's regions: an assert reading
   ORIGIN(SHARED) against LENGTH(SHARED) passes wherever the region is repointed. */
KICKOS_AMP_SHARED_ASSERT(ADDR(.amp_shared) - KICKOS_ARM64_VA_BASE)
KICKOS_AMP_SLICE_ASSERT(ORIGIN(BOOT), _estack - KICKOS_ARM64_VA_BASE,
                        KICKOS_AMP_PARTITION_BASE, KICKOS_AMP_NODE_SHARE)

/* A peer is started at its image base with its MMU off, so the entry must be the base. */
ASSERT(_start == ORIGIN(BOOT),
       "KickOS: this node's entry is not at its own image base, so a peer started at the address the partition derives would fetch something other than _start")

KICKOS_AMP_SHARED_FILLED_ASSERT()
ASSERT(__kickos_amp_chip_end > __kickos_amp_chip_start,
       "KickOS: .amp_shared.chip is empty: the console claim must be shared by every node")
#endif

KICKOS_APP_CODE_WINDOW_NONE()
KICKOS_APP_DATA_WINDOW_NONE()
/* The app is loaded at the virtual addresses it was linked for. */
__kickos_app_load_delta = 0;

#endif
