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
 * one level-2 slot the boot root leaves empty. The app links low and loads high over one
 * uniform delta, published as __kickos_app_load_delta.
 *
 * A CROSS-HALF REFERENCE CAN LINK SILENTLY. What catches one is two gates:
 * tests/static/check_riscv_kernel_apphalf.sh over the kernel archives' relocations and
 * tests/static/check_riscv_kernel_gp.sh over the linked image for the gp direction.
 *
 * THE SELECTION IS INVERTED: the closed KickOS-owned kernel set is named in script order and the
 * app catch-alls take everything else, so an archive nobody listed lands app-side.
 *
 * Chip<->arch contract: define __kickos_ram_start/__kickos_ram_end (the arch_ram_alloc pool),
 * _estack (boot stack top), and _sidata/.../_ebss (C runtime). __global_pointer$ is the APP's.
 * Under KICKOS_HAVE_ASPACE, also __kickos_frame_pool_start/_end and __kickos_frame_pool_delta.
 */

#include <sections.ld.h>
#include "boot_layout.ld.h"

ENTRY(_start)

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

_estack = ORIGIN(RAM) + LENGTH(RAM);   /* boot/trap stack: top of RAM */
_kernel_stack_size = KICKOS_RV64_BOOT_STACK_SIZE;  /* boot/trap stack, one stride per hart */

/* The frame pool the kernel's FrameAllocator describes: page tables and the pages a process
   holds. Its own carve, so the bump allocator and the frame allocator never own the same byte. */
#define KICKOS_RV64_FRAME_POOL_SIZE  8M

SECTIONS
{
    /* Kernel text and rodata, at the image's lowest load address. .text.init is the reset path,
       taken untranslated. The archive catch-alls in .apptext below cannot claim these: an input
       section goes to the first output section that names it. */
    .text ORIGIN(KTEXT) : AT(KICKOS_RV64_DRAM_BASE) ALIGN(8)
    {
        KEEP(*(.text.init))            /* _start: must be the image's lowest load address */
        KEEP(*(.text.privtrap))        /* the trap entry and the restore epilogue */
        *libkickos_kernel.a:*(.text .text.*)
        *libkickos_arch_rv64imac.a:*(.text .text.*)
        *libkickos_chip_virt_rv64.a:*(.text .text.*)
        KICKOS_CODE_DEBRIS_SECTIONS
        *libkickos_kernel.a:*(.rodata .rodata.* .srodata .srodata.*)
        *libkickos_arch_rv64imac.a:*(.rodata .rodata.* .srodata .srodata.*)
        *libkickos_chip_virt_rv64.a:*(.rodata .rodata.* .srodata .srodata.*)
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

    /* Global ctors SPLIT. THE SELECTOR LIST HERE IS THE KERNEL TEXT LIST AND NOTHING ELSE:
       Reset_Handler walks this array privileged, so a ctor of an archive whose text went app-side
       would be an S-mode fetch of a U=1 page. Every other ctor lands in .kickos_app_init_array
       and runs from root_entry.

       (READONLY) because the INPUT sections carry SHF_WRITE, and without the keyword the kernel's
       own code segment is flagged writable as well as executable. */
    .init_array (READONLY) : AT(ADDR(.init_array) - KICKOS_RV64_VA_BASE) ALIGN(8)
    {
        __init_array_start = .;
        KEEP(*libkickos_kernel.a:*(SORT(.init_array.*) .init_array))
        KEEP(*libkickos_arch_rv64imac.a:*(SORT(.init_array.*) .init_array))
        KEEP(*libkickos_chip_virt_rv64.a:*(SORT(.init_array.*) .init_array))
        __init_array_end = .;
    } > KTEXT

    /* .fini_array, ASSERTed empty below; nothing walks it. (READONLY) for the reason above. */
    .kickos_app_fini_array (READONLY) :
        AT(ADDR(.kickos_app_fini_array) - KICKOS_RV64_VA_BASE) ALIGN(8)
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
    .kickos_app_init_array (READONLY) :
        AT(ADDR(.kickos_app_init_array) + KICKOS_RV64_APP_LOAD_DELTA) ALIGN(8)
    {
        __kickos_app_init_array_start = .;
        KEEP(*(SORT(.init_array.*)))
        KEEP(*(.init_array))
        __kickos_app_init_array_end = .;
    } > APPTEXT

    /* The app's text and rodata: everything the kernel selectors above did not claim.
       aspace_image_seed maps this window read-execute in every space.

       .apptrap is the ecall wrapper (switch.S). Its own section name because the archive that
       holds it is kernel-side and `.text.*` would capture it up there. */
    .apptext : AT(ADDR(.apptext) + KICKOS_RV64_APP_LOAD_DELTA) ALIGN(8)
    {
        *(.apptrap)
        *(.text .text.*)
        *(.rodata .rodata.*)
        *(.srodata .srodata.*)
        /* DWARF EH unwind + landing-pad tables for the full-C++ opt-in. KEEP so --gc-sections
           cannot drop it, contiguous, with the LONG(0) terminator standing in for crtend's
           __FRAME_END__. App-side: an FDE reaches the code it describes with a 32-bit
           PC-relative field, and the registrar that walks it is libgcc's. Registration is in
           kickos_root_entry, which runs with this window mapped (user/src/root_entry.cc). */
        *(.gcc_except_table .gcc_except_table.*)
        . = ALIGN(8);
        __eh_frame_start = .;
        KEEP(*(.eh_frame))
        LONG(0)                /* zero-length CIE terminator: crtend is not linked
                                  under -nostartfiles, so the walk needs it here */
        . = ALIGN(8);
    } > APPTEXT

    /* The high alias of the load address: every KERNEL section carries the same VMA-to-LMA
       delta, so this IS _sdata and Reset_Handler's copy is a self-copy. The low value would be a
       dereference of an address no root maps. */
    _sidata = LOADADDR(.data) + KICKOS_RV64_VA_BASE;

    .data : AT(ADDR(.data) - KICKOS_RV64_VA_BASE) ALIGN(8)
    {
        _sdata = .;
        *libkickos_kernel.a:*(.data .data.* .sdata .sdata.*)
        *libkickos_arch_rv64imac.a:*(.data .data.* .sdata .sdata.*)
        *libkickos_chip_virt_rv64.a:*(.data .data.* .sdata .sdata.*)
        . = ALIGN(8);
        _edata = .;
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
       _sbss.._ebss for the reason .mmu_leaves is, and absent at one core so a single-core
       image carries no section for a handshake it has nobody to make. */
    .smpboot : AT(ADDR(.smpboot) - KICKOS_RV64_VA_BASE) ALIGN(8)
    {
        KEEP(*(.smpboot))
    } > RAM
#endif

    .bss (NOLOAD) : AT(ADDR(.bss) - KICKOS_RV64_VA_BASE) ALIGN(8)
    {
        _sbss = .;
        *libkickos_kernel.a:*(.sbss .sbss.* .bss .bss.* COMMON)
        *libkickos_arch_rv64imac.a:*(.sbss .sbss.* .bss .bss.* COMMON)
        *libkickos_chip_virt_rv64.a:*(.sbss .sbss.* .bss .bss.* COMMON)
        . = ALIGN(8);
        _ebss = .;
    } > RAM

    /* The app's writable state. Its bytes are already in place at the load address the mapping
       points at; what Reset_Handler owes is the zero over .appbss, reached through the kernel's
       alias of the same frames (__kickos_app_load_delta plus the kernel window's base). */
    .appdata : AT(ADDR(.appdata) + KICKOS_RV64_APP_LOAD_DELTA) ALIGN(8)
    {
        /* Here because a writable input section makes the whole output section writable, and
           app text is mapped executable. A static non-PIE link fills them at link time. */
        *(.got) *(.got.plt)
        *(.data .data.*)
        . = ALIGN(8);
        PROVIDE(__global_pointer$ = . + 0x800);  /* gp: middle of the small-data window */
        *(.sdata .sdata.* .sdata2.*)
        . = ALIGN(8);
    } > APPDATA

    .appbss (NOLOAD) : AT(ADDR(.appbss) + KICKOS_RV64_APP_LOAD_DELTA) ALIGN(8)
    {
        __kickos_appbss_start = .;
        *(.sbss .sbss.*)
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(8);
        __kickos_appbss_end = .;
    } > APPDATA

    /* The app's: user/src/newlib_sbrk.cc names _kickos_heap_start from app text. */
    .userheap (NOLOAD) :
    {
        . = ALIGN(8);
        _kickos_heap_start = .;
        . += KICKOS_USER_HEAP_SIZE;
        _kickos_heap_limit = .;
    } > APPDATA

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

    /* Chip memory extents read by the descriptor-less user-pointer admission
       (arch_ram_common.cc). Symbols only. This chip executes FROM RAM, so the rom range covers
       the kernel's read-only half and the sram range its writable one. The link needs all
       three defined even under KICKOS_MEMORY_ENFORCED, where the granted-range list is the
       only oracle. */
    __kickos_rom_start = ORIGIN(KTEXT);
    __kickos_rom_end = ADDR(.data);
    __kickos_sram_start = ADDR(.data);

    /* The app's own window: under enforcement the admission oracle, and what aspace_image_seed
       maps into every space. Weakly declared kernel-side, so a chip that carves no such window
       leaves its pair equal. */
    __kickos_app_rom_start = ORIGIN(APPTEXT);
    __kickos_app_rom_end = ADDR(.apptext) + SIZEOF(.apptext);
    __kickos_app_sram_start = ADDR(.appdata);
    __kickos_app_sram_end = _kickos_heap_limit;

    /* Added to an app VIRTUAL address to reach the frame the loader put those bytes in. */
    __kickos_app_load_delta = KICKOS_RV64_APP_LOAD_DELTA;

    /DISCARD/ : { *(.comment) *(.riscv.attributes) }

    KICKOS_NONALLOC_SECTIONS
}

KICKOS_STATIC_RELOC_ASSERT()
KICKOS_TLS_ALIGN_ASSERT()
KICKOS_TLS_FIT_ASSERT()

ASSERT(__kickos_ram_start <= __kickos_ram_end,
       "KickOS: static data overflows RAM (no room for the user-RAM pool + boot stack)")

/* The root spends ONE level-2 slot on the kernel window, and a level-2 slot spans 1 GiB. */
ASSERT(KICKOS_RV64_HART_STACK_STRIDE >= 0x2000,
       "KickOS: the per-hart boot stack stride is under 8 KiB; a parked secondary builds an interrupt frame on it")

ASSERT(_estack <= ORIGIN(KTEXT) + 0x40000000,
       "KickOS: the image outgrows the 1 GiB the root's kernel-window level-1 table maps; startup.S owes a second level-2 entry")

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

/* The two halves are out of each other's medany reach, so no single auipc pair spans them. A
   cross-half reference still links silently; the two gates named in the file header are what
   catch one. */
ASSERT(ORIGIN(KTEXT) - (ORIGIN(APPDATA) + LENGTH(APPDATA)) > 0x80000000,
       "KickOS: the halves are within medany's reach of each other, so one auipc pair spans them")

#include <boot_arena.ld.h>
KICKOS_BOOT_ARENA_ASSERT(__kickos_ram_start, __kickos_ram_end)

KICKOS_POOL_ARENA_ASSERT(__kickos_ram_start, __kickos_ram_end)
ASSERT(__kickos_app_fini_array_end == __kickos_app_fini_array_start,
       "KickOS: .fini_array is non-empty: a global with a non-trivial destructor? Static destructors are not supported; nothing would ever run it")

/* The gp small-data window must fit __global_pointer$ +/- 0x800 (psABI reach); past that
   reach, gp-relative loads resolve at the wrong addresses. */
ASSERT(ADDR(.appdata) + SIZEOF(.appdata) <= __global_pointer$ + 0x800,
       "KickOS: small-data overflows the gp + 0x800 reach")

/* Selector-mismatch guard: a broken selector shows up as an EMPTY KERNEL side, kernel writable
   state silently linked into the app's window. An empty app side is legitimate, so only the
   kernel side is asserted. */
ASSERT(_ebss > _sbss,
       "KickOS: kernel .bss empty: closed-set archive selector matched nothing (renamed lib?)")

/* Opt-in link-time heap floor (undefined by default). */
#ifdef KICKOS_HEAP_MIN
ASSERT(_kickos_heap_limit - _kickos_heap_start >= KICKOS_HEAP_MIN,
       "KickOS: heap below KICKOS_HEAP_MIN")
#endif
