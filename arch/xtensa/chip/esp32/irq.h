// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// ESP32 (WROOM-32) interrupt-source numbers.
//
// THREE distinct numbering spaces meet on this core (the Xtensa interrupt-matrix
// quirk):
//  1. Peripheral interrupt SOURCES (DPORT interrupt matrix inputs). The matrix
//     routes a source to a CPU interrupt via the source's DPORT map register.
//  2. CPU interrupt NUMBERS (the Xtensa core's 0..31 lines). Level/type is fixed
//     per number by the core; a matrix-routed source targets an external line.
//     The internal lines (timer 6, software 7) are owned by the arch/xtensa/lx6
//     layer (arch_xtensa.cc CCOMPARE0_INT / SW_INT_L1) and are mirrored here for
//     reference only, NOT redefined as chip constants.
//  3. Logical kernel IRQ lines (irq_table index / irq_claim). A software
//     controller decoupled from the physical Xtensa interrupts; chip-local.

#ifndef KICKOS_ARCH_XTENSA_CHIP_ESP32_IRQ_H
#define KICKOS_ARCH_XTENSA_CHIP_ESP32_IRQ_H

namespace kickos::esp32::irq
{
    // Peripheral interrupt sources (DPORT interrupt-matrix inputs). TRM Table 8.3-1:
    // source 34 is UART_INTR, its PRO_CPU map register DPORT_PRO_UART_INTR_MAP_REG at
    // 0x3FF0018C (TRM 12.4).
    enum periph_src
    {
        UART0_SRC = 34,
    };

    // Target CPU interrupt numbers. TRM Table 8.3-2: 13 is Peripheral, Level-Triggered,
    // priority 1, so it is C-handleable through the level-1 vector (bit 13 is in
    // KICKOS_L1_INT_MASK) and no other in-tree source maps to it. The two internal lines
    // are arch-layer owned (see file header), mirrored for context.
    enum cpu_int
    {
        UART0_CPU_INT = 13,
        // The cross-core doorbell. Must be Level-Triggered at priority 1, which Table 8.3-2
        // fixes per number and no register can change: a level input's pending state follows
        // the trigger register, so clearing the trigger is the whole acknowledgement. The
        // number is free within that set: 0, 1, 2, 3, 4, 5, 8, 9, 12, 17 and 18 all qualify
        // and 13 is taken above.
        DOORBELL_CPU_INT = 12,
        CCOMPARE0_INT = 6, // arch/xtensa/lx6-owned (timer); reference only
        SW_INT_L1 = 7,     // arch/xtensa/lx6-owned (local injection); reference only
    };

    // Which core takes a pinned device line (freeze N3): every route names its core.
    enum dev_core
    {
        CONSOLE_CORE = 0, // the console is the primary's: kmain's own output path
    };

    // Logical kernel IRQ lines. Kept clear of the selftest's injected lines (6..16, from
    // KICKOS_SELFTEST_IRQ_BASE) and the bench line (20).
    enum kernel_line
    {
        CONSOLE_TX_LINE = 30, // UART0 TX-empty drain ISR binding
    };
}

#endif
