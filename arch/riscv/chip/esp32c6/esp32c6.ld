/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * ESP32-C6 linker script: runs from the 512 KiB HP SRAM at 0x4080_0000. Fetch and load/store
 * share one address there (TRM Table 1.4-1), so ONE TOR code window covers unprivileged fetch
 * AND the shared-RX read.
 */

#include <chip_layout.h>
#include <amp_partition.ld.h>

/* The kernel's .data loads where it runs; every image's loads at its template
   (esp32c6.image.ld). */
#define KICKOS_RV32_LOADS_AT_VMA 1
#define KICKOS_RV32_IMAGE_DATA_LOADED 1

MEMORY
{
#if KICKOS_AMP_OWN_IMAGE
    RAM (rwx) : ORIGIN = KICKOS_AMP_IMAGE_BASE, LENGTH = KICKOS_AMP_NODE_SHARE
    SHARED (rw) : ORIGIN = KICKOS_AMP_SHARED_BASE, LENGTH = KICKOS_AMP_SHARED_SIZE
#else
    KICKOS_LINK_RAM
#endif
}

#include <rv32imac.ld.h>

#include <boot_arena.ld.h>
KICKOS_BOOT_ARENA_ASSERT(__kickos_ram_start, __kickos_ram_end)
KICKOS_POOL_ARENA_ASSERT(__kickos_ram_start, __kickos_ram_end)

#if KICKOS_AMP_OWN_IMAGE
ASSERT(KICKOS_AMP_PARTITION_BASE == 0x40800000 && KICKOS_AMP_PARTITION_END <= 0x40880000,
       "KickOS: ESP32-C6 AMP partition must fit its 512 KiB HP SRAM")
#endif
