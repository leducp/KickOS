/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * Linker script for the QEMU `virt` RISC-V board (RV64IMAC bare metal).
 * QEMU (`-bios none -kernel <elf>`) loads each PT_LOAD segment at its p_paddr and enters at
 * the LOWEST of them rather than at the ELF entry field, so .text.init must stay first in
 * .text and .text must keep the lowest load address in the image.
 *
 * THE IMAGE IS SPLIT IN TWO (docs/design-m6-mmu.md R2.2). Kernel text and kernel writable
 * state link into the high range the top gigabyte covers, with an LMA back in DRAM; the
 * app's text, rodata, data and bss link at KICKOS_RV64_APP_VA_BASE, one level-2 slot the boot
 * root leaves empty, so a per-space root owns that subtree outright.
 *
 * THE APP'S VMA IS NOT ITS LMA, AND NO PLACEMENT MAKES IT ONE. The prebuilt libc and libgcc
 * multilibs are medlow, which materialises bits 31:12 sign-extended and so reaches
 * [0, 0x7FFFFFFF] and [0xFFFFFFFF80000000, 0xFFFFFFFFFFFFFFFF] and nothing between. Every byte
 * of this machine's DRAM is at or above 0x80000000, so an identity-linked app window does not
 * exist: the app links low and loads high, one uniform delta for both its regions, published as
 * __kickos_app_load_delta and added by aspace_image_seed to reach the frames behind a page.
 *
 * THE HALVES ARE OUT OF EACH OTHER'S REACH BY CONSTRUCTION and that is the point: RISC-V has
 * neither a large code model nor a long-branch veneer, so a CALL across the boundary is an
 * auipc truncation the linker REFUSES, auipc+jalr having no absolute relaxation form. A kernel
 * reference to an app DATUM therefore has to be a relocated 64-bit word rather than an address
 * materialised in code. A missed one does NOT fail the link: medlow's absolute reach covers the
 * app's base, so the pair relaxes to lui+addi, or onto the app's gp anchor, and links silently.
 * tests/static/check_riscv_kernel_apphalf.sh and check_riscv_kernel_gp.sh refuse those two.
 *
 * THE SELECTION IS INVERTED, as on every enforcing board: the CLOSED KickOS-owned kernel set
 * is named in script order and the app catch-alls take everything else, so an archive nobody
 * listed lands app-side rather than inside the kernel's half.
 *
 * Chip<->arch contract: define __kickos_ram_start/__kickos_ram_end (the arch_ram_alloc
 * pool), _estack (boot stack top), and _sidata/.../_ebss (C runtime). __global_pointer$
 * anchors gp-relative small-data access and is the APP's: the KickOS-owned archives are built
 * -msmall-data-limit=0 here, which keeps kernel DATA out of the small-data sections. That flag
 * does NOT stop a kernel access resolving through gp: the linker MAKES gp addressing out of any
 * upper/lower pair whose target lands within gp +/- 0x800, and a kernel reference to an app-half
 * symbol is exactly that (R2.2 measured kmain's store come out as `sd a1,-2000(gp)`). What holds
 * it is the trap entry's re-anchor plus check_riscv_kernel_gp.sh over the LINKED image.
 *
 * Chip<->kernel contract under KICKOS_HAVE_ASPACE: __kickos_frame_pool_start/_end and
 * __kickos_frame_pool_delta, the frame carve the FrameAllocator describes.
 *
 * NO REGION DESCRIPTORS: this chip selects translation, so arch/Kconfig selects no HAS_MPU
 * and KICKOS_HAVE_MPU resolves 0. There is no pow2 NAPOT window here and no PMP to name
 * one. The 8-byte alignments are RV64's: a pointer is 8 bytes and the psABI wants a
 * 16-byte stack alignment, which _estack at the top of RAM already satisfies.
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
_kernel_stack_size = 64K;              /* boot/trap stack reservation */

/* The frame pool the kernel's FrameAllocator describes: page tables and the pages a process
   holds. Its own carve rather than a slice of the arena, so the bump allocator and the frame
   allocator never own the same byte and KICKOS_POOL_ARENA_ASSERT keeps modelling the arena
   exactly. */
#define KICKOS_RV64_FRAME_POOL_SIZE  8M

SECTIONS
{
    /* KERNEL TEXT AND RODATA, at the image's lowest load address. .text.init is the reset
       path, taken untranslated; .text.privtrap is the trap entry and the restore epilogue,
       which keep a section of their own because startup.S and switch.S name it and because a
       W^X split of this half (R2.3) draws its boundary between them and the rest. The
       archive catch-alls in .apptext below cannot claim these: an input section goes to the
       first output section that names it. */
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

    /* The boot page tables: the root and one page per level the configured mode adds above
       level 2, the level-2 table, the kernel window's level-1 table and the transient window's
       level-1 table (startup.S), page-aligned because satp and a non-leaf entry name them by
       frame number. NOT under .rodata.*: the catch-all claims that spelling, and these have to reach
       their own 4 KiB boundary rather than share a page with rodata. Kernel-side, so no
       unprivileged thread has any mapping of the tables that constrain it. Inside the
       read-execute leaf, which no entry here needs: the one table page written after boot is
       the transient window's LEAF table, in .mmu_leaves below (docs/design-m6-mmu.md R2.3). */
    .mmu_boot : AT(ADDR(.mmu_boot) - KICKOS_RV64_VA_BASE) ALIGN(4096)
    {
        KEEP(*(.mmu_boot))
    } > KTEXT

    /* Global ctors SPLIT: .init_array keeps ONLY the three archives whose TEXT the kernel
       selectors above also claim (kernel/arch/chip), run early by Reset_Handler; every other
       ctor, libkickos_lib.a's included, lands in .kickos_app_init_array and runs from
       root_entry with the kernel live. THIS array stays kernel-side, Reset_Handler walking it
       privileged.

       THE SELECTOR LIST IS THE KERNEL TEXT LIST AND NOTHING ELSE. libkickos_lib.a used to
       stand here while the .apptext catch-all took its text app-side: a ctor of that archive's
       would then be walked privileged at boot as an S-mode fetch of a U=1 page.

       (READONLY) because the INPUT sections carry SHF_WRITE, and the leaf this lands in is
       read-execute: nothing writes the array, and without the keyword the kernel's own code
       segment is flagged writable as well as executable. */
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

    /* The template the kernel copies each thread's TLS block FROM, so it is read by kernel
       text and stays in the kernel's half. */
    KICKOS_TLS_TEMPLATE(KTEXT)

    /* THE APP'S CTOR LIST, app-side because its WALKER is: kickos_root_entry runs
       unprivileged, so an array in the kernel's half is a read it cannot make. FIRST in
       APPTEXT, so the window's low bound below can name the region origin and stay right
       whether or not this section is empty.

       (READONLY) because the INPUT sections carry SHF_WRITE: without it the app's code
       segment is flagged writable AND executable, which is what keeping .got out of it
       exists to avoid. */
    .kickos_app_init_array (READONLY) :
        AT(ADDR(.kickos_app_init_array) + KICKOS_RV64_APP_LOAD_DELTA) ALIGN(8)
    {
        __kickos_app_init_array_start = .;
        KEEP(*(SORT(.init_array.*)))
        KEEP(*(.init_array))
        __kickos_app_init_array_end = .;
    } > APPTEXT

    /* THE APP'S TEXT AND RODATA. Everything the kernel selectors above did not claim lands
       here, the toolchain runtime (libc/libgcc/libstdc++), libkickos_lib.a and any unlisted
       archive included. aspace_image_seed maps this window read-execute in every space.

       .apptrap is the ecall wrapper (arch/riscv/rv64imac/switch.S). Its own section name
       because the archive that holds it is kernel-side and `.text.*` would capture it up
       there. */
    .apptext : AT(ADDR(.apptext) + KICKOS_RV64_APP_LOAD_DELTA) ALIGN(8)
    {
        *(.apptrap)
        *(.text .text.*)
        *(.rodata .rodata.*)
        *(.srodata .srodata.*)
        /* DWARF EH unwind + landing-pad tables for the full-C++ opt-in; a plain
           freestanding app is built -fno-exceptions, which leaves the section holding just
           the terminator. KEEP so --gc-sections cannot drop it, contiguous, with the LONG(0)
           terminator standing in for crtend's __FRAME_END__.

           APP-SIDE, AND NOT A CHOICE: an FDE reaches the code it describes with a 32-bit
           PC-relative field, so the table sits in the same half as that code, and the
           registrar that walks it is libgcc's, which is app-side too. That is why the
           registration moved out of Reset_Handler and into kickos_root_entry, which runs
           with this window mapped (user/src/root_entry.cc). */
        *(.gcc_except_table .gcc_except_table.*)
        . = ALIGN(8);
        __eh_frame_start = .;
        KEEP(*(.eh_frame))
        LONG(0)                /* zero-length CIE terminator: crtend is not linked
                                  under -nostartfiles, so the walk needs it here */
        . = ALIGN(8);
    } > APPTEXT

    /* The high alias of the load address. Every KERNEL section carries the same VMA-to-LMA
       delta, so this IS _sdata and Reset_Handler's copy is a self-copy; the LOW value here
       would be a dereference of an address no root maps. */
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

    /* THE TRANSIENT WINDOW'S LEAF TABLE, the one page of boot tables the kernel writes after
       boot (arch_aspace_acquire and arch_aspace_release), so it cannot sit in the read-execute
       leaf its siblings do (docs/design-m6-mmu.md R2.3). Loaded and outside _sbss.._ebss: the
       level-1 entry naming it is filled by _start, before Reset_Handler zeroes anything. */
    .mmu_leaves : AT(ADDR(.mmu_leaves) - KICKOS_RV64_VA_BASE) ALIGN(4096)
    {
        KEEP(*(.mmu_leaves))
    } > RAM

    .bss (NOLOAD) : AT(ADDR(.bss) - KICKOS_RV64_VA_BASE) ALIGN(8)
    {
        _sbss = .;
        *libkickos_kernel.a:*(.sbss .sbss.* .bss .bss.* COMMON)
        *libkickos_arch_rv64imac.a:*(.sbss .sbss.* .bss .bss.* COMMON)
        *libkickos_chip_virt_rv64.a:*(.sbss .sbss.* .bss .bss.* COMMON)
        . = ALIGN(8);
        _ebss = .;
    } > RAM

    /* THE APP'S WRITABLE STATE. Its bytes are ALREADY IN PLACE at the load address the
       mapping points at, so there is no app-side copy to run: what Reset_Handler still owes
       is the ZERO over .appbss, reached through the kernel's own alias of the same frames
       (__kickos_app_load_delta plus the kernel window's base). */
    .appdata : AT(ADDR(.appdata) + KICKOS_RV64_APP_LOAD_DELTA) ALIGN(8)
    {
        /* A static non-PIE link fills these at link time, so they are never written at run
           time. They sit here rather than with app text because a writable input section
           makes the whole output section writable, and app text is mapped executable. */
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

    /* The newlib heap is the app's: user/src/newlib_sbrk.cc names _kickos_heap_start from
       app text, which cannot reach the kernel's half at all. */
    .userheap (NOLOAD) :
    {
        . = ALIGN(8);
        _kickos_heap_start = .;
        . += KICKOS_USER_HEAP_SIZE;
        _kickos_heap_limit = .;
    } > APPDATA

    /* Frames, page-aligned because a descriptor's output address is. The FrameAllocator
       writes its bitmap into the first frames, so this is real RAM and not just symbols. */
    .framepool (NOLOAD) : AT(ADDR(.framepool) - KICKOS_RV64_VA_BASE) ALIGN(4096)
    {
        __kickos_frame_pool_start = .;
        . += KICKOS_RV64_FRAME_POOL_SIZE;
        __kickos_frame_pool_end = .;
    } > RAM

    /* The kernel address that corresponds to OUTPUT address 0 for the bytes of that carve,
       so the frame allocator can answer in the output addresses a descriptor records. It
       says nothing about the rest of physical memory, which is the backend's own map. */
    __kickos_frame_pool_delta = KICKOS_RV64_VA_BASE;

    /* From here up to _kernel_stack_size below _estack is the arch_ram_alloc pool
       (runtime bump-allocated, so symbols only, no section to reserve). */
    . = ALIGN(16);
    __kickos_ram_start = .;
    __kickos_ram_end = _estack - _kernel_stack_size;

    /* Chip memory extents read by the descriptor-less user-pointer admission
       (arch_ram_common.cc). Symbols only, no layout effect. This chip executes FROM RAM, so
       the rom range covers the kernel's read-only half and the sram range its writable one.
       Under KICKOS_MEMORY_ENFORCED neither is an oracle any more, the granted-range list
       being the only one, but the link still needs all three defined. */
    __kickos_rom_start = ORIGIN(KTEXT);
    __kickos_rom_end = ADDR(.data);
    __kickos_sram_start = ADDR(.data);

    /* THE APP'S OWN WINDOW, which under enforcement IS the admission oracle and is what
       aspace_image_seed maps into every space. Weakly declared kernel-side, so a chip that
       carves no such window admits nothing extra. */
    __kickos_app_rom_start = ORIGIN(APPTEXT);
    __kickos_app_rom_end = ADDR(.apptext) + SIZEOF(.apptext);
    __kickos_app_sram_start = ADDR(.appdata);
    __kickos_app_sram_end = _kickos_heap_limit;

    /* Added to an app VIRTUAL address to reach the frame the loader put those bytes in. An
       ABSOLUTE symbol whose VALUE is the whole offset, so kernel code reaches it as a
       relocated word and never PC-relatively (kernel/mem/aspace.cc). */
    __kickos_app_load_delta = KICKOS_RV64_APP_LOAD_DELTA;

    /DISCARD/ : { *(.comment) *(.riscv.attributes) }

    KICKOS_NONALLOC_SECTIONS
}

KICKOS_STATIC_RELOC_ASSERT()
KICKOS_TLS_ALIGN_ASSERT()
KICKOS_TLS_FIT_ASSERT()

ASSERT(__kickos_ram_start <= __kickos_ram_end,
       "KickOS: static data overflows RAM (no room for the user-RAM pool + boot stack)")

/* The root spends ONE level-2 slot on the kernel window, and a level-2 slot spans 1 GiB. Past
   that boundary the image would need a second entry startup.S does not fill, and the fetch or
   store would take a page fault instead of failing the build. */
ASSERT(_estack <= ORIGIN(KTEXT) + 0x40000000,
       "KickOS: the image outgrows the 1 GiB the root's kernel-window level-1 table maps; startup.S owes a second level-2 entry")

/* THE WRITE-EXECUTE BOUNDARY (docs/design-m6-mmu.md R2.3), the one half of it the linker owns:
   startup.S maps the KTEXT region read-execute and everything above it read-write, and it
   refuses a region size that is not a whole number of 2 MiB leaves, so what is left to state
   here is that nothing stored to is linked below the boundary. The app's load window is covered
   by the delta ASSERT above, which pins it to exactly that address. */
ASSERT(LOADADDR(.data) - KICKOS_RV64_DRAM_BASE >= KICKOS_RV64_KTEXT_SIZE,
       "KickOS: kernel .data is below the 2 MiB boundary the kernel window splits on; it would be linked into the read-execute leaf")

/* ONE DELTA FOR THE WHOLE APP WINDOW, which is what lets aspace_image_seed carry a single
   number. The two app regions are contiguous in both spaces or the seed would map the wrong
   frames behind the second one. */
ASSERT(ORIGIN(APPDATA) == ORIGIN(APPTEXT) + LENGTH(APPTEXT),
       "KickOS: the app's two regions are not contiguous, so one load delta cannot describe both")
ASSERT(ORIGIN(APPTEXT) + KICKOS_RV64_APP_LOAD_DELTA == KICKOS_RV64_DRAM_BASE + KICKOS_RV64_KTEXT_SIZE,
       "KickOS: the app window's load delta does not land it directly above the kernel's half")

/* THE APP WINDOW IS ONE LEVEL-2 SLOT OF ITS OWN. A window straddling two slots, or sharing
   the identity gigabyte the chip names its devices in, would put a per-space mapping in a
   table every space shares. */
ASSERT(ORIGIN(APPTEXT) >= 0x40000000 && ORIGIN(APPDATA) + LENGTH(APPDATA) <= 0x80000000,
       "KickOS: the app window leaves level-2 slot 1, which is the only slot the boot root leaves for it")

/* THE TWO HALVES ARE OUT OF EACH OTHER'S medany REACH, medany spanning a signed 32-bit
   displacement. That is the placement invariant F1 asks for and nothing more: no single auipc
   pair spans the halves. IT DOES NOT TURN A MISSED CROSS-HALF REFERENCE INTO A LINK ERROR: the
   app window at 0x40000000 is inside medlow's absolute reach, so the linker RELAXES such a
   reference to lui+addi and the link succeeds silently. What catches a crossing is two gates over
   two corpora: tests/static/check_riscv_kernel_apphalf.sh over the kernel archives' relocations and
   tests/static/check_riscv_kernel_gp.sh over the linked image for the gp direction, which lives
   inside 2 KiB of one anchor and is the opposite end of this range. */
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

/* SELECTOR-MISMATCH GUARD, the same one the enforcing boards carry. The inversion captures
   the closed kernel set and lets the app catch-alls take the rest, so a broken selector shows
   up as an EMPTY KERNEL side: kernel writable state silently linked into the app's window,
   which is the one window an unprivileged thread still reaches. An empty app side is
   legitimate (a minimal freestanding app has had its own state --gc-sections'd away), so only
   the kernel side is asserted. */
ASSERT(_ebss > _sbss,
       "KickOS: kernel .bss empty: closed-set archive selector matched nothing (renamed lib?)")

/* Opt-in link-time heap floor (undefined by default). */
#ifdef KICKOS_HEAP_MIN
ASSERT(_kickos_heap_limit - _kickos_heap_start >= KICKOS_HEAP_MIN,
       "KickOS: heap below KICKOS_HEAP_MIN")
#endif
