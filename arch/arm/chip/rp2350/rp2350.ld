/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * Linker script for the Raspberry Pi RP2350 (Cortex-M33) executing in place (XIP)
 * from external QSPI flash at 0x1000_0000, whose size is the board file's.
 *
 * BOOT (datasheet 5.9.5): the bootrom sets up QSPI XIP itself, then scans the first 4 KiB of
 * the image for an IMAGE_DEF block-loop; absent an ENTRY_POINT/VECTOR_TABLE item it reads the
 * initial SP/PC from the vector table at the image base. So the vector table is PINNED at the
 * flash origin and the IMAGE_DEF block (startup.S, .image_def) follows it, both inside the
 * 4 KiB scan window.
 *
 * SRAM (datasheet 2.2.3): 520 KiB total. Main RAM here is the striped lower 512 KiB
 * (0x2000_0000..0x2008_0000); the two 4 KiB non-striped banks at
 * 0x2008_0000/0x2008_1000 sit outside it, available for per-core stacks.
 *
 * PMSAv8 is 32-byte granular with an arbitrary base, so the app window needs no pow2 size or
 * base.
 */

#include <chip_layout.h>
#include <cortexm.ld.h>

ENTRY(Reset_Handler)

/* Text executes from XIP flash and data lives in SRAM, so an own-image node takes a slice of
   each: the same slice arithmetic over different bases and strides. The region every node
   writes sits in SRAM and never in flash, because LDREX/STREX succeed only over
   0x2000_0000..0x2008_2000 (datasheet 2.1.6.2) and the rendezvous cells are the one thing every
   node read-modify-writes. */
#if KICKOS_AMP_OWN_IMAGE
#include <amp_partition.ld.h>
#define KICKOS_RP2350_FLASH_BASE \
    KICKOS_AMP_SLICE_BASE(KICKOS_AMP_TEXT_BASE, KICKOS_AMP_TEXT_SHARE)
#define KICKOS_RP2350_FLASH_SIZE KICKOS_AMP_TEXT_SHARE
#define KICKOS_RP2350_RAM_BASE \
    KICKOS_AMP_SLICE_BASE(KICKOS_AMP_PARTITION_BASE, KICKOS_AMP_NODE_SHARE)
#define KICKOS_RP2350_RAM_SIZE   KICKOS_AMP_NODE_SHARE
#endif

MEMORY
{
#if KICKOS_AMP_OWN_IMAGE
    FLASH (rx)  : ORIGIN = KICKOS_RP2350_FLASH_BASE, LENGTH = KICKOS_RP2350_FLASH_SIZE
    RAM   (rwx) : ORIGIN = KICKOS_RP2350_RAM_BASE,   LENGTH = KICKOS_RP2350_RAM_SIZE
    SHARED (rw) : ORIGIN = KICKOS_AMP_SHARED_BASE, LENGTH = KICKOS_AMP_SHARED_SIZE
#else
    KICKOS_LINK_FLASH
    KICKOS_LINK_RAM
#endif
}

_estack = ORIGIN(RAM) + LENGTH(RAM);
_kernel_stack_size = 8K;

#if KICKOS_HAVE_MPU
#ifndef KICKOS_APPDATA_SIZE
#define KICKOS_APPDATA_SIZE 128K
#endif
_appdata_size = KICKOS_APPDATA_SIZE;
#endif

SECTIONS
{
    .text ORIGIN(FLASH) :
    {
        KEEP(*(.isr_vector))
        KEEP(*(.image_def))
        KEEP(*(.node_vector))  /* node 1's table (node_vectors.S), VTOR-aligned */
        KICKOS_CORTEXM_TEXT
    } > FLASH

    KICKOS_CORTEXM_EH
    KICKOS_CORTEXM_CTORS
    KICKOS_CORTEXM_EXIDX
    KICKOS_INIT_TABLES(FLASH)
    KICKOS_CORTEXM_DATA(.appdata : ALIGN(32))

    /* The whole flash window as the app's RX region. */
    __kickos_code_start = ORIGIN(FLASH);
    __kickos_code_end = ORIGIN(FLASH) + LENGTH(FLASH);

#if KICKOS_AMP_OWN_IMAGE
    KICKOS_AMP_SHARED_SECTION(SHARED)
#endif

    KICKOS_CORTEXM_DISCARD
}

KICKOS_CORTEXM_ASSERTS()
KICKOS_KERNEL_CTORS_NONE_ASSERT()

#include <boot_arena.ld.h>
KICKOS_BOOT_ARENA_ASSERT(__kickos_ram_start, __kickos_ram_end)
KICKOS_POOL_ARENA_ASSERT(__kickos_ram_start, __kickos_ram_end)

ASSERT(ADDR(.text) == ORIGIN(FLASH),
       "KickOS: RP2350 vector table must be at the flash image base 0x1000_0000")
ASSERT(g_image_def < ORIGIN(FLASH) + 0x1000,
       "KickOS: RP2350 IMAGE_DEF must be within the bootrom's first-4-KiB scan window")

#if KICKOS_HAVE_MPU
/* Last in .bss puts the ROM slice above the kernel blocks and the panic seat, so a ROM call made
   from either returns to a stack below the slice's limit: the case kickos_rp_rom_call restores
   the caller's limits ahead of SP for, which the bench's reboot captures run. */
PROVIDE(kickos_rp_rom_stack = _ebss - 1024); /* an image that makes no ROM call */
ASSERT(kickos_rp_rom_stack + 1024 == _ebss,
       "KickOS: kickos_rp_rom_stack is no longer the last .bss object, so the bench's ROM calls no longer return to a stack below the slice")
#endif

#if KICKOS_AMP_OWN_IMAGE
KICKOS_AMP_SHARED_ASSERT(ADDR(.amp_shared))
KICKOS_AMP_SHARED_FILLED_ASSERT()
ASSERT(__kickos_amp_chip_end > __kickos_amp_chip_start,
       "KickOS: .amp_shared.chip is empty: this chip's clock figure and console-claim owner are not placed in the shared region, so each node would read its own and the two would neither agree on the core clock nor serialise a line against each other")

/* A node's text and its data are sliced out of different apertures with different strides,
   so neither span implies the other. */
KICKOS_AMP_SLICE_ASSERT(ORIGIN(RAM), _estack, KICKOS_AMP_PARTITION_BASE, KICKOS_AMP_NODE_SHARE)
KICKOS_AMP_SLICE_ASSERT(ORIGIN(FLASH), ORIGIN(FLASH) + LENGTH(FLASH),
                        KICKOS_AMP_TEXT_BASE, KICKOS_AMP_TEXT_SHARE)

/* The partition's SRAM ends at or below the striped SRAM end (datasheet Table 11), and every
   node's text lies in the first 16 MiB: bootable content must (datasheet 5.1.2.1) and XIP
   window 0 is 16 MiB per chip select (12.14). */
ASSERT(KICKOS_AMP_PARTITION_END <= KICKOS_LAYOUT_SRAM_BASE + KICKOS_LAYOUT_SRAM_SIZE,
       "KickOS: the AMP partition runs past the RP2350's striped SRAM end at 0x20080000. The two 4 KiB banks above it are non-striped, sit in the SRAM1 power domain, and are reserved for per-core stacks. Narrow KICKOS_AMP_NODE_SHARE or KICKOS_AMP_SHARED_SIZE")
ASSERT(KICKOS_AMP_TEXT_BASE + KICKOS_AMP_NODES * KICKOS_AMP_TEXT_SHARE
           <= KICKOS_LAYOUT_XIP_FLASH_BASE + KICKOS_LAYOUT_XIP_FLASH_SIZE,
       "KickOS: the AMP partition's text runs past the RP2350's first 16 MiB flash window. Bootable content must lie wholly inside it and XIP window 0 ends at 0x11000000. Narrow KICKOS_AMP_TEXT_SHARE")
#endif
