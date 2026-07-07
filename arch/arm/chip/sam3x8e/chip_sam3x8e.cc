// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Atmel/Microchip AT91SAM3X8E (Arduino Due, Cortex-M3) chip backend. Registers
// clean-room from the SAM3X/SAM3A datasheet (Atmel-11057); hand-rolled, no ASF.
//
// M1 scope: privilege + SVC, no MPU. clock_init() brings the part up on the
// 12 MHz crystal + PLLA to MCK = 84 MHz (SAM3X max); the core boots on the
// imprecise 4 MHz fast RC, at which 115200 is unreachable. Two SAM3X specifics
// that bite: (1) the WATCHDOG runs at reset and WDT_MR is WRITE-ONCE -- it must
// be disabled first thing or the part resets itself; (2) flash is at 0x0008_0000
// (aliased to 0x0 at boot), so the reset path points VTOR at the real table.
// Peripheral clocks are individually gated in the PMC. Console = the dedicated
// UART on PA8/PA9 at a true 115200 once the crystal/PLLA clock is up.
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

    uint32_t SystemCoreClock = 4000000u; // fast RC at reset; clock_init() raises it to 84 MHz
}

namespace
{
    inline volatile uint32_t& r32(uintptr_t a) { return *reinterpret_cast<volatile uint32_t*>(a); }

    constexpr uintptr_t WDT_MR = 0x400E1A54;    // write-once; WDDIS = bit 15
    constexpr uint32_t WDT_MR_WDDIS = 1u << 15;
    constexpr uintptr_t SCB_VTOR = 0xE000ED08;
    constexpr uintptr_t FLASH_BASE = 0x00080000; // real flash (aliased at 0x0)

    // EEFC (sec.18): the two flash banks. FWS (EEFC_FMR bits 11:8) sets the flash
    // read/write wait states; per sec.45 the AC-flash table, FWS=4 (5 read cycles)
    // covers up to 90 MHz at VDDCORE 1.8V -- required for 84 MHz. Set BEFORE the
    // clock is raised. EEFC_FMR at 0x400E0A00 (bank 0) / 0x400E0C00 (bank 1).
    constexpr uintptr_t EEFC0_FMR = 0x400E0A00;
    constexpr uintptr_t EEFC1_FMR = 0x400E0C00;
    constexpr uint32_t FMR_FWS_4 = 4u << 8;

    // PMC (sec.28): clock generator + status. Base 0x400E0600.
    constexpr uintptr_t PMC_BASE = 0x400E0600;
    constexpr uintptr_t CKGR_MOR = PMC_BASE + 0x20;   // Main Oscillator Register
    constexpr uintptr_t CKGR_PLLAR = PMC_BASE + 0x28; // PLLA Register
    constexpr uintptr_t PMC_MCKR = PMC_BASE + 0x30;   // Master Clock Register
    constexpr uintptr_t PMC_SR = PMC_BASE + 0x68;     // Status Register

    // CKGR_MOR (sec.28): crystal oscillator. KEY 0x37 (bits 23:16) gates the write;
    // MOSCXTST (15:8) is the crystal startup counter (in SLCK/8); keep the fast RC
    // (MOSCRCEN) enabled while the crystal warms up, then MOSCSEL picks the crystal.
    constexpr uint32_t MOR_KEY = 0x37u << 16;
    constexpr uint32_t MOR_MOSCXTEN = 1u << 0;
    constexpr uint32_t MOR_MOSCRCEN = 1u << 3;
    constexpr uint32_t MOR_MOSCXTST = 0x3Fu << 8;
    constexpr uint32_t MOR_MOSCSEL = 1u << 24;
    constexpr uint32_t MOR_CRYSTAL = MOR_KEY | MOR_MOSCXTST | MOR_MOSCRCEN | MOR_MOSCXTEN;

    // CKGR_PLLAR (sec.28): PLLA = MAINCK * (MULA+1) / DIVA. ONE (bit 29) reads 1;
    // MULA (26:16) = 13 -> x14; DIVA (7:0) = 1; PLLCOUNT (13:8) = LOCK delay in SLCK.
    // 12 MHz * 14 / 1 = 168 MHz.
    constexpr uint32_t PLLAR_ONE = 1u << 29;
    constexpr uint32_t PLLAR_MULA = 13u << 16;
    constexpr uint32_t PLLAR_COUNT = 0x3Fu << 8;
    constexpr uint32_t PLLAR_DIVA = 1u << 0;

    // PMC_MCKR (sec.28): CSS (1:0) source, PRES (6:4) prescaler. PLLA/2 = 84 MHz.
    constexpr uint32_t MCKR_CSS_MAIN = 1u << 0;
    constexpr uint32_t MCKR_CSS_PLLA = 2u << 0;
    constexpr uint32_t MCKR_PRES_DIV2 = 1u << 4;

    // PMC_SR (sec.28) poll bits.
    constexpr uint32_t SR_MOSCXTS = 1u << 0;   // crystal oscillator stable
    constexpr uint32_t SR_LOCKA = 1u << 1;     // PLLA locked
    constexpr uint32_t SR_MCKRDY = 1u << 3;    // master clock ready
    constexpr uint32_t SR_MOSCSELS = 1u << 16; // main oscillator selection done

    // Bounded poll: a missing/dead crystal degrades (falls back to slow clock on a
    // PLLA switch, per sec.28) instead of hanging the boot.
    void pmc_wait(uint32_t bit)
    {
        for (uint32_t i = 0; i < 0x100000u; i++)
        {
            if ((r32(PMC_SR) & bit) != 0)
            {
                break;
            }
        }
    }

    void clock_init()
    {
        // 1. Flash wait states first, both banks (sec.18 / sec.45), before raising MCK.
        r32(EEFC0_FMR) = FMR_FWS_4;
        r32(EEFC1_FMR) = FMR_FWS_4;

        // 2. Start the 12 MHz crystal, then select it as the main clock (sec.28).
        r32(CKGR_MOR) = MOR_CRYSTAL;
        pmc_wait(SR_MOSCXTS);
        r32(CKGR_MOR) = MOR_CRYSTAL | MOR_MOSCSEL;
        pmc_wait(SR_MOSCSELS);

        // Run MCK off the 12 MHz main clock (PRES=1) before touching PLLA.
        r32(PMC_MCKR) = MCKR_CSS_MAIN;
        pmc_wait(SR_MCKRDY);

        // 3. PLLA = 12 MHz * 14 / 1 = 168 MHz (sec.28 CKGR_PLLAR).
        r32(CKGR_PLLAR) = PLLAR_ONE | PLLAR_MULA | PLLAR_COUNT | PLLAR_DIVA;
        pmc_wait(SR_LOCKA);

        // 4. Switch MCK to PLLA/2 = 84 MHz. sec.28 mandates, for a PLL source: set
        //    PRES, wait MCKRDY, then set CSS, wait MCKRDY (two writes, not one).
        r32(PMC_MCKR) = MCKR_PRES_DIV2 | MCKR_CSS_MAIN;
        pmc_wait(SR_MCKRDY);
        r32(PMC_MCKR) = MCKR_PRES_DIV2 | MCKR_CSS_PLLA;
        pmc_wait(SR_MCKRDY);

        SystemCoreClock = 84000000u;
    }

    // PMC (sec.28): per-peripheral clock enable by peripheral ID.
    constexpr uintptr_t PMC_PCER0 = 0x400E0610;
    constexpr uint32_t PID_UART = 1u << 8;
    constexpr uint32_t PID_PIOA = 1u << 11;

    // PIOA (sec.31): route PA8/PA9 to the UART (peripheral A).
    constexpr uintptr_t PIOA_BASE = 0x400E0E00;
    constexpr uintptr_t PIOA_PDR = PIOA_BASE + 0x04; // give pins to the peripheral
    constexpr uint32_t PA8_PA9 = (1u << 8) | (1u << 9);

    // UART (sec.34), dedicated simple UART.
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
    // CD = MCK/(16*baud) = 84e6/(16*115200) = 45.57 -> 46; actual 84e6/(16*46) =
    // 114130 baud (-0.93%, well inside the 5% limit in sec.34).
    constexpr uint32_t BRGR_115200 = 46;

    void uart_init()
    {
        r32(PMC_PCER0) = PID_UART | PID_PIOA; // clock the UART + its port
        r32(PIOA_PDR) = PA8_PA9;              // PA8/PA9 -> peripheral A (ABSR=0 at reset)
        r32(UART_CR) = CR_RSTRX_RSTTX;
        r32(UART_MR) = MR_NO_PARITY;
        r32(UART_BRGR) = BRGR_115200;
        r32(UART_CR) = CR_RXEN_TXEN;
    }
}

extern "C"
{

void arch_init(void)
{
    clock_init(); // crystal + PLLA -> 84 MHz (watchdog already disabled in Reset_Handler)
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

// Kernel diagnostic LED: "L" LED = PB27 via PIO controller B, active-high.
void arch_diag_led_init(void)
{
    constexpr uintptr_t PIOB_PER = 0x400E1000 + 0x00;
    constexpr uintptr_t PIOB_OER = 0x400E1000 + 0x10;
    r32(PMC_PCER0) = 1u << 12; // clock PIOB (peripheral ID 12)
    r32(PIOB_PER) = 1u << 27;  // pin controlled by the PIO
    r32(PIOB_OER) = 1u << 27;  // output enabled
}

void arch_diag_led_set(int on)
{
    constexpr uintptr_t PIOB_SODR = 0x400E1000 + 0x30;
    constexpr uintptr_t PIOB_CODR = 0x400E1000 + 0x34;
    if (on)
    {
        r32(PIOB_SODR) = 1u << 27;
    }
    else
    {
        r32(PIOB_CODR) = 1u << 27;
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
