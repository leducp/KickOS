/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * Linker script for the i.MX 8M Plus EVK (quad Cortex-A53).
 *
 * DRAM is the part's external DDR window; the chip file carves the first 64 MiB, so a `-m`
 * below that overflows.
 */

#include <armv8a.ld.h>

#include <boot_arena.ld.h>
KICKOS_BOOT_ARENA_ASSERT(__kickos_ram_start, __kickos_ram_end)
KICKOS_POOL_ARENA_ASSERT(__kickos_ram_start, __kickos_ram_end)
