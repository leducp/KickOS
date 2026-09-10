/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * Linker script for the Raspberry Pi RP2350 (Cortex-M33) executing in place (XIP)
 * from external QSPI flash at 0x1000_0000. This board carries 16 MiB QSPI.
 *
 * BOOT (datasheet 5.9.5): the RP2350 bootrom sets up QSPI XIP itself, then scans the
 * first 4 KiB of the image for an IMAGE_DEF block-loop; absent an
 * ENTRY_POINT/VECTOR_TABLE item it reads the initial SP/PC from the vector table at the
 * image base. So the vector table is PINNED at the flash origin (0x1000_0000) and the
 * IMAGE_DEF block (startup.S, .image_def) is homed immediately after it, both inside the
 * 4 KiB scan window: the vector VMA is fixed by section placement rather than by
 * whatever the linker packs ahead of it.
 *
 * SRAM (datasheet 2.2.3): 520 KiB total. Main RAM here is the striped lower 512 KiB
 * (0x2000_0000..0x2008_0000); the two 4 KiB non-striped banks at
 * 0x2008_0000/0x2008_1000 sit outside it, available for per-core stacks.
 *
 * ENFORCEMENT (KICKOS_HAVE_MPU): the armv8-m / PMSAv8 MPU backend
 * (arch/arm/common/arch_arm_pmsav8.cc). PMSAv8 uses RBAR/RLAR base+limit at a 32-byte
 * granule (NON-pow2, like SYSMPU), so the app-data window is two 32-byte-aligned symbols
 * and needs no pow2 ALIGN of the kind the v6-M/v7-M PMSA chips carry (rp2040.ld). The
 * writable state is split KERNEL vs app as in mk64f.ld, the other byte-granular backend.
 * A non-enforcement build (KICKOS_HAVE_MPU=0) reserves no window.
 */

#include <sections.ld.h>
#include <app_heap.ld.h>

ENTRY(Reset_Handler)

/* Text executes from XIP flash and data lives in SRAM, so an own-image node takes a slice of
   each: the same slice arithmetic over different bases and strides
   (arch/common/amp_partition.ld.h). The region every node writes sits in SRAM and never in
   flash, because LDREX/STREX succeed only over 0x2000_0000..0x2008_2000 (datasheet 2.1.6.2)
   and the rendezvous cells are the one thing every node read-modify-writes.

   Two nodes plus the shared region are sized to end at 0x2008_0000, the striped SRAM end,
   leaving the two 4 KiB non-striped banks unclaimed for per-core stacks. */
#if KICKOS_AMP_OWN_IMAGE
#include <amp_partition.ld.h>
#define KICKOS_RP2350_FLASH_BASE \
    KICKOS_AMP_SLICE_BASE(KICKOS_AMP_TEXT_BASE, KICKOS_AMP_TEXT_SHARE)
#define KICKOS_RP2350_FLASH_SIZE KICKOS_AMP_TEXT_SHARE
#define KICKOS_RP2350_RAM_BASE \
    KICKOS_AMP_SLICE_BASE(KICKOS_AMP_PARTITION_BASE, KICKOS_AMP_NODE_SHARE)
#define KICKOS_RP2350_RAM_SIZE   KICKOS_AMP_NODE_SHARE
#else
#define KICKOS_RP2350_FLASH_BASE 0x10000000
#define KICKOS_RP2350_FLASH_SIZE 16M
#define KICKOS_RP2350_RAM_BASE   0x20000000
#define KICKOS_RP2350_RAM_SIZE   512K
#endif

MEMORY
{
    FLASH (rx)  : ORIGIN = KICKOS_RP2350_FLASH_BASE, LENGTH = KICKOS_RP2350_FLASH_SIZE
    RAM   (rwx) : ORIGIN = KICKOS_RP2350_RAM_BASE,   LENGTH = KICKOS_RP2350_RAM_SIZE
#if KICKOS_AMP_OWN_IMAGE
    /* Above every node's SRAM slice, so no image's own allocations can reach it. */
    SHARED (rw) : ORIGIN = KICKOS_AMP_SHARED_BASE, LENGTH = KICKOS_AMP_SHARED_SIZE
#endif
}

_estack = ORIGIN(RAM) + LENGTH(RAM);
_kernel_stack_size = 8K;

/* App-owned globals get their own MPU region so an unprivileged thread reaches its own
   .data/.bss but not the kernel's. PMSAv8 is 32-byte granular with an arbitrary base
   (no pow2 rule), so the window base need only be 32-byte aligned and the size a
   32-byte multiple; the app data is padded up to _appdata_size for a fixed grant.
   Overridable (a full-C++ build needs a larger heap + libstdc++/unwind window). */
#if KICKOS_HAVE_MPU
#ifndef KICKOS_APPDATA_SIZE
#define KICKOS_APPDATA_SIZE 128K
#endif
_appdata_size = KICKOS_APPDATA_SIZE;
#endif

SECTIONS
{
    /* .text VMA is PINNED to the flash origin: the vector table is the first thing
       here (bootrom reads SP/PC from image base), and the IMAGE_DEF block follows
       it (still within the bootrom's first-4-KiB scan window). */
    .text ORIGIN(FLASH) :
    {
        KEEP(*(.isr_vector))   /* vector table at 0x1000_0000 */
        KEEP(*(.image_def))    /* boot IMAGE_DEF block, right after the vectors */
        KEEP(*(.node_vector))  /* node 1's table (node_vectors.S), VTOR-aligned */
        *(.text .text.*)
        *(.rodata .rodata.*)
        *(.glue_7) *(.glue_7t)
        KICKOS_CODE_DEBRIS_SECTIONS
        KEEP(*(.init))
        KEEP(*(.fini))
        . = ALIGN(4);
    } > FLASH

    /* EHABI unwind instructions + LSDA (read-only), homed in the flash code window
       right after .text so they never orphan into the vector slot. M33 is EHABI;
       libstdc++ may emit .gcc_except_table even on EHABI, so it is homed too. */
    .ARM.extab :
    {
        *(.ARM.extab* .gnu.linkonce.armextab.*)
    } > FLASH

    .gcc_except_table :
    {
        *(.gcc_except_table .gcc_except_table.*)
    } > FLASH

    /* Global ctors SPLIT: .init_array keeps ONLY the closed KickOS-owned set
       (kernel/arch/chip/lib), run early by Reset_Handler; other (app/lib) ctors
       land in .kickos_app_init_array and run from root_entry with the kernel live. */
    .init_array :
    {
        __init_array_start = .;
        KEEP(*libkickos_kernel.a:*(SORT(.init_array.*) .init_array))
        KEEP(*libkickos_arch_armv7m.a:*(SORT(.init_array.*) .init_array))
        KEEP(*libkickos_chip_rp2350.a:*(SORT(.init_array.*) .init_array))
        KEEP(*libkickos_lib.a:*(SORT(.init_array.*) .init_array))
        __init_array_end = .;
    } > FLASH

    /* .fini_array, app side, ASSERTed empty below; nothing walks it. See
       arch/arm/chip/stm32f411/stm32f411.ld. */
    .kickos_app_fini_array :
    {
        __kickos_app_fini_array_start = .;
        KEEP(*(SORT(.fini_array.*)))
        KEEP(*(.fini_array))
        __kickos_app_fini_array_end = .;
    } > FLASH

    KICKOS_TLS_TEMPLATE(FLASH)

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
       {dst,len}. */
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
        /* Zero the whole granted window past the loaded .appdata (the pre-.appbss
           alignment gap, .appbss, and the pad) so no stale read-back in the app grant. */
        LONG(__kickos_appdata_load_end); LONG(__kickos_appdata_end - __kickos_appdata_load_end);
#endif
        LONG(ADDR(.bss));    LONG(SIZEOF(.bss));
        __kickos_zero_table_end = .;
    } > FLASH

    _sidata = LOADADDR(.data);

#if KICKOS_HAVE_MPU
    /* Enforcement layout (mirrors mk64f.ld, the other byte-granular backend). Writable
       state is split KERNEL vs app with an INVERTED selector: the CLOSED KickOS-owned
       set (kernel/arch/chip/lib) is captured FIRST into kernel .data/.bss; the appdata
       catch-alls that follow take EVERYTHING ELSE (app objects + toolchain runtime), so
       an unmatched archive lands app-side (reachable), never kernel-side (faults). The
       `archive:member` colon syntax is used (this binutils does not match archive
       members inside EXCLUDE_FILE). PMSAv8 is 32-byte granular with an arbitrary base,
       so the app window needs no pow2 base; it is simply placed above kernel data. */
    .data : ALIGN(4)
    {
        _sdata = .;
        *libkickos_kernel.a:*(.data .data.*)
        *libkickos_arch_armv7m.a:*(.data .data.*)
        *libkickos_chip_rp2350.a:*(.data .data.*)
        *libkickos_lib.a:*(.data .data.*)
        . = ALIGN(4);
        _edata = .;
    } > RAM AT > FLASH

    .bss (NOLOAD) : ALIGN(4)
    {
        _sbss = .;
        *libkickos_kernel.a:*(.bss .bss.*)
        *libkickos_arch_armv7m.a:*(.bss .bss.*)
        *libkickos_chip_rp2350.a:*(.bss .bss.*)
        *libkickos_lib.a:*(.bss .bss.*)
        *libkickos_kernel.a:*(COMMON)
        *libkickos_arch_armv7m.a:*(COMMON)
        *libkickos_chip_rp2350.a:*(COMMON)
        *libkickos_lib.a:*(COMMON)
        . = ALIGN(4);
        _ebss = .;
    } > RAM

    /* App-granted RW window: all remaining .data/.bss/COMMON (app + runtime). .appdata
       is loaded from flash; .appbss + the pad are zeroed (no stale read-back). */
    .appdata : ALIGN(32)
    {
        __kickos_appdata_start = .;
        *(.data .data.*)
        . = ALIGN(8);
        __kickos_appdata_load_end = .;
    } > RAM AT > FLASH

    /* Pin the base to the window (start + SIZEOF .appdata), NOT the location counter: an
       EMPTY .appdata (an app with no initialized globals) does not carry the section
       ALIGN forward, so a defaulted .appbss would fall back to kernel-.bss-end. */
    .appbss (__kickos_appdata_start + SIZEOF(.appdata)) (NOLOAD) : ALIGN(4)
    {
        *(.bss .bss.* COMMON)
        . = ALIGN(4);
        _appdata_used_end = .;
        /* ld evaluates ASSERTs after layout, so a bare `. = start + size` that moved
           backwards would fail the link ahead of the overflow ASSERT below. MAX keeps
           that ASSERT the thing that prints. */
        . = MAX(., __kickos_appdata_start + _appdata_size); /* pad the granted window */
        __kickos_appdata_end = .;
        /* The newlib heap IS the window pad: [ALIGN(used_end,8), window end). That ALIGN is
           relative to this section's start, which is why .appdata closes on ALIGN(8);
           app_heap.ld.h holds the rule and the file-scope ASSERT that reads it back. */
        _kickos_heap_start = ALIGN(_appdata_used_end, 8);
        _kickos_heap_limit = __kickos_appdata_end;
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

#if !KICKOS_HAVE_MPU
    /* Non-enforcement build: no .appdata window, so the newlib heap is an explicit
       section carved from RAM before the thread arena. */
    .userheap (NOLOAD) :
    {
        . = ALIGN(8);
        _kickos_heap_start = .;
        . += KICKOS_USER_HEAP_SIZE;
        _kickos_heap_limit = .;
    } > RAM
#endif

    . = ALIGN(32);
    __kickos_ram_start = .;
    __kickos_ram_end = _estack - _kernel_stack_size;

    /* Chip memory extents read by the non-enforcing user-pointer admission
       (arch_ram_common.cc). Symbols only, no layout effect. */
    __kickos_rom_start = ORIGIN(FLASH);
    __kickos_rom_end = ORIGIN(FLASH) + LENGTH(FLASH);
    __kickos_sram_start = ORIGIN(RAM);

    /* App code region for unprivileged threads (RX): the whole flash window. Lets an
       unprivileged thread fetch its code + read flash rodata (XIP). Consumed by the
       (deferred) PMSAv8 backend; kept here so the enforcement seam has the bounds. */
    __kickos_code_start = ORIGIN(FLASH);
    __kickos_code_end = ORIGIN(FLASH) + LENGTH(FLASH);

#if KICKOS_AMP_OWN_IMAGE
    /* The region every node of the partition writes.

       NOLOAD for correctness and not size: a region carrying bytes is written at load time,
       which on this part is a flash program, and by then a peer may already have published
       into it.

       Outside .bss, so no image's C runtime clears it; arch_amp_shared_zero, from the primary's
       Reset_Handler, does that once, before any peer is released.

       The order below must match virt_arm64.ld's: it is a property of the partition, not of
       the chip. */
    .amp_shared (NOLOAD) : ALIGN(64)
    {
        __kickos_amp_shared_start = .;
        __kickos_amp_cells_start = .;
        KEEP(*(.amp_shared.cells.request))
        KEEP(*(.amp_shared.cells.answer))
        KEEP(*(.amp_shared.cells.served))
        KEEP(*(.amp_shared.affinity))
        KEEP(*(.amp_shared.affinity_seated))
        __kickos_amp_cells_end = .;
        . = ALIGN(64);
        __kickos_amp_window_start = .;
        KEEP(*(.amp_shared.window))
        __kickos_amp_window_end = .;
        . = ALIGN(64);
        __kickos_amp_counts_start = .;
        KEEP(*(.amp_shared.counts))
        __kickos_amp_counts_end = .;
        /* A chip backend's own partition-wide publication. This part makes one, so the
           assert below requires it; a chip that publishes nothing places nothing here and
           owes no such assert. */
        __kickos_amp_chip_start = .;
        KEEP(*(.amp_shared.chip))
        __kickos_amp_chip_end = .;
        /* Last, and with no ALIGN ahead of it: this section is empty unless
           KICKOS_AMP_DIAG_REPORT is on, and anywhere else it would move every object
           below it the moment a diagnostic is turned on. */
        KEEP(*(.amp_shared.diag))
        __kickos_amp_shared_end = .;
    } > SHARED
#endif

    /DISCARD/ : { *(.ARM.attributes) }

    KICKOS_NONALLOC_SECTIONS
}

KICKOS_STATIC_RELOC_ASSERT()
KICKOS_TLS_ALIGN_ASSERT()
KICKOS_TLS_FIT_ASSERT()

ASSERT(__kickos_ram_start <= __kickos_ram_end,
       "KickOS: static data overflows RAM (no room for the user-RAM pool + stack)")

#include <boot_arena.ld.h>
KICKOS_BOOT_ARENA_ASSERT(__kickos_ram_start, __kickos_ram_end)

KICKOS_POOL_ARENA_ASSERT(__kickos_ram_start, __kickos_ram_end)
ASSERT(__kickos_app_fini_array_end == __kickos_app_fini_array_start,
       "KickOS: .fini_array is non-empty: a global with a non-trivial destructor? Static destructors are not supported; nothing would ever run it")

#if KICKOS_HAVE_MPU
/* App globals must fit the granted window. If this trips, raise KICKOS_APPDATA_SIZE. */
KICKOS_APP_HEAP_ALIGN_ASSERT(_kickos_heap_start, _appdata_used_end)
ASSERT(_appdata_used_end <= __kickos_appdata_start + _appdata_size,
       "KickOS: app .data/.bss overflow _appdata_size (raise KICKOS_APPDATA_SIZE)")
/* Lower bound: an empty .appdata dropping its ALIGN would place .appbss below the base. */
ASSERT(ADDR(.appbss) >= __kickos_appdata_start,
       "KickOS: .appbss placed below the app-data window base (empty .appdata alignment-drop regression)")
/* Selector-mismatch guard: the inverted scheme leaves an EMPTY KERNEL side if the
   closed-set archive selector matched nothing (renamed lib?). An empty .appdata is
   legitimate (a minimal app), so the loud signal is an empty kernel .bss. */
ASSERT(_ebss > _sbss,
       "KickOS: kernel .bss empty: closed-set archive selector matched nothing (renamed lib?)")
#endif

/* Opt-in link-time heap floor (undefined by default). */
#ifdef KICKOS_HEAP_MIN
ASSERT(_kickos_heap_limit - _kickos_heap_start >= KICKOS_HEAP_MIN,
       "KickOS: heap below KICKOS_HEAP_MIN")
#endif

/* The vector table (and thus the initial SP/PC the bootrom reads) must sit at the
   flash image base, and the IMAGE_DEF block must be within the first 4 KiB. */
ASSERT(ADDR(.text) == ORIGIN(FLASH),
       "KickOS: RP2350 vector table must be at the flash image base 0x1000_0000")
#if KICKOS_AMP_OWN_IMAGE
/* --- The placement contract, as a refusal ----------------------------------------------
 * The bounds below are the partition's own constants and never this script's regions: an
 * assert reading ORIGIN(SHARED) against LENGTH(SHARED) passes wherever the region is
 * repointed, which is the one edit it exists to catch.
 */
ASSERT(ADDR(.amp_shared) == KICKOS_AMP_SHARED_BASE,
       "KickOS: the AMP shared region is not at the address the partition derives. Every node's script computes KICKOS_AMP_PARTITION_BASE + KICKOS_AMP_NODES * KICKOS_AMP_NODE_SHARE, so a region elsewhere means this image was built against a different geometry than its peers")

ASSERT(__kickos_amp_shared_end - __kickos_amp_shared_start <= KICKOS_AMP_SHARED_SIZE,
       "KickOS: the AMP shared objects overflow KICKOS_AMP_SHARED_SIZE. Raise it in the partition's defconfig, in every node's, or narrow KICKOS_AMP_NODES / KICKOS_AMP_PARTITION_CORES")

/* Two spans, and neither implies the other: a node's text and its data are sliced out of
   different apertures with different strides. */
ASSERT(ORIGIN(RAM) >= KICKOS_AMP_PARTITION_BASE && _estack <= KICKOS_AMP_SHARED_BASE,
       "KickOS: this node's DATA is not inside its own SRAM share of the partition. It starts at KICKOS_AMP_PARTITION_BASE plus its node index times KICKOS_AMP_NODE_SHARE and must end below the region every node writes")

ASSERT(ORIGIN(FLASH) >= KICKOS_AMP_TEXT_BASE
       && ORIGIN(FLASH) + LENGTH(FLASH) <= KICKOS_AMP_TEXT_BASE + KICKOS_AMP_NODES * KICKOS_AMP_TEXT_SHARE,
       "KickOS: this node's TEXT is not inside its own flash share of the partition. It starts at KICKOS_AMP_TEXT_BASE plus its node index times KICKOS_AMP_TEXT_SHARE")

/* Two machine facts, refused here rather than in the shared header. The partition's SRAM must
   end at or below the striped SRAM end (datasheet Table 11), the two 4 KiB banks above it being
   non-striped, in a different power domain and left unclaimed for per-core stacks. And every
   node's text must lie in the first 16 MiB: bootable content must (datasheet 5.1.2.1) and XIP
   window 0 is 16 MiB per chip select (12.14). */
ASSERT(KICKOS_AMP_PARTITION_END <= 0x20080000,
       "KickOS: the AMP partition runs past the RP2350's striped SRAM end at 0x20080000. The two 4 KiB banks above it are non-striped, sit in the SRAM1 power domain, and are reserved for per-core stacks. Narrow KICKOS_AMP_NODE_SHARE or KICKOS_AMP_SHARED_SIZE")

ASSERT(KICKOS_AMP_TEXT_BASE + KICKOS_AMP_NODES * KICKOS_AMP_TEXT_SHARE <= 0x11000000,
       "KickOS: the AMP partition's text runs past the RP2350's first 16 MiB flash window. Bootable content must lie wholly inside it and XIP window 0 ends at 0x11000000. Narrow KICKOS_AMP_TEXT_SHARE")

ASSERT(__kickos_amp_cells_end > __kickos_amp_cells_start,
       "KickOS: .amp_shared.cells is empty: the doorbell backend's rendezvous cells are not placed in the shared region, so each node would rendezvous with itself")

ASSERT(__kickos_amp_window_end > __kickos_amp_window_start,
       "KickOS: .amp_shared.window is empty: kernel/amp/ampwindow.cc's window is not placed in the shared region, so each node would publish into a ring no peer reads")

ASSERT(__kickos_amp_counts_end > __kickos_amp_counts_start,
       "KickOS: .amp_shared.counts is empty: a node sweeping a peer's row would read its own")

ASSERT(__kickos_amp_chip_end > __kickos_amp_chip_start,
       "KickOS: .amp_shared.chip is empty: this chip's clock figure and console-claim owner are not placed in the shared region, so each node would read its own and the two would neither agree on the core clock nor serialise a line against each other")
#endif

ASSERT(g_image_def < ORIGIN(FLASH) + 0x1000,
       "KickOS: RP2350 IMAGE_DEF must be within the bootrom's first-4-KiB scan window")
