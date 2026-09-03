/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * Linker script for the i.MX 8M Plus EVK (quad Cortex-A53). QEMU (`-kernel <elf>`) loads each
 * segment at its p_paddr and enters at the ELF entry point with the MMU off. So `.text.init`
 * links where it loads.
 *
 * ENTRY IS AT EL3 ON THIS BOARD and startup.S drops to EL1 before anything here matters; the
 * placement below is the EL1 kernel's.
 *
 * The image is split in two. Kernel text and kernel writable state link into the high half
 * TTBR1 translates, with an LMA back in DRAM; the app's text, rodata, data and bss link LOW,
 * where a per-process TTBR0 root can name them and VMA == LMA.
 *
 * The kernel-side TUs that name the app's half are built -mcmodel=large (kickos_split_image_tu,
 * cmake/kickos.cmake), the halves being 2^40 apart where adrp spans 4 GiB. That leaves the ONE
 * .got a static link has entirely to the app; a GOT slot is itself reached by adrp, so a GOT
 * with users in both halves could not be placed at all.
 *
 * Chip<->arch contract: define __kickos_ram_start/__kickos_ram_end (the arch_ram_alloc pool),
 * _estack (boot stack top), and _sidata/.../_ebss (C runtime). Under KICKOS_HAVE_ASPACE, also
 * __kickos_frame_pool_start/_end and __kickos_frame_pool_delta.
 *
 * Enforcement here is translation, not descriptors: TTBR1's level-2 table grants EL0 nothing
 * (startup.S), so every section placed in KTEXT or RAM below is unreachable from EL0.
 */

#include <sections.ld.h>

/* _start (startup.S): ENTRY is the only thing anchoring the startup object, --gc-sections
 * reaching .text.init through nothing else. It is in the identity-linked section, so the ELF
 * entry field is a physical address.
 */
ENTRY(_start)

/* The part's external DDR window. The EVK populates 6 GiB of it and QEMU's machine defaults
 * to the same; only the first 64 MiB is carved here, so a smaller `-m` than that is what
 * would overflow. */
#define KICKOS_ARM64_DRAM_BASE  0x40000000
#define KICKOS_ARM64_DRAM_SIZE  64M

/* TCR_EL1.T1SZ is 25 (startup.S), which makes the high half the 39-bit range at this
 * base. VA is PA plus this, so the kernel window and the map of physical RAM are one set
 * of descriptors. A multiple of 2^39, so every alignment below is the same either side. */
#define KICKOS_ARM64_VA_BASE    0xFFFFFF8000000000

/* The identity-linked span: one page for the entry code, the EL refusal reporter and the
 * strings it reads, plus the FOUR 4 KiB translation tables the two TTBRs are pointed at. ld
 * refuses an overflow, so a fifth table page has to raise this. */
#define KICKOS_ARM64_BOOT_SIZE  20K

/* The ONE level-2 block startup.S maps read-only and executable. Everything fetched has to fit
 * below this boundary and everything stored to has to start at it, on BOTH sides of the split. */
#define KICKOS_ARM64_CODE_BLOCK 2M

/* The kernel's share of the code block; the rest is the app's, so raising one lowers the
 * other. */
#define KICKOS_ARM64_KTEXT_SIZE 1M

/* The app's writable window: its .data, .bss and the newlib heap. */
#define KICKOS_ARM64_APP_DATA_SIZE 1M

/* The frame pool the kernel's FrameAllocator describes: page tables and the pages a process
 * holds. Its own carve, so the bump allocator and the frame allocator never own the same
 * byte. */
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
}

_estack = ORIGIN(RAM) + LENGTH(RAM);   /* boot/exception stack: top of RAM */
#include <kickos/arch/core_stack.ld.h>
_kernel_stack_size = KICKOS_ARMV8A_CORE_STACK; /* boot/exception stack reservation */

/* startup.S reads this to reach a physical address from a high one, in the window before
 * the MMU is on where an absolute literal cannot be translated. */
__kickos_arm64_va_base = KICKOS_ARM64_VA_BASE;

SECTIONS
{
    /* VMA == LMA: the entry is taken untranslated, and a translation-table descriptor names its
       output by physical address. The catch-alls below would take both input sections, so this
       output section has to stay FIRST in script order to claim them. */
    .text.init ORIGIN(BOOT) : ALIGN(8)
    {
        KEEP(*(.text.init))
        . = ALIGN(8);
        KEEP(*(.rodata.init))
        . = ALIGN(4096);
        KEEP(*(.rodata.mmu_ident))
    } > BOOT

    /* THE SELECTION IS INVERTED: the closed KickOS-owned kernel set is named here, in script
       order, and the app catch-alls further down take everything else, so an archive nobody
       listed lands app-side. The `archive:member` colon syntax is required: this binutils
       matches a member basename inside EXCLUDE_FILE, not an archive name. */
    .text : AT(ADDR(.text) - KICKOS_ARM64_VA_BASE) ALIGN(8)
    {
        *libkickos_kernel.a:*(.text .text.*)
        *libkickos_arch_armv8a.a:*(.text .text.*)
        *libkickos_chip_imx8mp.a:*(.text .text.*)
        KICKOS_CODE_DEBRIS_SECTIONS
        *libkickos_kernel.a:*(.rodata .rodata.*)
        *libkickos_arch_armv8a.a:*(.rodata .rodata.*)
        *libkickos_chip_imx8mp.a:*(.rodata .rodata.*)
        . = ALIGN(8);
    } > KTEXT

    /* Global ctors SPLIT. THE SELECTOR LIST HERE IS THE KERNEL TEXT LIST AND NOTHING ELSE:
       Reset_Handler walks this array at EL1, so an archive listed here whose text the .apptext
       catch-all takes app-side has its ctor walked as a privileged fetch of an EL0 page. Every
       other ctor lands in .kickos_app_init_array and runs from kickos_root_entry.

       (READONLY) because the INPUT sections carry SHF_WRITE, and without the keyword the kernel's
       own code segment is flagged writable as well as executable. */
    .init_array (READONLY) : AT(ADDR(.init_array) - KICKOS_ARM64_VA_BASE) ALIGN(8)
    {
        __init_array_start = .;
        KEEP(*libkickos_kernel.a:*(SORT(.init_array.*) .init_array))
        KEEP(*libkickos_arch_armv8a.a:*(SORT(.init_array.*) .init_array))
        KEEP(*libkickos_chip_imx8mp.a:*(SORT(.init_array.*) .init_array))
        __init_array_end = .;
    } > KTEXT

    /* .fini_array, ASSERTed empty below; nothing walks it. (READONLY) for the reason above. */
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

    /* The app's ctor list, app-side because kickos_root_entry walks it unprivileged. FIRST in
       APPTEXT, so the window's low bound below can name the region origin whether or not this
       section is empty. (READONLY) because the INPUT sections carry SHF_WRITE. */
    .kickos_app_init_array (READONLY) : ALIGN(8)
    {
        __kickos_app_init_array_start = .;
        KEEP(*(SORT(.init_array.*)))
        KEEP(*(.init_array))
        __kickos_app_init_array_end = .;
    } > APPTEXT

    /* The app's text, at the address it will be fetched from. No AT(): VMA == LMA, so a
       per-process root names these bytes by the value the app was linked at. Everything the
       kernel selectors above did not claim lands here.

       .apptrap is the EL0 trap leaf (arch/arm64/armv8a/switch.S). Its own section name because
       the archive that holds it is kernel-side and `.text.*` would capture it up there.

       The .got is the app's alone: AArch64 reaches a WEAK undefined symbol GOT-indirect
       (R_AARCH64_ADR_GOT_PAGE), and the kernel side emits no GOT reference under
       -mcmodel=large. --orphan-handling=error refuses it unplaced. */
    .apptext : ALIGN(8)
    {
        *(.apptrap)
        *(.text .text.*)
        *(.rodata .rodata.*)
        /* DWARF EH unwind + landing-pad tables for the full-C++ opt-in. KEEP so --gc-sections
           cannot drop it, contiguous, with the LONG(0) terminator standing in for crtend's
           __FRAME_END__. App-side: an FDE reaches the code it describes with a 32-bit
           PC-relative field (R_AARCH64_PREL32), so the table sits in the same half as that
           code. */
        *(.gcc_except_table .gcc_except_table.*)
        . = ALIGN(8);
        __eh_frame_start = .;
        KEEP(*(.eh_frame))
        LONG(0)                /* zero-length CIE terminator: crtend is not linked
                                  under -nostartfiles, so the walk needs it here */
        . = ALIGN(8);
    } > APPTEXT

    /* The high alias of the load address: every kernel section carries the same VMA-to-LMA
       delta, so this IS _sdata and the copy Reset_Handler runs is a self-copy. */
    _sidata = LOADADDR(.data) + KICKOS_ARM64_VA_BASE;

    /* The kernel's writable state. The 2 MiB boundary the level-2 map splits on is the APPDATA
       origin, not an ALIGN here: with an explicit `> RAM` the address comes from the region's
       own pointer and a location-counter assignment moves nothing. */
    .data : AT(ADDR(.data) - KICKOS_ARM64_VA_BASE) ALIGN(8)
    {
        _sdata = .;
        *libkickos_kernel.a:*(.data .data.*)
        *libkickos_arch_armv8a.a:*(.data .data.*)
        *libkickos_chip_imx8mp.a:*(.data .data.*)
        . = ALIGN(8);
        _edata = .;
    } > RAM

    .bss (NOLOAD) : AT(ADDR(.bss) - KICKOS_ARM64_VA_BASE) ALIGN(8)
    {
        _sbss = .;
        *libkickos_kernel.a:*(.bss .bss.*)
        *libkickos_arch_armv8a.a:*(.bss .bss.*)
        *libkickos_chip_imx8mp.a:*(.bss .bss.*)
        *libkickos_kernel.a:*(COMMON)
        *libkickos_arch_armv8a.a:*(COMMON)
        *libkickos_chip_imx8mp.a:*(COMMON)
        . = ALIGN(8);
        _ebss = .;
    } > RAM

    /* The app's writable state, identity-linked like its text. VMA == LMA, so nothing is copied
       into place; .appbss is zeroed by Reset_Handler off the two symbols below. */
    .appdata : ALIGN(8)
    {
        /* Here because a writable input section makes the whole output section writable, and
           app text is mapped executable. A static non-PIE link fills them at link time. */
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

    /* The app's: user/src/newlib_sbrk.cc names _kickos_heap_start from app text, which cannot
       adrp to the kernel's half. */
    .userheap (NOLOAD) :
    {
        . = ALIGN(8);
        _kickos_heap_start = .;
        . += KICKOS_USER_HEAP_SIZE;
        _kickos_heap_limit = .;
    } > APPDATA

    /* Frames, page-aligned because a descriptor's output address is. The FrameAllocator writes
       its bitmap into the first frames, so this is real RAM. */
    .framepool (NOLOAD) : AT(ADDR(.framepool) - KICKOS_ARM64_VA_BASE) ALIGN(4096)
    {
        __kickos_frame_pool_start = .;
        . += KICKOS_ARM64_FRAME_POOL_SIZE;
        __kickos_frame_pool_end = .;
    } > RAM

    /* The kernel address corresponding to OUTPUT address 0 for the bytes of that carve, so the
       frame allocator can answer in the output addresses a descriptor records. */
    __kickos_frame_pool_delta = KICKOS_ARM64_VA_BASE;

    /* From here up to _kernel_stack_size below _estack is the arch_ram_alloc pool
       (runtime bump-allocated, so symbols only, no section to reserve). */
    . = ALIGN(16);
    __kickos_ram_start = .;
    __kickos_ram_end = _estack - _kernel_stack_size;

    /* Chip memory extents read by the descriptor-less user-pointer admission
       (arch_ram_common.cc). Symbols only. The code block is the ROM range, which to the
       admission means readable and refused as an out-pointer: the level-2 map marks it
       read-only at both levels. The sram range therefore starts at the writable boundary, not
       at the RAM origin.

       __kickos_dram_base is the DRAM origin for the level-2 map in startup.S, PHYSICAL because
       a descriptor's output address is. */
    __kickos_dram_base = KICKOS_ARM64_DRAM_BASE;

    /* The high alias of the DRAM origin, so the identity-linked boot span is inside the readable
       range and every admission bound is a pointer of the same shape. */
    __kickos_rom_start = KICKOS_ARM64_VA_BASE + KICKOS_ARM64_DRAM_BASE;
    __kickos_rom_end = ADDR(.data);
    __kickos_sram_start = ADDR(.data);

    /* The app's own low window, the second admitted extent: a syscall pointer into app rodata
       or app .data lands here and not in the pair above. Weakly declared arch-side, so a chip
       that carves no such window leaves its pair equal. */
    __kickos_app_rom_start = ORIGIN(APPTEXT);
    __kickos_app_rom_end = ADDR(.apptext) + SIZEOF(.apptext);
    __kickos_app_sram_start = ADDR(.appdata);
    __kickos_app_sram_end = _kickos_heap_limit;

    /DISCARD/ : { *(.comment) *(.note.gnu.build-id) *(.ARM.attributes) }

    KICKOS_NONALLOC_SECTIONS
}

KICKOS_STATIC_RELOC_ASSERT()
KICKOS_TLS_ALIGN_ASSERT()
KICKOS_TLS_FIT_ASSERT()

/* The W^X split, on the LOAD addresses, the level-2 descriptors naming physical output
   addresses. These state that the two code regions together end where the writable side begins,
   which a bad edit of the size constants above would silently break. */
ASSERT(ORIGIN(APPTEXT) + LENGTH(APPTEXT) == KICKOS_ARM64_DRAM_BASE + KICKOS_ARM64_CODE_BLOCK,
       "KickOS: the two code regions do not end at the 2 MiB boundary the level-2 map splits on")
ASSERT(ADDR(.appdata) - KICKOS_ARM64_DRAM_BASE == KICKOS_ARM64_CODE_BLOCK,
       "KickOS: app .data is not at the 2 MiB boundary the level-2 map splits on; it would be linked into the read-only executable block")
ASSERT(LOADADDR(.data) - KICKOS_ARM64_DRAM_BASE >= KICKOS_ARM64_CODE_BLOCK,
       "KickOS: kernel .data is below the 2 MiB boundary the level-2 map splits on; it would be linked into the read-only executable block")

ASSERT(__kickos_ram_start <= __kickos_ram_end,
       "KickOS: static data overflows RAM (no room for the user-RAM pool + boot stack)")

#include <boot_arena.ld.h>
KICKOS_BOOT_ARENA_ASSERT(__kickos_ram_start, __kickos_ram_end)

KICKOS_POOL_ARENA_ASSERT(__kickos_ram_start, __kickos_ram_end)
ASSERT(__kickos_app_fini_array_end == __kickos_app_fini_array_start,
       "KickOS: .fini_array is non-empty: a global with a non-trivial destructor? Static destructors are not supported; nothing would ever run it")

/* Selector-mismatch guard: a broken selector shows up as an EMPTY KERNEL side, kernel writable
   state silently linked into the app's low window. An empty app side is legitimate, so only the
   kernel side is asserted. */
ASSERT(_ebss > _sbss,
       "KickOS: kernel .bss empty: closed-set archive selector matched nothing (renamed lib?)")

/* Opt-in link-time heap floor (undefined by default). */
#ifdef KICKOS_HEAP_MIN
ASSERT(_kickos_heap_limit - _kickos_heap_start >= KICKOS_HEAP_MIN,
       "KickOS: heap below KICKOS_HEAP_MIN")
#endif
