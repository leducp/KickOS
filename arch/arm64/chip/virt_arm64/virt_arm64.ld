/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * Linker script for the QEMU `virt` AArch64 board (Cortex-A53, EL1 bare metal).
 * QEMU (`-kernel <elf>`) loads each segment at its p_paddr and enters at the ELF entry
 * point with the MMU off. So `.text.init` links where it loads.
 *
 * THE IMAGE IS SPLIT IN TWO (docs/design-m6-mmu.md, T5b.1). Kernel text and kernel
 * writable state link into the high half TTBR1 translates, with an LMA back in DRAM; the
 * app's text, rodata, data and bss link LOW, where a per-process TTBR0 root can name them
 * and where VMA == LMA. App TEXT moves and not just app data because high-half text cannot
 * name a low-half symbol under the small code model, and the app's own text must be
 * reachable from the app's own data.
 *
 * The reverse reach is what the kernel side pays for: the halves are 2^40 apart and adrp
 * spans 4 GiB, so the kernel-side TUs that name the app's half are built -mcmodel=large
 * (kickos_split_image_tu, cmake/kickos.cmake). That also leaves the ONE .got a static
 * link has entirely to the app, which matters because a GOT slot is itself reached by adrp
 * and a GOT with users in both halves could not be placed at all.
 *
 * Chip<->arch contract: define __kickos_ram_start/__kickos_ram_end (the arch_ram_alloc
 * pool), _estack (boot stack top), and _sidata/.../_ebss (C runtime).
 *
 * Chip<->kernel contract under KICKOS_HAVE_ASPACE: __kickos_frame_pool_start/_end and
 * __kickos_frame_pool_delta, the frame carve the FrameAllocator describes.
 *
 * NO REGION DESCRIPTORS, WHICH IS NOT THE SAME AS NO ENFORCEMENT. This chip has no region
 * MPU and its arch ships no arch_mpu_apply, so arch/Kconfig selects no HAS_MPU and
 * KICKOS_HAVE_MPU resolves 0, while KICKOS_MEMORY_ENFORCED is 1 here, translation being
 * the enforcement. So the app window below is not the descriptor-gated .appdata window an
 * MPU chip carves; it is a LINK split, and what an unprivileged thread may reach is its own
 * TTBR0 root alone: TTBR1's level-2 table grants EL0 nothing (startup.S), so every section
 * placed in KTEXT or RAM below is unreachable from EL0 at any address.
 *
 * The 8-byte alignments below are AArch64's: a pointer is 8 bytes and the ABI requires
 * a 16-byte stack alignment, which _estack at the top of RAM already satisfies.
 */

#include <sections.ld.h>

/* _start (startup.S), not Reset_Handler: --gc-sections reaches .text.init through
 * nothing else. The family `arm` link forces its vector table in with -u,g_isr_vector
 * (top-level CMakeLists) and that guard does not match `arm64`, so ENTRY is the only
 * thing anchoring the startup object here.
 *
 * _start is in the identity-linked section, so the ELF entry field is a physical address.
 */
ENTRY(_start)

#define KICKOS_ARM64_DRAM_BASE  0x40000000      /* QEMU virt DRAM (ample) */
#define KICKOS_ARM64_DRAM_SIZE  64M

/* TCR_EL1.T1SZ is 25 (startup.S), which makes the high half the 39-bit range at this
 * base. VA is PA plus this, so the kernel window and the map of physical RAM are one set
 * of descriptors. A multiple of 2^39, so every alignment below is the same either side. */
#define KICKOS_ARM64_VA_BASE    0xFFFFFF8000000000

/* The identity-linked span: one page for the entry code, the EL refusal reporter and the
 * strings it reads, plus the FOUR 4 KiB translation tables the two TTBRs are pointed at: a
 * root each, and a level-2 table each, the two halves describing the same DRAM with
 * different EL0 permissions. ld refuses an overflow, so a fifth table page has to raise
 * this. */
#define KICKOS_ARM64_BOOT_SIZE  20K

/* The ONE level-2 block startup.S maps read-only and executable; every block above it is
 * writable and executable at neither level. So everything that is fetched has to fit below
 * this boundary and everything that is stored to has to start at it, on BOTH sides of the
 * split. */
#define KICKOS_ARM64_CODE_BLOCK 2M

/* The kernel's share of the code block. The rest of it is the app's, so raising one lowers
 * the other; ld reports the overflow by region name. */
#define KICKOS_ARM64_KTEXT_SIZE 1M

/* The app's writable window: its .data, .bss and the newlib heap. */
#define KICKOS_ARM64_APP_DATA_SIZE 1M

/* The frame pool the kernel's FrameAllocator describes: page tables and the pages a
 * process holds. Its own carve rather than a slice of the arena, so the bump allocator
 * and the frame allocator never own the same byte and KICKOS_POOL_ARENA_ASSERT keeps
 * modelling the arena exactly. */
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
_kernel_stack_size = 64K;              /* boot/exception stack reservation */

/* startup.S reads this to reach a physical address from a high one, in the window before
 * the MMU is on where an absolute literal cannot be translated. */
__kickos_arm64_va_base = KICKOS_ARM64_VA_BASE;

SECTIONS
{
    /* VMA == LMA: the entry is taken untranslated, and a translation-table descriptor
       names its output by physical address. .rodata.init is the same need for read-only
       data, the strings the EL refusal reporter writes before the MMU is on
       (chip_virt_arm64.cc); the catch-alls below would take both input sections, so this
       output section has to stay FIRST in script order to claim them. */
    .text.init ORIGIN(BOOT) : ALIGN(8)
    {
        KEEP(*(.text.init))
        . = ALIGN(8);
        KEEP(*(.rodata.init))
        . = ALIGN(4096);
        KEEP(*(.rodata.mmu_ident))
    } > BOOT

    /* THE SELECTION IS INVERTED, as on every enforcing board (arch/arm/chip/mk64f/mk64f.ld):
       the CLOSED KickOS-owned kernel set is named here, in script order, and the app
       catch-alls further down take EVERYTHING ELSE. So an archive nobody listed lands
       app-side, where the identity map still reaches it, rather than kernel-side. The
       `archive:member` colon syntax and not EXCLUDE_FILE: this binutils matches a member
       basename inside EXCLUDE_FILE and not an archive name. */
    .text : AT(ADDR(.text) - KICKOS_ARM64_VA_BASE) ALIGN(8)
    {
        *libkickos_kernel.a:*(.text .text.*)
        *libkickos_arch_armv8a.a:*(.text .text.*)
        *libkickos_chip_virt_arm64.a:*(.text .text.*)
        KICKOS_CODE_DEBRIS_SECTIONS
        *libkickos_kernel.a:*(.rodata .rodata.*)
        *libkickos_arch_armv8a.a:*(.rodata .rodata.*)
        *libkickos_chip_virt_arm64.a:*(.rodata .rodata.*)
        . = ALIGN(8);
    } > KTEXT

    /* Global ctors SPLIT: .init_array keeps ONLY the three archives whose TEXT the kernel
       selectors above also claim (kernel/arch/chip), run early by Reset_Handler; every other
       ctor, libkickos_lib.a's included, lands in .kickos_app_init_array and runs from
       kickos_root_entry with the kernel live. THIS array stays kernel-side, Reset_Handler
       walking it at EL1; the app's is APP-side below, its walker being unprivileged.

       THE SELECTOR LIST IS THE KERNEL TEXT LIST AND NOTHING ELSE. libkickos_lib.a used to
       stand here while the .apptext catch-all took its text app-side: a ctor of that archive's
       would then be walked at EL1 at boot as a privileged fetch of an EL0 page.

       (READONLY) because the INPUT sections carry SHF_WRITE, and the leaf this lands in is
       read-execute: nothing writes the array, and without the keyword the kernel's own code
       segment is flagged writable as well as executable. */
    .init_array (READONLY) : AT(ADDR(.init_array) - KICKOS_ARM64_VA_BASE) ALIGN(8)
    {
        __init_array_start = .;
        KEEP(*libkickos_kernel.a:*(SORT(.init_array.*) .init_array))
        KEEP(*libkickos_arch_armv8a.a:*(SORT(.init_array.*) .init_array))
        KEEP(*libkickos_chip_virt_arm64.a:*(SORT(.init_array.*) .init_array))
        __init_array_end = .;
    } > KTEXT

    /* .fini_array, app side, ASSERTed empty below; nothing walks it. (READONLY) for the reason
       above: it lands in KTEXT, so without the keyword one SHF_WRITE input would flag the
       kernel's own code segment writable as well as executable. */
    .kickos_app_fini_array (READONLY) :
        AT(ADDR(.kickos_app_fini_array) - KICKOS_ARM64_VA_BASE) ALIGN(8)
    {
        __kickos_app_fini_array_start = .;
        KEEP(*(SORT(.fini_array.*)))
        KEEP(*(.fini_array))
        __kickos_app_fini_array_end = .;
    } > KTEXT

    /* The template the kernel copies each thread's TLS block FROM, so it is read by kernel
       text and stays in the kernel's half; the blocks themselves are arena carves and move
       with the stacks in T6. */
    KICKOS_TLS_TEMPLATE(KTEXT)

    /* THE APP'S CTOR LIST, and app-side because its WALKER is: kickos_root_entry runs
       unprivileged, so an array in the kernel's half is a read EL0 cannot make. Nothing
       writes it at run time, so it belongs in the app's rom window rather than beside .got
       in the writable one. FIRST in APPTEXT, so the window's low bound below can name the
       region origin and stay right whether or not this section is empty.

       (READONLY) because the INPUT sections carry SHF_WRITE: without it the app's code
       segment is flagged writable AND executable, which is what keeping .got out of it
       exists to avoid. It changes no runtime permission, the level-2 code block and the
       per-process leaf both being read-execute. */
    .kickos_app_init_array (READONLY) : ALIGN(8)
    {
        __kickos_app_init_array_start = .;
        KEEP(*(SORT(.init_array.*)))
        KEEP(*(.init_array))
        __kickos_app_init_array_end = .;
    } > APPTEXT

    /* THE APP'S TEXT, at the address it will be fetched from. No AT(): VMA == LMA, so a
       per-process root will name these bytes by the same value the app was linked at.
       Everything the kernel selectors above did not claim lands here, the toolchain
       runtime (libc/libgcc/libstdc++) and any unlisted archive included.

       .apptrap is the EL0 trap leaf (arch/arm64/armv8a/switch.S). Its own section name
       because the archive that holds it is kernel-side and `.text.*` would capture it up
       there; the kernel reaches the same two numbers through its own karch_syscall.

       The .got is the app's alone. AArch64 reaches a WEAK undefined symbol GOT-indirect
       (R_AARCH64_ADR_GOT_PAGE), which is how newlib's __libc_fini_array and
       __call_exitprocs reach __fini_array_start/__libc_fini; the kernel side emits no GOT
       reference at all under -mcmodel=large, which is what makes one .got placeable.
       --orphan-handling=error refuses it unplaced. */
    .apptext : ALIGN(8)
    {
        *(.apptrap)
        *(.text .text.*)
        *(.rodata .rodata.*)
        /* DWARF EH unwind + landing-pad tables for the full-C++ opt-in; a plain
           freestanding app is built -fno-exceptions, which leaves the section holding just
           the terminator. Under -nostartfiles the boot-time registration is
           Reset_Handler's, through a WEAK __register_frame (chip_virt_arm64.cc), standing
           in for crtbegin's frame_dummy. KEEP so --gc-sections cannot drop it, contiguous,
           with the LONG(0) terminator standing in for crtend's __FRAME_END__.

           APP-SIDE, AND NOT A CHOICE: an FDE reaches the code it describes with a 32-bit
           PC-relative field (R_AARCH64_PREL32), so the table has to sit in the same half as
           that code. Only the toolchain archives emit either section here, and those are
           app-side, so one table still covers everything that unwinds. */
        *(.gcc_except_table .gcc_except_table.*)
        . = ALIGN(8);
        __eh_frame_start = .;
        KEEP(*(.eh_frame))
        LONG(0)                /* zero-length CIE terminator: crtend is not linked
                                  under -nostartfiles, so the walk needs it here */
        . = ALIGN(8);
    } > APPTEXT

    /* The high alias of the load address, so the C runtime dereferences no low address.
       Every kernel section carries the same VMA-to-LMA delta, so this IS _sdata and the
       copy Reset_Handler runs is a self-copy, as it was under the identity link. */
    _sidata = LOADADDR(.data) + KICKOS_ARM64_VA_BASE;

    /* The kernel's writable state. The 2 MiB boundary the level-2 map splits on is the
       APPDATA origin now, not an ALIGN here: with an explicit `> RAM` the address comes
       from the region's own pointer and a location-counter assignment moves nothing. */
    .data : AT(ADDR(.data) - KICKOS_ARM64_VA_BASE) ALIGN(8)
    {
        _sdata = .;
        *libkickos_kernel.a:*(.data .data.*)
        *libkickos_arch_armv8a.a:*(.data .data.*)
        *libkickos_chip_virt_arm64.a:*(.data .data.*)
        . = ALIGN(8);
        _edata = .;
    } > RAM

    .bss (NOLOAD) : AT(ADDR(.bss) - KICKOS_ARM64_VA_BASE) ALIGN(8)
    {
        _sbss = .;
        *libkickos_kernel.a:*(.bss .bss.*)
        *libkickos_arch_armv8a.a:*(.bss .bss.*)
        *libkickos_chip_virt_arm64.a:*(.bss .bss.*)
        *libkickos_kernel.a:*(COMMON)
        *libkickos_arch_armv8a.a:*(COMMON)
        *libkickos_chip_virt_arm64.a:*(COMMON)
        . = ALIGN(8);
        _ebss = .;
    } > RAM

    /* THE APP'S WRITABLE STATE, identity-linked like its text. VMA == LMA again, so
       nothing is copied into place; .appbss is zeroed by Reset_Handler off the two symbols
       below, the way _sbss/_ebss are for the kernel's. */
    .appdata : ALIGN(8)
    {
        /* A static non-PIE link fills these at link time, so they are never written at
           run time. They sit here rather than with app text because a writable input
           section makes the whole output section writable, and app text is mapped
           executable: W and X on one mapping is what the code/data split exists to
           prevent. */
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

    /* The newlib heap is the app's: user/src/newlib_sbrk.cc names _kickos_heap_start from
       app text, and app text cannot adrp to the kernel's half. */
    .userheap (NOLOAD) :
    {
        . = ALIGN(8);
        _kickos_heap_start = .;
        . += KICKOS_USER_HEAP_SIZE;
        _kickos_heap_limit = .;
    } > APPDATA

    /* Frames, page-aligned because a descriptor's output address is. The FrameAllocator
       writes its bitmap into the first frames, so this is real RAM and not just symbols. */
    .framepool (NOLOAD) : AT(ADDR(.framepool) - KICKOS_ARM64_VA_BASE) ALIGN(4096)
    {
        __kickos_frame_pool_start = .;
        . += KICKOS_ARM64_FRAME_POOL_SIZE;
        __kickos_frame_pool_end = .;
    } > RAM

    /* The kernel address that corresponds to OUTPUT address 0 for the bytes of that carve,
       so the frame allocator can answer in the output addresses a descriptor records. It
       says nothing about the rest of physical memory, which is the backend's own map.
       A DELTA and not the pool's output address: the latter is a low value, and a low
       absolute symbol is out of adrp's reach from the high half, so kernel code cannot
       materialise one at all. */
    __kickos_frame_pool_delta = KICKOS_ARM64_VA_BASE;

    /* From here up to _kernel_stack_size below _estack is the arch_ram_alloc pool
       (runtime bump-allocated, so symbols only, no section to reserve). */
    . = ALIGN(16);
    __kickos_ram_start = .;
    __kickos_ram_end = _estack - _kernel_stack_size;

    /* Chip memory extents read by the descriptor-less user-pointer admission
       (arch_ram_common.cc). Symbols only, no layout effect.
       The code block is declared as the ROM range even though it is DRAM, because that is
       the range's meaning to the admission: readable, and refused as an out-pointer. The
       level-2 map marks it read-only at both levels, so admitting it as writable static RAM
       would pass a syscall out-pointer that then faults on the privileged store. The sram
       range therefore starts at the writable boundary, not at the RAM origin. */
    /* The DRAM origin, for the level-2 map in startup.S. Named separately from the
       admission symbols below, which describe PERMISSIONS rather than layout, and PHYSICAL
       because a descriptor's output address is. */
    __kickos_dram_base = KICKOS_ARM64_DRAM_BASE;

    /* The high alias of the DRAM origin, so the identity-linked boot span is inside the
       readable range and every admission bound is a pointer of the same shape. */
    __kickos_rom_start = KICKOS_ARM64_VA_BASE + KICKOS_ARM64_DRAM_BASE;
    __kickos_rom_end = ADDR(.data);
    __kickos_sram_start = ADDR(.data);

    /* THE SECOND ADMITTED WINDOW, the app's own low one. The pair above is one contiguous
       range and the split put the app's strings and buffers outside it, so a syscall taking
       a pointer into app rodata or app .data would be refused with no second extent to
       consult. Weakly declared arch-side, so a chip that carves no such window admits
       nothing extra. */
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

/* THE W^X SPLIT, on the LOAD addresses: the level-2 descriptors name physical output
   addresses, and there is exactly one read-only executable block. Outgrowing either code
   region is reported by ld as a region overflow naming KTEXT or APPTEXT; what these state
   is that the two together end where the writable side begins, which a bad edit of the size
   constants above would silently break. */
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

/* SELECTOR-MISMATCH GUARD, the same one the enforcing boards carry. The inversion captures
   the closed kernel set and lets the app catch-alls take the rest, so a broken selector
   shows up as an EMPTY KERNEL side: kernel writable state silently linked into the app's
   low window, which is the one window EL0 still reaches. An empty app side is
   legitimate (a minimal freestanding app has had its own state --gc-sections'd away), so
   only the kernel side is asserted. */
ASSERT(_ebss > _sbss,
       "KickOS: kernel .bss empty: closed-set archive selector matched nothing (renamed lib?)")

/* Opt-in link-time heap floor (undefined by default). */
#ifdef KICKOS_HEAP_MIN
ASSERT(_kickos_heap_limit - _kickos_heap_start >= KICKOS_HEAP_MIN,
       "KickOS: heap below KICKOS_HEAP_MIN")
#endif
