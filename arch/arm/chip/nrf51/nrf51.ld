/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * Linker script for the Nordic nRF51822, Cortex-M0. This is the runnable armv6m
 * CI/verification target under QEMU (-M microbit).
 *   Flash : 256 KiB @ 0x0000_0000
 *   SRAM  :  32 KiB @ 0x2000_0000   (the QFAC variant; see the MEMORY block below)
 */

#include <chip_layout.h>
#include <cortexm.ld.h>

ENTRY(Reset_Handler)

/* 32 KiB AND NOT THE 16 A BBC micro:bit v1 HAS. This board is the fleet's only executable
   armv6m target, and 16 KiB cannot run what every other board runs. The nRF51822 ships in a
   32 KiB variant (QFAC).

   QEMU HAS TO BE TOLD. Its microbit machine defaults to 16 KiB and ignores -m; the size is a
   QOM property, so every invocation passes -global nrf51-soc.sram-size=<the RAM length>, which
   cmake/kickos.cmake's kickos_qemu_machine reads from the chip file's RAM link region. Without
   it an image linked here locks up on its first push, before any vector table is live, as
   "can't escalate 3 to HardFault". */
MEMORY
{
    KICKOS_LINK_FLASH
    KICKOS_LINK_RAM
}

_estack = ORIGIN(RAM) + LENGTH(RAM);
_kernel_stack_size = 2K;

SECTIONS
{
    .text :
    {
        KEEP(*(.isr_vector))
        KICKOS_CORTEXM_TEXT
    } > FLASH

    KICKOS_CORTEXM_CTORS
    KICKOS_CORTEXM_EH
    KICKOS_CORTEXM_EXIDX
    KICKOS_CORTEXM_DATA()
    KICKOS_CORTEXM_DISCARD
}

KICKOS_CORTEXM_ASSERTS()
KICKOS_APP_CODE_WINDOW_NONE()

#include <boot_arena.ld.h>
KICKOS_BOOT_ARENA_ASSERT(__kickos_ram_start, __kickos_ram_end)
KICKOS_POOL_ARENA_ASSERT(__kickos_ram_start, __kickos_ram_end)
