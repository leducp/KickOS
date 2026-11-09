/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * Linker script for the QEMU `virt` RISC-V board (RV64IMAC bare metal).
 * QEMU (`-bios none -kernel <elf>`) loads each PT_LOAD segment at its p_paddr and enters at
 * the LOWEST of them rather than at the ELF entry field, so .text.init must stay first in
 * .text and .text must keep the lowest load address in the image.
 *
 * The image is split in two. Kernel text and kernel writable state link into the high range
 * the top gigabyte covers, with an LMA back in DRAM; the app links at KICKOS_RV64_APP_VA_BASE,
 * one level-2 slot the boot root leaves empty. Image 0 links low and loads high over one
 * uniform delta, KICKOS_RV64_APP_LOAD_DELTA.
 *
 * Two forms, by KICKOS_IMAGE_PROGRAM: the program form links an image on its own into APPTEXT
 * and APPDATA; the kernel form links the kernel and places the loadable bytes tools/image-link.py
 * hands it, image 0's where the image was linked (<kickos_image0.ld>) and every later image's in
 * .kickos_images.
 *
 * A CROSS-HALF REFERENCE CAN LINK SILENTLY. What catches one is
 * tests/static/check_riscv_kernel_apphalf.sh over the kernel archives' relocations; the link
 * is --no-relax-gp (CMakeLists.txt, kickos_kernel_leaf), so none resolves through gp.
 *
 * The closed KickOS-owned kernel set is named first in script order, then the rest of the
 * kernel's link.
 *
 * Chip<->arch contract: define __kickos_ram_start/__kickos_ram_end (the arch_ram_alloc pool),
 * _estack (boot stack top), and _sidata/.../_ebss (C runtime). __global_pointer$ is the APP's.
 * Under KICKOS_HAVE_ASPACE, also __kickos_frame_pool_start/_end and __kickos_frame_pool_delta.
 */

#include <sections.ld.h>
#include "boot_layout.ld.h"
#include <kickos/arch/rv64_trap_stack.h>

#define KICKOS_RV64_APP_WINDOW (KICKOS_RV64_APPTEXT_SIZE + KICKOS_RV64_APPDATA_SIZE)

MEMORY
{
    KTEXT   (rx)  : ORIGIN = KICKOS_RV64_VA_BASE + KICKOS_RV64_DRAM_BASE,
                    LENGTH = KICKOS_RV64_KTEXT_SIZE
    APPTEXT (rx)  : ORIGIN = KICKOS_RV64_APP_VA_BASE,
                    LENGTH = KICKOS_RV64_APPTEXT_SIZE
    APPDATA (rw)  : ORIGIN = KICKOS_RV64_APP_VA_BASE + KICKOS_RV64_APPTEXT_SIZE,
                    LENGTH = KICKOS_RV64_APPDATA_SIZE
    RAM     (rwx) : ORIGIN = KICKOS_RV64_VA_BASE + KICKOS_RV64_DRAM_BASE
                             + KICKOS_RV64_KTEXT_SIZE + KICKOS_RV64_APP_WINDOW,
                    LENGTH = KICKOS_RV64_DRAM_SIZE - KICKOS_RV64_KTEXT_SIZE
                             - KICKOS_RV64_APP_WINDOW
}


#if KICKOS_IMAGE_PROGRAM

ENTRY(kickos_root_entry)

SECTIONS
{
    /* The header FIRST in APPTEXT, so the window's low bound below names the region origin. */
    .kickos_image0_header ORIGIN(APPTEXT) :
        AT(ADDR(.kickos_image0_header) + KICKOS_RV64_APP_LOAD_DELTA) ALIGN(8)
    {
        __kickos_image0_code_start = .;
        KEEP(*(.kickos_image_header))
    } > APPTEXT

    /* kickos_root_entry walks it unprivileged. (READONLY) because the INPUT sections carry
       SHF_WRITE. */
    .kickos_app_init_array (READONLY) :
        AT(ADDR(.kickos_app_init_array) + KICKOS_RV64_APP_LOAD_DELTA) ALIGN(8)
    {
        __kickos_app_init_array_start = .;
        KEEP(*(SORT(.init_array.*)))
        KEEP(*(.init_array))
        __kickos_app_init_array_end = .;
    } > APPTEXT

    /* aspace_image_seed maps this window read-execute in every space. */
    .apptext : AT(ADDR(.apptext) + KICKOS_RV64_APP_LOAD_DELTA) ALIGN(8)
    {
        *(.text .text.*)
        *(.iplt) *(.rela.iplt)
        *(.rodata .rodata.*)
        *(.srodata .srodata.*)
        /* DWARF EH unwind + landing-pad tables for the full-C++ opt-in. KEEP so --gc-sections
           cannot drop it, contiguous, with the LONG(0) terminator standing in for crtend's
           __FRAME_END__. An FDE reaches the code it describes with a 32-bit PC-relative field,
           and the registrar that walks it is libgcc's. */
        *(.gcc_except_table .gcc_except_table.*)
        . = ALIGN(8);
        __eh_frame_start = .;
        KEEP(*(.eh_frame))
        LONG(0)                /* zero-length CIE terminator: crtend is not linked
                                  under -nostartfiles, so the walk needs it here */
        . = ALIGN(8);
    } > APPTEXT

    /* The kernel seeds each thread's block from these bytes through its own alias of them. */
    KICKOS_TLS_TEMPLATE(APPTEXT, AT(ADDR(.tdata) + KICKOS_RV64_APP_LOAD_DELTA))

    /* ASSERTed empty below. (READONLY) for the reason above. */
    .kickos_app_fini_array (READONLY) :
        AT(ADDR(.kickos_app_fini_array) + KICKOS_RV64_APP_LOAD_DELTA) ALIGN(8)
    {
        __kickos_app_fini_array_start = .;
        KEEP(*(SORT(.fini_array.*)))
        KEEP(*(.fini_array))
        __kickos_app_fini_array_end = .;
    } > APPTEXT

    /* Its bytes are in place at the load address the mapping points at; what Reset_Handler owes
       is the zero over .appbss. */
    .appdata : AT(ADDR(.appdata) + KICKOS_RV64_APP_LOAD_DELTA) ALIGN(8)
    {
        /* Small data first, at the region origin: gp is then one address in every program, the
           one the kernel seats at every resume. */
        *(.sdata .sdata.* .sdata2.*)
        *(.sbss .sbss.*)
        . = ALIGN(8);
        __kickos_small_data_end = .;
        /* Here because a writable input section makes the whole output section writable, and
           app text is mapped executable. A static non-PIE link fills them at link time. */
        *(.got) *(.got.plt) *(.igot.plt)
        *(.data .data.*)
        . = ALIGN(8);
    } > APPDATA
    PROVIDE(__global_pointer$ = ORIGIN(APPDATA) + 0x800);  /* gp: middle of the small-data window */

    .appbss (NOLOAD) : AT(ADDR(.appbss) + KICKOS_RV64_APP_LOAD_DELTA) ALIGN(8)
    {
        __kickos_appbss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(8);
        __kickos_appbss_end = .;
    } > APPDATA

    /* user/src/newlib_sbrk.cc names _kickos_heap_start from app text. */
    .userheap (NOLOAD) :
    {
        . = ALIGN(8);
        _kickos_heap_start = .;
        . += KICKOS_USER_HEAP_SIZE;
        _kickos_heap_limit = .;
    } > APPDATA

    /* The app's own window: under enforcement the admission oracle, and what aspace_image_seed
       maps into every space. */
    __kickos_app_rom_start = ORIGIN(APPTEXT);
    __kickos_app_rom_end = ADDR(.kickos_app_fini_array) + SIZEOF(.kickos_app_fini_array);
    __kickos_app_sram_start = ADDR(.appdata);
    __kickos_app_sram_end = _kickos_heap_limit;

    __kickos_image0_code_end = __kickos_app_rom_end;
    __kickos_image0_window_start = ADDR(.appdata);
    __kickos_image0_window_end = _kickos_heap_limit;
    __kickos_image0_data_load = ADDR(.appdata);
    __kickos_image0_data_start = ADDR(.appdata);
    __kickos_image0_data_end = ADDR(.appdata) + SIZEOF(.appdata);
    __kickos_image0_bss_end = __kickos_appbss_end;
    __kickos_image0_heap_start = _kickos_heap_start;
    __kickos_image0_heap_end = _kickos_heap_limit;
    __kickos_image0_ctor_start = __kickos_app_init_array_start;
    __kickos_image0_ctor_end = __kickos_app_init_array_end;
    __kickos_image0_eh_frame = __eh_frame_start;
    KICKOS_IMAGE0_TLS()
    KICKOS_IMAGE0_OWN_NAMES()

    /* The kernel's layout words an image may name, each asserted against the kernel's own by
       the kernel's link (tools/image-link.py). */
    PROVIDE(_sdata = ORIGIN(RAM));
    PROVIDE(_estack = ORIGIN(RAM) + LENGTH(RAM));
    PROVIDE(__kickos_ram_end = ORIGIN(RAM) + LENGTH(RAM) - KICKOS_RV64_BOOT_STACK_SIZE);
    PROVIDE(__kickos_rom_start = ORIGIN(KTEXT));

    /DISCARD/ : { *(.comment) *(.riscv.attributes) }

    KICKOS_NONALLOC_SECTIONS
}

KICKOS_STATIC_RELOC_ASSERT()
KICKOS_TLS_ALIGN_ASSERT()
KICKOS_TLS_FIT_ASSERT()
KICKOS_FINI_EMPTY_ASSERT()
KICKOS_HEAP_MIN_ASSERT()

/* One delta for the whole app window, which is what lets aspace_image_seed carry one number. */
ASSERT(ORIGIN(APPDATA) == ORIGIN(APPTEXT) + LENGTH(APPTEXT),
       "KickOS: the app's two regions are not contiguous, so one load delta cannot describe both")
ASSERT(ORIGIN(APPTEXT) + KICKOS_RV64_APP_LOAD_DELTA == KICKOS_RV64_DRAM_BASE + KICKOS_RV64_KTEXT_SIZE,
       "KickOS: the app window's load delta does not land it directly above the kernel's half")

/* The app window is one level-2 slot of its own: a window straddling two slots would put a
   per-space mapping in a table every space shares. */
ASSERT(ORIGIN(APPTEXT) >= 0x40000000 && ORIGIN(APPDATA) + LENGTH(APPDATA) <= 0x80000000,
       "KickOS: the app window leaves level-2 slot 1, which is the only slot the boot root leaves for it")

/* The gp small-data window must fit __global_pointer$ +/- 0x800 (psABI reach); past that
   reach, gp-relative loads resolve at the wrong addresses. */
ASSERT(__kickos_small_data_end <= __global_pointer$ + 0x800,
       "KickOS: small-data overflows the gp + 0x800 reach")

#else

ENTRY(_start)

_estack = ORIGIN(RAM) + LENGTH(RAM);   /* boot/trap stack: top of RAM */
_kernel_stack_size = KICKOS_RV64_BOOT_STACK_SIZE;  /* boot/trap stack, one stride per hart */

/* The frame pool the kernel's FrameAllocator describes: page tables and the pages a process
   holds. Its own carve, so the bump allocator and the frame allocator never own the same byte. */
#define KICKOS_RV64_FRAME_POOL_SIZE  8M

/* The kernel's view of an image address: the high alias of the frame the image was loaded in. */
__kickos_image_kernel_alias = KICKOS_RV64_VA_BASE + KICKOS_RV64_APP_LOAD_DELTA;

SECTIONS
{
    /* Kernel text and rodata, at the image's lowest load address. .text.init is the reset path,
       taken untranslated. */
    .text ORIGIN(KTEXT) : AT(KICKOS_RV64_DRAM_BASE) ALIGN(8)
    {
        KEEP(*(.text.init))            /* _start: must be the image's lowest load address */
        KEEP(*(.text.privtrap))        /* the trap entry and the restore epilogue */
        *libkickos_kernel.a:*(.text .text.*)
        *libkickos_arch_rv64imac.a:*(.text .text.*)
        *libkickos_chip_virt_rv64.a:*(.text .text.*)
        *(.text .text.*)
        KICKOS_CODE_DEBRIS_SECTIONS
        *libkickos_kernel.a:*(.rodata .rodata.* .srodata .srodata.*)
        *libkickos_arch_rv64imac.a:*(.rodata .rodata.* .srodata .srodata.*)
        *libkickos_chip_virt_rv64.a:*(.rodata .rodata.* .srodata .srodata.*)
        *(.rodata .rodata.* .srodata .srodata.*)
        . = ALIGN(8);
    } > KTEXT

    /* The boot page tables (startup.S), page-aligned because satp and a non-leaf entry name them
       by frame number. Their own section name: the .rodata.* catch-all would otherwise claim them
       and they must reach their own 4 KiB boundary. Kernel-side, and inside the read-execute leaf
       because nothing writes them after boot. */
    .mmu_boot : AT(ADDR(.mmu_boot) - KICKOS_RV64_VA_BASE) ALIGN(4096)
    {
        KEEP(*(.mmu_boot))
    } > KTEXT

    /* Reset_Handler walks this array privileged.

       (READONLY) because the INPUT sections carry SHF_WRITE, and without the keyword the kernel's
       own code segment is flagged writable as well as executable. */
    .init_array (READONLY) : AT(ADDR(.init_array) - KICKOS_RV64_VA_BASE) ALIGN(8)
    {
        __init_array_start = .;
        KEEP(*libkickos_kernel.a:*(SORT(.init_array.*) .init_array))
        KEEP(*libkickos_arch_rv64imac.a:*(SORT(.init_array.*) .init_array))
        KEEP(*libkickos_chip_virt_rv64.a:*(SORT(.init_array.*) .init_array))
        KEEP(*(SORT(.init_array.*) .init_array))
        __init_array_end = .;
    } > KTEXT

    /* Every image's header, and per image where a space maps it from (tools/image-link.py). */
    .kickos_image_table (READONLY) : AT(ADDR(.kickos_image_table) - KICKOS_RV64_VA_BASE) ALIGN(8)
    {
        __kickos_image_table = .;
        INCLUDE kickos_image_table.ld
        __kickos_image_table_end = .;
        __kickos_image_frames = .;
        INCLUDE kickos_image_frames.ld
        __kickos_image_frames_end = .;
    } > KTEXT

    /* Image 0's loadable bytes where its own link put them, and its bounds. */
    INCLUDE kickos_image0.ld

    /* The high alias of the load address: every KERNEL section carries the same VMA-to-LMA
       delta, so this IS _sdata and Reset_Handler's copy is a self-copy. The low value would be a
       dereference of an address no root maps. */
    _sidata = LOADADDR(.data) + KICKOS_RV64_VA_BASE;

    /* The KickOS libs are built -msmall-data-limit=0, and gp is the app's. Small data the
       kernel's link takes is captured here and refused at the end of this file. */
    .kickos_kernel_sdata :
    {
        *(.sdata* .sbss*)
    }

    .data : AT(ADDR(.data) - KICKOS_RV64_VA_BASE) ALIGN(8)
    {
        _sdata = .;
        *libkickos_kernel.a:*(.data .data.*)
        *libkickos_arch_rv64imac.a:*(.data .data.*)
        *libkickos_chip_virt_rv64.a:*(.data .data.*)
        *(.data .data.*)
        . = ALIGN(8);
        _edata = .;
    } > RAM

    /* Every image past image 0, its text and its window each on pages of their own. */
    .kickos_images : AT(ADDR(.kickos_images) - KICKOS_RV64_VA_BASE) ALIGN(4096)
    {
        KEEP(*(.kickos_images))
        . = ALIGN(4096);
    } > RAM

    /* The transient window's leaf table, the one page of boot tables the kernel writes after
       boot, so it cannot sit in the read-execute leaf its siblings do. LOADED and outside
       _sbss.._ebss: _start fills the level-1 entry naming it before Reset_Handler zeroes
       anything. */
    .mmu_leaves : AT(ADDR(.mmu_leaves) - KICKOS_RV64_VA_BASE) ALIGN(4096)
    {
        KEEP(*(.mmu_leaves))
    } > RAM

#if KICKOS_NUM_CORES > 1
    /* The boot handshake (startup.S): the per-hart claim, release and probe words every hart
       reaches untranslated before the boot hart has run the C runtime. LOADED and outside
       _sbss.._ebss for the reason .mmu_leaves is. */
    .smpboot : AT(ADDR(.smpboot) - KICKOS_RV64_VA_BASE) ALIGN(8)
    {
        KEEP(*(.smpboot))
    } > RAM
#endif

    .bss (NOLOAD) : AT(ADDR(.bss) - KICKOS_RV64_VA_BASE) ALIGN(8)
    {
        _sbss = .;
        *libkickos_kernel.a:*(.bss .bss.* COMMON)
        *libkickos_arch_rv64imac.a:*(.bss .bss.* COMMON)
        *libkickos_chip_virt_rv64.a:*(.bss .bss.* COMMON)
        *(.bss .bss.* COMMON)
        . = ALIGN(8);
        _ebss = .;
    } > RAM

    /* Frames, page-aligned because a descriptor's output address is. The FrameAllocator writes
       its bitmap into the first frames, so this is real RAM. */
    .framepool (NOLOAD) : AT(ADDR(.framepool) - KICKOS_RV64_VA_BASE) ALIGN(4096)
    {
        __kickos_frame_pool_start = .;
        . += KICKOS_RV64_FRAME_POOL_SIZE;
        __kickos_frame_pool_end = .;
    } > RAM

    /* The kernel address corresponding to OUTPUT address 0 for the bytes of that carve, so the
       frame allocator can answer in the output addresses a descriptor records. */
    __kickos_frame_pool_delta = KICKOS_RV64_VA_BASE;

    /* From here up to _kernel_stack_size below _estack is the arch_ram_alloc pool
       (runtime bump-allocated, so symbols only, no section to reserve). */
    . = ALIGN(16);
    __kickos_ram_start = .;
    __kickos_ram_end = _estack - _kernel_stack_size;

    /* Hart 0's slice, the size every hart's slice has: the main stack check_trap_redzone.sh
       reads out of the map. */
    __kickos_kernel_stack_start = ABSOLUTE(_estack - KICKOS_RV64_HART_STACK_STRIDE);
    __kickos_kernel_stack_end = ABSOLUTE(_estack);

    /* Chip memory extents read by the descriptor-less user-pointer admission
       (arch_ram_common.cc). Symbols only. This chip executes FROM RAM, so the rom range covers
       the kernel's read-only half and the sram range its writable one. The link needs all
       three defined even under KICKOS_MEMORY_ENFORCED, where the granted-range list is the
       only oracle. */
    __kickos_rom_start = ORIGIN(KTEXT);
    __kickos_rom_end = ADDR(.data);
    __kickos_sram_start = ADDR(.data);


    /DISCARD/ : { *(.comment) *(.riscv.attributes) }

    KICKOS_NONALLOC_SECTIONS
}

KICKOS_STATIC_RELOC_ASSERT()

ASSERT(__kickos_ram_start <= __kickos_ram_end,
       "KickOS: static data overflows RAM (no room for the user-RAM pool + boot stack)")

ASSERT(KICKOS_RV64_HART_STACK_STRIDE >= KICKOS_RV64_TRAP_FRAME_NONE + KICKOS_RV64_MAIN_DEPTH
       && KICKOS_RV64_HART_STACK_STRIDE >= KICKOS_RV64_TRAP_FRAME + KICKOS_RV64_CORE_DEPTH,
       "KickOS: a hart's slice of the boot stack cannot hold the boot path, or a secondary's park and the interrupt nested on it (rv64_trap_stack.h)")

/* The root spends ONE level-2 slot on the kernel window, and a level-2 slot spans 1 GiB. */
ASSERT(_estack <= ORIGIN(KTEXT) + 0x40000000,
       "KickOS: the image outgrows the 1 GiB the root's kernel-window level-1 table maps; startup.S owes a second level-2 entry")

/* Every byte of the two boot-table sections is a page startup.S writes under one name, so the
   hardware walks no table its BOOT_LEAF checks did not read. The root chain above level 2 is
   checked in startup.S, being one span of a single section. */
ASSERT(ADDR(.mmu_boot) == kickos_rv64_root
       && kickos_rv64_kernel_l1 == kickos_rv64_high_l2 + 4096
       && kickos_rv64_window_l1 == kickos_rv64_kernel_l1 + 4096
       && ADDR(.mmu_boot) + SIZEOF(.mmu_boot) == kickos_rv64_window_l1 + 4096,
       "KickOS: .mmu_boot holds bytes no boot table in startup.S accounts for, so the hardware can walk a page whose leaves nothing checked")
ASSERT(ADDR(.mmu_leaves) == kickos_rv64_window_l0 && SIZEOF(.mmu_leaves) == 4096,
       "KickOS: .mmu_leaves holds bytes other than kickos_rv64_window_l0, so the hardware can walk a page whose leaves nothing checked")

/* The write-execute boundary, the half of it the linker owns: nothing stored to is linked below
   the boundary startup.S maps read-execute. */
ASSERT(LOADADDR(.data) - KICKOS_RV64_DRAM_BASE >= KICKOS_RV64_KTEXT_SIZE,
       "KickOS: kernel .data is below the 2 MiB boundary the kernel window splits on; it would be linked into the read-execute leaf")

/* One delta for the whole app window, which is what lets aspace_image_seed carry one number. */
ASSERT(ORIGIN(APPDATA) == ORIGIN(APPTEXT) + LENGTH(APPTEXT),
       "KickOS: the app's two regions are not contiguous, so one load delta cannot describe both")
ASSERT(ORIGIN(APPTEXT) + KICKOS_RV64_APP_LOAD_DELTA == KICKOS_RV64_DRAM_BASE + KICKOS_RV64_KTEXT_SIZE,
       "KickOS: the app window's load delta does not land it directly above the kernel's half")

/* The app window is one level-2 slot of its own: a window straddling two slots would put a
   per-space mapping in a table every space shares. */
ASSERT(ORIGIN(APPTEXT) >= 0x40000000 && ORIGIN(APPDATA) + LENGTH(APPDATA) <= 0x80000000,
       "KickOS: the app window leaves level-2 slot 1, which is the only slot the boot root leaves for it")
ASSERT(__kickos_app_rom_start == ORIGIN(APPTEXT)
       && __kickos_app_rom_end <= ORIGIN(APPTEXT) + LENGTH(APPTEXT)
       && __kickos_app_sram_start == ORIGIN(APPDATA)
       && __kickos_app_sram_end <= ORIGIN(APPDATA) + LENGTH(APPDATA),
       "KickOS: image 0 is not linked in the APPTEXT and APPDATA regions this kernel maps it from")

/* The two halves are out of each other's medany reach, so no single auipc pair spans them. A
   cross-half reference still links silently; the gate named in the file header is what catches
   one. */
ASSERT(ORIGIN(KTEXT) - (ORIGIN(APPDATA) + LENGTH(APPDATA)) > 0x80000000,
       "KickOS: the halves are within medany's reach of each other, so one auipc pair spans them")

#include <boot_arena.ld.h>
KICKOS_BOOT_ARENA_ASSERT(__kickos_ram_start, __kickos_ram_end)

KICKOS_POOL_ARENA_ASSERT(__kickos_ram_start, __kickos_ram_end)

ASSERT(SIZEOF(.kickos_kernel_sdata) == 0,
       "KickOS: a KickOS archive emits .sdata/.sbss small data, which belongs to the app's gp window. Build every KickOS RISC-V lib -msmall-data-limit=0 (kickos_apply_freestanding in cmake/kickos.cmake)")


/* The app windows this script does not carve, stated EMPTY rather than left absent:
   the kernel's references are strong, so an omitted bound is a link error. */
KICKOS_APP_CODE_WINDOW_NONE()
KICKOS_APP_DATA_WINDOW_NONE()

INCLUDE kickos_image0_checks.ld

#endif
