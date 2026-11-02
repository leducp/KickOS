/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * Linker script for the QEMU `virt` RISC-V board (RV32IMAC, M-mode bare metal).
 * QEMU (`-bios none -kernel <elf>`) loads the image into DRAM at 0x8000_0000, 2^31-aligned,
 * and enters it in machine mode.
 */

#include <chip_layout.h>

/* QEMU's ELF loader places each segment at its PhysAddr, so .data's bytes really are at its
   LMA and the copy table does real work. */
#define KICKOS_RV32_LOADS_AT_VMA 0

/* QEMU caches no TLB entry for a 4 KiB page a PMP entry splits, so each access to it takes the
   slow path; the kernel's hottest data follows the code window's end. */
#define KICKOS_RV32_CODE_WINDOW_ALIGN 4096

MEMORY
{
    KICKOS_LINK_RAM
}

#include <rv32imac.ld.h>

#include <boot_arena.ld.h>
KICKOS_BOOT_ARENA_ASSERT(__kickos_ram_start, __kickos_ram_end)
KICKOS_POOL_ARENA_ASSERT(__kickos_ram_start, __kickos_ram_end)
