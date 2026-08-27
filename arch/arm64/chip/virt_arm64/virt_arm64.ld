/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * Linker script for the QEMU `virt` AArch64 board (Cortex-A53, EL1 bare metal).
 * QEMU (`-kernel <elf>`) loads each segment at its p_paddr and enters at the ELF entry
 * point with the MMU off. So `.text.init` links where it loads, and every other section
 * links into the high half TTBR1 translates, with an LMA back in DRAM.
 *
 * Chip<->arch contract: define __kickos_ram_start/__kickos_ram_end (the arch_ram_alloc
 * pool), _estack (boot stack top), and _sidata/.../_ebss (C runtime).
 *
 * Chip<->kernel contract under KICKOS_HAVE_ASPACE: __kickos_frame_pool_start/_end and
 * __kickos_frame_pool_delta, the frame carve the FrameAllocator describes.
 *
 * FLAT MEMORY MODEL ONLY. This chip has no region MPU and its arch ships no
 * arch_mpu_apply, so arch/Kconfig selects no HAS_MPU for it and there is no
 * KICKOS_HAVE_MPU .appdata window to carve here.
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

/* The identity-linked span: one page of entry code plus the three 4 KiB translation
 * tables the two TTBRs are pointed at. ld refuses an overflow, so a fourth table page
 * has to raise this. */
#define KICKOS_ARM64_BOOT_SIZE  16K

/* The frame pool the kernel's FrameAllocator describes: page tables and the pages a
 * process holds. Its own carve rather than a slice of the arena, so the bump allocator
 * and the frame allocator never own the same byte and KICKOS_POOL_ARENA_ASSERT keeps
 * modelling the arena exactly. */
#define KICKOS_ARM64_FRAME_POOL_SIZE  1M

MEMORY
{
    BOOT (rx)  : ORIGIN = KICKOS_ARM64_DRAM_BASE,
                 LENGTH = KICKOS_ARM64_BOOT_SIZE
    RAM  (rwx) : ORIGIN = KICKOS_ARM64_VA_BASE + KICKOS_ARM64_DRAM_BASE
                          + KICKOS_ARM64_BOOT_SIZE,
                 LENGTH = KICKOS_ARM64_DRAM_SIZE - KICKOS_ARM64_BOOT_SIZE
}

_estack = ORIGIN(RAM) + LENGTH(RAM);   /* boot/exception stack: top of RAM */
_kernel_stack_size = 64K;              /* boot/exception stack reservation */

/* startup.S reads this to reach a physical address from a high one, in the window before
 * the MMU is on where an absolute literal cannot be translated. */
__kickos_arm64_va_base = KICKOS_ARM64_VA_BASE;

SECTIONS
{
    /* VMA == LMA: the entry is taken untranslated, and a translation-table descriptor
       names its output by physical address. */
    .text.init ORIGIN(BOOT) : ALIGN(8)
    {
        KEEP(*(.text.init))
        . = ALIGN(4096);
        KEEP(*(.rodata.mmu_ident))
    } > BOOT

    .text : AT(ADDR(.text) - KICKOS_ARM64_VA_BASE) ALIGN(8)
    {
        *(.text .text.*)
        KICKOS_CODE_DEBRIS_SECTIONS
        *(.rodata .rodata.*)
        /* The first family in the fleet to link a .got at all. AArch64 reaches a WEAK
           undefined symbol GOT-indirect (R_AARCH64_ADR_GOT_PAGE) where M-profile and
           RV32 resolve it in a literal pool, and this image has eight such references:
           __register_frame, kickos_app_build_stamp, and the six MPU-window symbols a
           flat chip leaves undefined. A static non-PIE link fills the slots at link
           time, so they are constants and belong here rather than in .data.
           --orphan-handling=error refuses them unplaced. */
        *(.got) *(.got.plt)
        /* DWARF EH unwind + landing-pad tables for the full-C++ opt-in; a plain
           freestanding app is built -fno-exceptions, which leaves the section holding just
           the terminator. Under -nostartfiles the boot-time registration is
           Reset_Handler's, through a WEAK __register_frame (chip_virt_arm64.cc), standing
           in for crtbegin's frame_dummy. KEEP so --gc-sections cannot drop it, contiguous,
           with the LONG(0) terminator standing in for crtend's __FRAME_END__. */
        *(.gcc_except_table .gcc_except_table.*)
        . = ALIGN(8);
        __eh_frame_start = .;
        KEEP(*(.eh_frame))
        LONG(0)                /* zero-length CIE terminator: crtend is not linked
                                  under -nostartfiles, so the walk needs it here */
        . = ALIGN(8);
    } > RAM

    /* Global ctors SPLIT: .init_array keeps ONLY the closed KickOS-owned set
       (kernel/arch/chip/lib), run early by Reset_Handler; other (app/lib) ctors
       land in .kickos_app_init_array and run from root_entry with the kernel live. */
    .init_array : AT(ADDR(.init_array) - KICKOS_ARM64_VA_BASE) ALIGN(8)
    {
        __init_array_start = .;
        KEEP(*libkickos_kernel.a:*(SORT(.init_array.*) .init_array))
        KEEP(*libkickos_arch_armv8a.a:*(SORT(.init_array.*) .init_array))
        KEEP(*libkickos_chip_virt_arm64.a:*(SORT(.init_array.*) .init_array))
        KEEP(*libkickos_lib.a:*(SORT(.init_array.*) .init_array))
        __init_array_end = .;
    } > RAM

    /* .fini_array, app side, ASSERTed empty below; nothing walks it. */
    .kickos_app_fini_array : AT(ADDR(.kickos_app_fini_array) - KICKOS_ARM64_VA_BASE) ALIGN(8)
    {
        __kickos_app_fini_array_start = .;
        KEEP(*(SORT(.fini_array.*)))
        KEEP(*(.fini_array))
        __kickos_app_fini_array_end = .;
    } > RAM

    KICKOS_TLS_TEMPLATE(RAM)

    .kickos_app_init_array : AT(ADDR(.kickos_app_init_array) - KICKOS_ARM64_VA_BASE) ALIGN(8)
    {
        __kickos_app_init_array_start = .;
        KEEP(*(SORT(.init_array.*)))
        KEEP(*(.init_array))
        __kickos_app_init_array_end = .;
    } > RAM

    /* The high alias of the load address, so the C runtime dereferences no low address.
       Every section here carries the same VMA-to-LMA delta, so this IS _sdata and the
       copy Reset_Handler runs is a self-copy, as it was under the identity link. */
    _sidata = LOADADDR(.data) + KICKOS_ARM64_VA_BASE;

    /* ALIGN(2M) ON THE SECTION, not a bare `. = ALIGN(2M)` before it: with an explicit
       `> RAM` the address comes from the region's own pointer and a location-counter
       assignment moves nothing, which is exactly the silent no-op this hit first.
       The 2 MiB boundary is what the level-2 map splits on (startup.S): read-only and
       executable below, writable and not above. */
    .data : AT(ADDR(.data) - KICKOS_ARM64_VA_BASE) ALIGN(2M)
    {
        _sdata = .;
        *(.data .data.*)
        . = ALIGN(8);
        _edata = .;
    } > RAM

    .bss (NOLOAD) : AT(ADDR(.bss) - KICKOS_ARM64_VA_BASE) ALIGN(8)
    {
        _sbss = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(8);
        _ebss = .;
    } > RAM

    /* No .appdata window, so the newlib heap is an explicit section carved from RAM
       before the thread arena. */
    .userheap (NOLOAD) : AT(ADDR(.userheap) - KICKOS_ARM64_VA_BASE)
    {
        . = ALIGN(8);
        _kickos_heap_start = .;
        . += KICKOS_USER_HEAP_SIZE;
        _kickos_heap_limit = .;
    } > RAM

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

    /* Chip memory extents read by the non-enforcing user-pointer admission
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

    /DISCARD/ : { *(.comment) *(.note.gnu.build-id) *(.ARM.attributes) }

    KICKOS_NONALLOC_SECTIONS
}

KICKOS_STATIC_RELOC_ASSERT()
KICKOS_TLS_ALIGN_ASSERT()
KICKOS_TLS_FIT_ASSERT()

/* On the LOAD address, which is what the level-2 map splits: the descriptors name
   physical output addresses. */
ASSERT(LOADADDR(.data) - KICKOS_ARM64_DRAM_BASE == 2M,
       "KickOS: .data is not at the 2 MiB boundary the level-2 map splits on; code+rodata has outgrown its block")

ASSERT(__kickos_ram_start <= __kickos_ram_end,
       "KickOS: static data overflows RAM (no room for the user-RAM pool + boot stack)")

#include <boot_arena.ld.h>
KICKOS_BOOT_ARENA_ASSERT(__kickos_ram_start, __kickos_ram_end)

KICKOS_POOL_ARENA_ASSERT(__kickos_ram_start, __kickos_ram_end)
ASSERT(__kickos_app_fini_array_end == __kickos_app_fini_array_start,
       "KickOS: .fini_array is non-empty: a global with a non-trivial destructor? Static destructors are not supported; nothing would ever run it")

/* Opt-in link-time heap floor (undefined by default). */
#ifdef KICKOS_HEAP_MIN
ASSERT(_kickos_heap_limit - _kickos_heap_start >= KICKOS_HEAP_MIN,
       "KickOS: heap below KICKOS_HEAP_MIN")
#endif
