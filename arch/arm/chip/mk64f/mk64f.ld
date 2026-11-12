/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * Linker script for the NXP MK64FN1M0VLL12 (FRDM-K64F), Cortex-M4F.
 * (K64 Sub-Family Reference Manual K64P144M120SF5RM.)
 *
 *   Flash : 1 MiB  @ 0x0000_0000
 *   SRAM  : SRAM_L 64 KiB @ 0x1FFF_0000, SRAM_U 192 KiB @ 0x2000_0000
 *
 * CRITICAL: the 16-byte Flash Configuration Field at 0x0000_0400 (FSEC/FOPT/
 * FPROT) is loaded at reset. A wrong/zeroed FSEC secures or mass-erase-disables
 * the part (bricks debug access), so .fcf is placed here explicitly and KEPT.
 */

/* No object may straddle 0x2000_0000: an unaligned or multi-word access across it is
 * unsupported (RM 3.5.3.1, ARMv7-M ARM B3.1), and so is a backdoor burst (RM 3.5.3.3). chip.yaml
 * states it as the arena's bank line, which cortexm.ld.h keeps every linked object off. */
#include <chip_layout.h>
#include <cortexm.ld.h>

ENTRY(Reset_Handler)

MEMORY
{
    KICKOS_LINK_FLASH
    KICKOS_LINK_RAM
}

_estack = ORIGIN(RAM) + LENGTH(RAM);
_kernel_stack_size = 8K;

SECTIONS
{
    .isr_vector ORIGIN(FLASH) :
    {
        KICKOS_KERNEL_KEEP(.isr_vector)
    } > FLASH

    /* The vector table (0x198 bytes) fits below the field; code follows above 0x410. */
    .fcf 0x00000400 :
    {
        KICKOS_KERNEL_KEEP(.fcf)
    } > FLASH

    .text :
    {
        KICKOS_CORTEXM_TEXT
    } > FLASH

    KICKOS_CORTEXM_EH
    KICKOS_CORTEXM_CTORS
    KICKOS_CORTEXM_EXIDX
    KICKOS_INIT_TABLES(FLASH)
    KICKOS_CORTEXM_DATA()

    /* The whole flash as the app's RX region, read through the core's code bus. */
    __kickos_code_start = ORIGIN(FLASH);
    __kickos_code_end = ORIGIN(FLASH) + LENGTH(FLASH);

    KICKOS_CORTEXM_DISCARD
}

KICKOS_CORTEXM_ASSERTS()
KICKOS_KERNEL_CTORS_NONE_ASSERT()

#include <boot_arena.ld.h>
KICKOS_BOOT_ARENA_ASSERT(__kickos_ram_start, __kickos_ram_end)
KICKOS_POOL_ARENA_ASSERT(__kickos_ram_start, __kickos_ram_end)
