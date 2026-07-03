// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Atmel/Microchip AT91SAM3X8E (Arduino Due, Cortex-M3) chip backend. Registers
// clean-room from the SAM3X/SAM3A datasheet (Atmel-11057); hand-rolled, no ASF.
//
// M1 scope: privilege + SVC, no MPU. NO PLL: runs at the internal 4 MHz fast RC
// that the core boots on (FWS=0 needs no flash config). Two SAM3X specifics that
// bite: (1) the WATCHDOG runs at reset and WDT_MR is WRITE-ONCE -- it must be
// disabled first thing or the part resets itself; (2) flash is at 0x0008_0000
// (aliased to 0x0 at boot), so the reset path points VTOR at the real table.
// Peripheral clocks are individually gated in the PMC. Console = the dedicated
// UART on PA8/PA9; at 4 MHz, 115200 is unreachable, so it runs at 19200.
//
// Build-only here; flash with bossac (the Due programming port). apps/blink
// toggles the onboard "L" LED (PB27) for a no-UART smoke test.

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

    uint32_t SystemCoreClock = 4000000u; // internal fast RC at reset, no PLL
}

namespace
{
    inline volatile uint32_t& r32(uintptr_t a) { return *reinterpret_cast<volatile uint32_t*>(a); }

    constexpr uintptr_t WDT_MR = 0x400E1A54;    // write-once; WDDIS = bit 15
    constexpr uint32_t WDT_MR_WDDIS = 1u << 15;
    constexpr uintptr_t SCB_VTOR = 0xE000ED08;
    constexpr uintptr_t FLASH_BASE = 0x00080000; // real flash (aliased at 0x0)

    // PMC (§28): per-peripheral clock enable by peripheral ID.
    constexpr uintptr_t PMC_PCER0 = 0x400E0610;
    constexpr uint32_t PID_UART = 1u << 8;
    constexpr uint32_t PID_PIOA = 1u << 11;

    // PIOA (§31): route PA8/PA9 to the UART (peripheral A).
    constexpr uintptr_t PIOA_BASE = 0x400E0E00;
    constexpr uintptr_t PIOA_PDR = PIOA_BASE + 0x04; // give pins to the peripheral
    constexpr uint32_t PA8_PA9 = (1u << 8) | (1u << 9);

    // UART (§34), dedicated simple UART.
    constexpr uintptr_t UART_BASE = 0x400E0800;
    constexpr uintptr_t UART_CR = UART_BASE + 0x00;
    constexpr uintptr_t UART_MR = UART_BASE + 0x04;
    constexpr uintptr_t UART_SR = UART_BASE + 0x14;
    constexpr uintptr_t UART_THR = UART_BASE + 0x1C;
    constexpr uintptr_t UART_BRGR = UART_BASE + 0x20;
    constexpr uint32_t CR_RSTRX_RSTTX = (1u << 2) | (1u << 3);
    constexpr uint32_t CR_RXEN_TXEN = (1u << 4) | (1u << 6);
    constexpr uint32_t MR_NO_PARITY = 4u << 9; // PAR=100 (none), CHMODE=normal
    constexpr uint32_t SR_TXRDY = 1u << 1;
    constexpr uint32_t BRGR_19200 = 13; // CD = 4e6/(16*19200) ~= 13 (+0.16%)

    void uart_init()
    {
        r32(PMC_PCER0) = PID_UART | PID_PIOA; // clock the UART + its port
        r32(PIOA_PDR) = PA8_PA9;              // PA8/PA9 -> peripheral A (ABSR=0 at reset)
        r32(UART_CR) = CR_RSTRX_RSTTX;
        r32(UART_MR) = MR_NO_PARITY;
        r32(UART_BRGR) = BRGR_19200;
        r32(UART_CR) = CR_RXEN_TXEN;
    }
}

extern "C"
{

void arch_init(void)
{
    uart_init();
    kickos_armv7m_init();
}

void arch_console_write(char const* buf, size_t n)
{
    for (size_t i = 0; i < n; i++)
    {
        while ((r32(UART_SR) & SR_TXRDY) == 0)
        {
        }
        r32(UART_THR) = static_cast<uint8_t>(buf[i]);
    }
}

void arch_shutdown(int status)
{
    (void)status;
    __asm volatile("cpsid i" ::: "memory");
    while (true)
    {
        __asm volatile("wfi");
    }
}

void Reset_Handler(void)
{
    // FIRST: the watchdog is enabled at reset and WDT_MR is write-once -- disable
    // it before anything else can burn the (~16 s) budget or the write.
    r32(WDT_MR) = WDT_MR_WDDIS;
    // Flash (hence the vector table) lives at 0x0008_0000; point VTOR there (the
    // reset SP/PC were fetched via the 0x0 boot alias, which mirrors it).
    r32(SCB_VTOR) = FLASH_BASE;

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
