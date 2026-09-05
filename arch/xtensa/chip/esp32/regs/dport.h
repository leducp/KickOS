// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// ESP32-WROOM-32 DPORT register map (ESP32 TRM DPORT / interrupt-matrix chapters):
// CPU clock-period select, the peripheral-source-to-CPU-interrupt matrix, and the four
// inter-CPU interrupt triggers.

#ifndef KICKOS_ARCH_XTENSA_CHIP_ESP32_REGS_DPORT_H
#define KICKOS_ARCH_XTENSA_CHIP_ESP32_REGS_DPORT_H

#include <kickos/chip_mmap.h>

#include <stdint.h>

namespace kickos::esp32::reg::dport
{
    constexpr uintptr_t CPU_PER_CONF = mmap::DPORT_BASE + 0x3Cu;

    // Interrupt-matrix map register for the UART0 source (peripheral source 34).
    // Written with the TARGET CPU interrupt number (see irq::cpu_int::UART0_CPU_INT).
    constexpr uintptr_t PRO_UART_INTR_MAP = mmap::DPORT_BASE + 0x18Cu;

    // CPU_PER_CONF.CPUPERIOD_SEL [1:0]: 0=80 1=160 2=240 MHz.
    constexpr uint32_t CPUPERIOD_SEL_240 = 2;

    // APP_CPU control (TRM v5.8 Registers 12.5 to 12.8, p.247-248). Each is bit 0 with the
    // rest reserved, except CTRL_REG_D, which is a full 32-bit boot address.
    constexpr uintptr_t APPCPU_CTRL_A = mmap::DPORT_BASE + 0x02Cu; // Register 12.5, reset 1
    constexpr uintptr_t APPCPU_CTRL_B = mmap::DPORT_BASE + 0x030u; // Register 12.6, reset 0
    constexpr uintptr_t APPCPU_CTRL_C = mmap::DPORT_BASE + 0x034u; // Register 12.7, reset 0
    constexpr uintptr_t APPCPU_CTRL_D = mmap::DPORT_BASE + 0x038u; // Register 12.8, reset 0
    constexpr uint32_t APPCPU_RESETTING = 1u << 0;
    constexpr uint32_t APPCPU_CLKGATE_EN = 1u << 0; // RESETS CLOSED: the gate must be opened
    constexpr uint32_t APPCPU_RUNSTALL = 1u << 0;

    // Each CPU owns a bank of 69 map registers, one per peripheral source, indexed by the
    // SOURCE number (TRM v5.8 register summary 12.4, p.239-242: PRO 0x104..0x214, APP
    // 0x218..0x328). PRO_UART_INTR_MAP above is pro_intr_map(34).
    constexpr uintptr_t PRO_INTR_MAP_BASE = mmap::DPORT_BASE + 0x104u;
    constexpr uintptr_t APP_INTR_MAP_BASE = mmap::DPORT_BASE + 0x218u;

    constexpr uintptr_t pro_intr_map(uint32_t source)
    {
        return PRO_INTR_MAP_BASE + 4u * source;
    }

    constexpr uintptr_t app_intr_map(uint32_t source)
    {
        return APP_INTR_MAP_BASE + 4u * source;
    }

    // The four inter-CPU triggers, bit 0 each, R/W, reset 0 (TRM v5.8 Register 12.24, p.258),
    // raised as matrix sources 24..27 (Table 8.3-1, p.176). GLOBAL, not per target core: one
    // bit is one source and each CPU's bank routes it independently. NO HARDWARE CLEAR IS
    // DOCUMENTED, and the register carries plain R/W where this manual's self-clearing bits
    // carry R/W/SC.
    constexpr uint32_t CPU_INTR_FROM_CPU_SOURCE_0 = 24u;

    constexpr uintptr_t cpu_intr_from_cpu(uint32_t n)
    {
        return mmap::DPORT_BASE + 0x0DCu + 4u * n;
    }

    constexpr uint32_t CPU_INTR_FROM_CPU_TRIGGER = 1u << 0;

    // Pointing a CPU's map register at any of these DISABLES the source for that CPU: they
    // number the six inputs the matrix drives on neither core (TRM v5.8 8.3.3, p.179: Timer.0,
    // Software, Profiling, Timer.1, Timer.2, Software), and the manual states the choice among
    // them does not change behaviour. 16 is ALSO the reset value of every map register in both
    // banks (the shared map-register diagram, p.262), so an unwritten bank is already sunk.
    constexpr uint32_t INTR_MAP_SINK = 16u;
}

#endif
