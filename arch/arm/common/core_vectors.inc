/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * Shared core-exception vector block for the ARM Cortex-M chip startup tables.
 * Slots 0..15 are architecturally fixed and character-identical across the
 * fleet; only the device-IRQ table below slot 15 (and any clock-ISR splice)
 * varies per chip and stays open-coded in each startup.S. A chip invokes the
 * macro for its profile right after its own .section/.align/g_isr_vector header:
 *
 *   KICKOS_CORE_VECTORS_V7M : Cortex-M3/M4 (v7-M): MemManage/BusFault/Usage
 *   KICKOS_CORE_VECTORS_V6M : Cortex-M0/M0+ (v6-M): slots 4/5/6 reserved
 *   KICKOS_CORE_VECTORS_V8M : Cortex-M33 (v8-M): slot 7 = SecureFault
 *
 * The words reference Reset_Handler/SVC_Handler/PendSV_Handler/SysTick_Handler
 * (arch layer), HardFault_Handler (chip), NMI_Handler (file-local, per chip) and
 * _estack (linker). Expanding the macro emits the same relocations the chips
 * spelled by hand, so the linked .isr_vector bytes are unchanged.
 */

#ifndef KICKOS_ARCH_ARM_COMMON_CORE_VECTORS_INC
#define KICKOS_ARCH_ARM_COMMON_CORE_VECTORS_INC

    .macro KICKOS_CORE_VECTORS_V7M
    .word _estack                    /* 0: initial MSP */
    .word Reset_Handler              /* 1: reset */
    .word NMI_Handler                /* 2 */
    .word HardFault_Handler          /* 3 */
    .word HardFault_Handler          /* 4: MemManage */
    .word HardFault_Handler          /* 5: BusFault */
    .word HardFault_Handler          /* 6: UsageFault */
    .word 0                          /* 7 */
    .word 0                          /* 8 */
    .word 0                          /* 9 */
    .word 0                          /* 10 */
    .word SVC_Handler                /* 11: SVCall */
    .word 0                          /* 12: DebugMonitor */
    .word 0                          /* 13 */
    .word PendSV_Handler             /* 14: PendSV */
    .word SysTick_Handler            /* 15: SysTick */
    .endm

    .macro KICKOS_CORE_VECTORS_V6M
    .word _estack                    /* 0: initial MSP */
    .word Reset_Handler              /* 1: reset */
    .word NMI_Handler                /* 2 */
    .word HardFault_Handler          /* 3 */
    .word 0                          /* 4 */
    .word 0                          /* 5 */
    .word 0                          /* 6 */
    .word 0                          /* 7 */
    .word 0                          /* 8 */
    .word 0                          /* 9 */
    .word 0                          /* 10 */
    .word SVC_Handler                /* 11: SVCall */
    .word 0                          /* 12 */
    .word 0                          /* 13 */
    .word PendSV_Handler             /* 14: PendSV */
    .word SysTick_Handler            /* 15: SysTick */
    .endm

    .macro KICKOS_CORE_VECTORS_V8M
    .word _estack                    /* 0: initial MSP */
    .word Reset_Handler              /* 1: reset */
    .word NMI_Handler                /* 2 */
    .word HardFault_Handler          /* 3 */
    .word HardFault_Handler          /* 4: MemManage */
    .word HardFault_Handler          /* 5: BusFault */
    .word HardFault_Handler          /* 6: UsageFault */
    .word HardFault_Handler          /* 7: SecureFault (armv8-m) */
    .word 0                          /* 8 */
    .word 0                          /* 9 */
    .word 0                          /* 10 */
    .word SVC_Handler                /* 11: SVCall */
    .word 0                          /* 12: DebugMonitor */
    .word 0                          /* 13 */
    .word PendSV_Handler             /* 14: PendSV */
    .word SysTick_Handler            /* 15: SysTick */
    .endm

#endif
