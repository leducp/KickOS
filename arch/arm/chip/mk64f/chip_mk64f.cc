// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// MK64FN1M0 (FRDM-K64F) chip backend. Register addresses/fields are from the K64
// Sub-Family Reference Manual (K64P144M120SF5RM); hand-rolled (no vendor CMSIS
// pack), consistent with the arch layer's clean-room regs.h.
//
// M1 scope: privilege + SVC only, no hardware MPU. First bring-up runs at the
// MCG FEI reset clock (~20.97 MHz core) -- no PLL/120 MHz setup yet (a follow-up)
// -- and drives the OpenSDA virtual UART (UART0 on PTB16/PTB17). Console is
// polled TX. NOT run in this environment (no board/QEMU model); verified by
// build + image inspection. Flash to the board to confirm UART output.

#include <kickos/arch/arch.h>

#include <stdint.h>

namespace kickos
{
    int kmain(int argc, char** argv);
}

extern "C"
{
    void kickos_armv7m_init(void);

    extern uint32_t _sidata, _sdata, _edata, _sbss, _ebss;
    extern void (*__init_array_start[])();
    extern void (*__init_array_end[])();

    // FEI reset clock: MCGOUTCLK = 32.768 kHz internal ref x 640 FLL factor,
    // SIM_CLKDIV1 OUTDIV1 = /1. UART0 is clocked by this system clock.
    uint32_t SystemCoreClock = 20971520u;
}

namespace
{
    inline volatile uint32_t& r32(uintptr_t a) { return *reinterpret_cast<volatile uint32_t*>(a); }
    inline volatile uint16_t& r16(uintptr_t a) { return *reinterpret_cast<volatile uint16_t*>(a); }
    inline volatile uint8_t& r8(uintptr_t a) { return *reinterpret_cast<volatile uint8_t*>(a); }

    // --- Peripheral registers (refman memory map) ---
    constexpr uintptr_t WDOG_STCTRLH = 0x40052000; // 16-bit
    constexpr uintptr_t WDOG_UNLOCK = 0x4005200E;  // 16-bit
    constexpr uint16_t WDOG_UNLOCK_1 = 0xC520;
    constexpr uint16_t WDOG_UNLOCK_2 = 0xD928;

    constexpr uintptr_t SIM_SCGC4 = 0x40048034; // UART0 = bit 10
    constexpr uintptr_t SIM_SCGC5 = 0x40048038; // PORTB = bit 10
    constexpr uint32_t SCGC4_UART0 = 1u << 10;
    constexpr uint32_t SCGC5_PORTB = 1u << 10;

    // OpenSDA VCOM is PTB16/PTB17. Per the K64 signal-mux table these pins are
    // UART0_RX/UART0_TX at ALT3 (PTB16 has no UART1 option) -- the FRDM-K64F user
    // guide's "UART1" label is a doc typo; UART0 is what the silicon exposes.
    constexpr uintptr_t PORTB_PCR16 = 0x4004A040; // UART0_RX (ALT3)
    constexpr uintptr_t PORTB_PCR17 = 0x4004A044; // UART0_TX (ALT3)
    constexpr uint32_t PCR_MUX_ALT3 = 3u << 8;

    constexpr uintptr_t UART0_BASE = 0x4006A000;
    constexpr uintptr_t UART0_BDH = UART0_BASE + 0x00; // 8-bit
    constexpr uintptr_t UART0_BDL = UART0_BASE + 0x01;
    constexpr uintptr_t UART0_C2 = UART0_BASE + 0x03;
    constexpr uintptr_t UART0_S1 = UART0_BASE + 0x04;
    constexpr uintptr_t UART0_D = UART0_BASE + 0x07;
    constexpr uintptr_t UART0_C4 = UART0_BASE + 0x0A;
    constexpr uint8_t C2_TE = 1u << 3;
    constexpr uint8_t C2_RE = 1u << 2;
    constexpr uint8_t S1_TDRE = 1u << 7;

    void wdog_disable()
    {
        // WDOG resets the part ~238 ms after reset if left enabled, so this runs
        // first (RM 24.3.2: the unlock must also complete within 256 bus cycles
        // of reset). The two unlock keys must land within 20 bus cycles of each
        // other (RM 24.3.1) -- emit both stores back-to-back in ONE asm block so
        // an unoptimized (-O0) build cannot insert a helper call between them
        // (a non-inlined store helper would blow the 20-cycle budget).
        volatile uint16_t* unlock = reinterpret_cast<volatile uint16_t*>(WDOG_UNLOCK);
        uint32_t k1 = WDOG_UNLOCK_1;
        uint32_t k2 = WDOG_UNLOCK_2;
        __asm volatile("strh %1, [%0]\n\t"
                       "strh %2, [%0]"
                       ::"r"(unlock), "r"(k1), "r"(k2) : "memory");
        // STCTRLH := reset value 0x01D3 with WDOGEN cleared, keeping ALLOWUPDATE
        // and the reset-1 reserved bit 8 (matches NXP SystemInit; 0x0010 would
        // clear that reserved bit -- pointless risk on never-run silicon).
        r16(WDOG_STCTRLH) = 0x01D2;
    }

    void enable_fpu()
    {
        volatile uint32_t* cpacr = reinterpret_cast<volatile uint32_t*>(0xE000ED88);
        *cpacr |= (0xFu << 20); // CP10/CP11 full access
        __asm volatile("dsb" ::: "memory");
        __asm volatile("isb" ::: "memory");
    }

    void uart0_init()
    {
        r32(SIM_SCGC5) |= SCGC5_PORTB; // clock PORTB
        r32(SIM_SCGC4) |= SCGC4_UART0; // clock UART0
        r32(PORTB_PCR16) = PCR_MUX_ALT3;
        r32(PORTB_PCR17) = PCR_MUX_ALT3;

        r8(UART0_C2) = 0; // disable TX/RX while configuring
        // baud = clk / (16 x (SBR + BRFA/32)); 20.97 MHz, 115200 -> SBR 11, BRFA 12.
        uint32_t sbr = 11;
        r8(UART0_BDH) = static_cast<uint8_t>((sbr >> 8) & 0x1F);
        r8(UART0_BDL) = static_cast<uint8_t>(sbr & 0xFF);
        r8(UART0_C4) = 12; // BRFA fine-adjust (low 5 bits)
        r8(UART0_C2) = C2_TE | C2_RE;
    }
}

extern "C"
{

void arch_init(void)
{
    // FPU is enabled earlier (Reset_Handler, before C++ ctors).
    uart0_init();
    kickos_armv7m_init();
}

void arch_console_write(char const* buf, size_t n)
{
    for (size_t i = 0; i < n; i++)
    {
        while ((r8(UART0_S1) & S1_TDRE) == 0)
        {
        }
        r8(UART0_D) = static_cast<uint8_t>(buf[i]);
    }
}

void arch_shutdown(int status)
{
    (void)status; // no exit on bare metal
    __asm volatile("cpsid i" ::: "memory");
    while (true)
    {
        __asm volatile("wfi");
    }
}

void Reset_Handler(void)
{
    wdog_disable(); // first: the watchdog would reset the part mid-bring-up
    enable_fpu();   // before ANY later code (the copy loops are integer, but a
                    // hard-float ABI could emit FP anywhere; CPACR-off FP faults)

    uint32_t* src = &_sidata;
    uint32_t* dst = &_sdata;
    while (dst < &_edata)
    {
        *dst++ = *src++;
    }
    for (uint32_t* b = &_sbss; b < &_ebss; b++)
    {
        *b = 0;
    }
    for (void (**fn)() = __init_array_start; fn != __init_array_end; fn++)
    {
        (*fn)();
    }
    arch_init();
    kickos::kmain(0, nullptr);
    arch_shutdown(0);
}

void HardFault_Handler(void)
{
    arch_shutdown(132);
}

}
