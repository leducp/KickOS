// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// ESP32-D0WDQ6 (WROOM-32) chip backend. Register addresses are clean-room facts
// transcribed from the ESP32 TRM v5.8 (peripheral base addresses per Table 3.3-6 in
// chapter 3, "System and Memory"; UART/WDT register offsets per the UART and Watchdog
// chapters). Hand-rolled, no ESP-IDF/HAL sources.

#include <kickos/arch/arch.h>
#include <kickos/arch/clk_q32.h> // shared Q32 tickless-clock reciprocal + multiply
#include <kickos/console_tx.h>
#include <kickos/sys/abi.h> // KOS_E* taxonomy (arch_pinmux_set)

#include <stdint.h>

#include "mmap.h"
#include "irq.h"
#include "regs/uart.h"
#include "regs/timg.h"
#include "regs/rtc_cntl.h"
#include "regs/dport.h"
#include "regs/gpio.h"
#include "regs/system.h"

namespace mmap = kickos::esp32::mmap;
namespace reg = kickos::esp32::reg;
namespace irq = kickos::esp32::irq;

namespace kickos
{
    int kmain(int argc, char** argv);
}

extern "C"
{
    void kickos_lx6_init(void);

    // Add a (CPU interrupt, logical line) device route and arm that CPU interrupt in
    // INTENABLE, which then serves as the line's kernel-owned mask (RULE L1). The
    // per-transfer gate stays at the peripheral, in the driver-owned reg::uart::INT_ENA.
    void kickos_lx6_bind_dev_int(int cpu_int, int line);

    extern uint32_t _sidata, _sdata, _edata, _sbss, _ebss;
    extern void (*__init_array_start[])();
    extern void (*__init_array_end[])();

    // The ROM first-stage loader leaves the CPU on the 40 MHz crystal (no PLL,
    // since KickOS boots without the IDF second-stage bootloader). This is the
    // reset value; clock_init_240mhz() (arch_init) raises the PLL and rewrites
    // this to 240 MHz so the CCOUNT/CCOMPARE0 ns<->cycle math stays coherent.
    uint32_t SystemCoreClock = 40000000u;
}

namespace
{
    inline volatile uint32_t& r32(uintptr_t a) { return *reinterpret_cast<volatile uint32_t*>(a); }

    // Logical kernel IRQ line the console_tx drain ISR is bound to (irq_table index).
    // DISTINCT namespace from the CPU interrupt number: on this arch the arch.h irq_*
    // seam is a software controller over logical lines, decoupled from the physical
    // Xtensa interrupts. See regs irq.h for the three numbering spaces.

    // --- Watchdogs. The ROM (running the image in flash-boot mode) leaves THREE
    //     watchdogs armed: the RTC WDT and the two Timer Group MWDTs (TIMG0, TIMG1).
    //     Each must be fully disabled or it resets the part within seconds of
    //     bring-up. Each register file is unlocked by writing its 32-bit write-
    //     protect key (default 0x50D83AA1), edited, then re-locked (write 0).
    //
    //     Clearing WDT_EN alone is NOT enough: the ROM arms the stage-0 watchdog
    //     via the separate FLASHBOOT_MOD_EN bit (a flash-boot watchdog independent
    //     of WDT_EN), which stays live until explicitly cleared. So each WDT needs
    //     both WDT_EN and FLASHBOOT_MOD_EN cleared. NOTE: the classic ESP32 has NO
    //     RTC super-watchdog (SWD); RTC_CNTL_SWD_* first appears on the ESP32-S2, so
    //     there is nothing more to disable here.
    void timg_wdt_disable(uintptr_t base)
    {
        r32(base + reg::timg::WDTWPROTECT_OFF) = reg::timg::WDT_WKEY;
        r32(base + reg::timg::WDTCONFIG0_OFF) &= ~(reg::timg::WDT_EN | reg::timg::WDT_FLASHBOOT_MOD_EN);
        r32(base + reg::timg::WDTWPROTECT_OFF) = 0;
    }

    void wdt_disable()
    {
        r32(reg::rtc_cntl::WDTWPROTECT) = reg::rtc_cntl::WDT_WKEY;
        r32(reg::rtc_cntl::WDTCONFIG0) &= ~(reg::rtc_cntl::WDT_EN | reg::rtc_cntl::WDT_FLASHBOOT_MOD_EN);
        r32(reg::rtc_cntl::WDTWPROTECT) = 0;

        timg_wdt_disable(mmap::TIMG0_BASE);
        timg_wdt_disable(mmap::TIMG1_BASE);
    }

    // --- CPU clock: raise the core from the ROM's 40 MHz XTAL to the 240 MHz PLL.
    //     The classic ESP32 makes 240 MHz from the 480 MHz BBPLL divided by 2. The
    //     BBPLL analog register file is NOT memory-mapped: it is reached over the
    //     chip's internal "reg-I2C" bus, whose bit-level transaction lives in the ESP32
    //     ROM, so the ROM routine is called at its fixed entry (mmap::ROM_REGI2C_WRITE,
    //     the symbol the IDF links as _regi2c_impl_write). Register addresses/bitfields
    //     and the 480 MHz / 40 MHz-XTAL analog values are clean-room facts from the
    //     ESP32 TRM (RTC_CNTL + DPORT clock chapters, analog-PLL description).

    // The classic-ESP32 BBPLL has NO memory-mapped lock/ready bit. The available
    // barrier is a slow-clock-domain one: start a TIMG0 RTC calibration for 0 slow
    // cycles and wait for RDY, which the hardware sets on the next RTC-slow edge, so
    // the analog writes have provably latched across the clock-domain crossing before
    // the CPU is switched onto the PLL.
    constexpr uintptr_t TIMG0_RTCCALICFG = mmap::TIMG0_BASE + reg::timg::RTCCALICFG_OFF;

    inline uint32_t rd_ccount()
    {
        uint32_t c;
        __asm volatile("rsr.ccount %0" : "=r"(c));
        return c;
    }

    // Busy-wait `us` microseconds. CCOUNT ticks at the CPU clock, so the caller
    // passes the cycles-per-us for whichever clock is live at the call site (40
    // before the PLL switch, 240 after).
    inline void delay_us(uint32_t us, uint32_t mhz)
    {
        uint32_t start = rd_ccount();
        uint32_t want = us * mhz;
        while ((rd_ccount() - start) < want)
        {
        }
    }

    void bbpll_write(uint8_t reg_add, uint8_t data)
    {
        // ROM _regi2c_impl_write(block, host_id, reg_add, data): windowed ABI at a fixed
        // ROM address, doing the whole analog reg-I2C transaction internally.
        auto rom_regi2c_write =
            reinterpret_cast<void (*)(uint8_t, uint8_t, uint8_t, uint8_t)>(mmap::ROM_REGI2C_WRITE);
        rom_regi2c_write(reg::system::I2C_BBPLL, reg::system::I2C_BBPLL_HOSTID, reg_add, data);
    }

    // Wait one RTC-slow cycle so pending analog/RTC writes latch across the clock
    // domain. Bounded: if RDY never sets (e.g. slow clock stopped) it returns after the
    // cap rather than hanging; the caller's fixed settle delay still covers it.
    void wait_slow_cycle()
    {
        r32(TIMG0_RTCCALICFG) = 0;                       // CLK_SEL=RTC_SLOW, MAX=0, clear RDY/START
        r32(TIMG0_RTCCALICFG) = reg::timg::CALI_START;   // RDY sets on the next slow edge
        for (uint32_t i = 0; i < 200000u; i++)
        {
            if ((r32(TIMG0_RTCCALICFG) & reg::timg::CALI_RDY) != 0)
            {
                return;
            }
        }
    }

    void clock_init_240mhz()
    {
        // Open the internal reg-I2C bus to the BBPLL: reset gates all analog blocks,
        // then ungate BBPLL (bit 17).
        r32(reg::system::ANA_CONFIG) |= reg::system::ANA_CONFIG_ALL_GATES;
        r32(reg::system::ANA_CONFIG) &= ~reg::system::ANA_CONFIG_BBPLL_GATE;

        // Power up the reg-I2C bus and the BBPLL analog block (clear force-power-down).
        r32(reg::rtc_cntl::OPTIONS0) &= ~reg::rtc_cntl::BIAS_I2C_FORCE_PD;
        r32(reg::rtc_cntl::OPTIONS0) &= ~(reg::rtc_cntl::BB_I2C_FORCE_PD |
                                          reg::rtc_cntl::BBPLL_FORCE_PD |
                                          reg::rtc_cntl::BBPLL_I2C_FORCE_PD);
        wait_slow_cycle(); // the power-up must latch before the reg-I2C config writes

        // BBPLL reset/calibration defaults (byte offsets 0/1/4/10/12 in the block).
        bbpll_write(0, 0x18);  // IR_CAL_DELAY
        bbpll_write(1, 0x20);  // IR_CAL_EXT_CAP
        bbpll_write(4, 0x9A);  // OC_ENB_FCAL
        bbpll_write(10, 0x00); // OC_ENB_VCON
        bbpll_write(12, 0x00); // BBADC_CAL_7_0

        // Raise core voltage to 1.25 V BEFORE locking the PLL: 240 MHz is unstable at
        // the XTAL-boot voltage. Still on the 40 MHz XTAL here, so 40 cyc/us.
        uint32_t dbias = r32(reg::rtc_cntl::DBIAS_REG);
        dbias &= ~(reg::rtc_cntl::DIG_DBIAS_MASK << reg::rtc_cntl::DIG_DBIAS_SHIFT);
        dbias |= reg::rtc_cntl::DIG_DBIAS_1V25 << reg::rtc_cntl::DIG_DBIAS_SHIFT;
        r32(reg::rtc_cntl::DBIAS_REG) = dbias;
        delay_us(3, 40);

        // Program the BBPLL to 480 MHz for a 40 MHz crystal: div_ref=0, div7_0=28,
        // div10_8=0, lref=0, dcur=6, bw=3. OC_LREF=(lref<<7)|(div10_8<<4)|div_ref=0,
        // OC_DIV_7_0=div7_0=28, OC_DCUR=(bw<<6)|dcur=0xC6.
        bbpll_write(11, 0xC3); // ENDIV5    (480 MHz)
        bbpll_write(9, 0x74);  // BBADC_DSMP (480 MHz)
        bbpll_write(2, 0x00);  // OC_LREF
        bbpll_write(3, 28);    // OC_DIV_7_0
        bbpll_write(5, 0xC6);  // OC_DCUR
        delay_us(160, 40);     // PLL lock settle (no lock bit on this chip; conservative)
        wait_slow_cycle();     // config latched across the domain before the source flip

        // Select 480/2 = 240 MHz, then route the CPU off the XTAL onto the PLL. The
        // divider must be set before the source flip.
        r32(reg::dport::CPU_PER_CONF) = reg::dport::CPUPERIOD_SEL_240;
        uint32_t clk = r32(reg::rtc_cntl::CLK_CONF);
        clk &= ~(reg::rtc_cntl::SOC_CLK_SEL_MASK << reg::rtc_cntl::SOC_CLK_SEL_SHIFT);
        clk |= reg::rtc_cntl::SOC_CLK_SEL_PLL << reg::rtc_cntl::SOC_CLK_SEL_SHIFT;
        r32(reg::rtc_cntl::CLK_CONF) = clk;

        // CCOUNT/CCOMPARE0 now tick at 240 MHz: publish it so arch_xtensa.cc's
        // ns<->cycle math (which reads SystemCoreClock live) stays coherent.
        SystemCoreClock = reg::system::CPU_CLOCK_HZ;
        delay_us(30, 240); // settle at the new clock (240 cyc/us now)

        // APB doubled 40->80 MHz, so the ROM's UART0 divider now halves the baud.
        // Drain any in-flight byte, then recompute CLKDIV for 80 MHz APB. clkdiv is
        // in 1/16 units: integer=[19:0], fraction=[23:20].
        while (((r32(reg::uart::STATUS) >> reg::uart::TXFIFO_CNT_SHIFT) & reg::uart::TXFIFO_CNT_MASK) != 0)
        {
        }
        uint32_t clkdiv16 = (reg::system::APB_CLOCK_HZ << 4) / reg::uart::CONSOLE_BAUD;
        uint32_t integer = clkdiv16 >> 4;
        uint32_t frac = clkdiv16 & 0xF;
        r32(reg::uart::CLKDIV) =
            (frac << reg::uart::CLKDIV_FRAC_SHIFT) | (integer & reg::uart::CLKDIV_INT_MASK);
    }

    // --- Monotonic clock: TIMG0 timer T0, a 64-bit free-running up-counter -------
    // Replaces the CCOUNT-backed arch_clock_now fallback (arch/xtensa/lx6). CCOUNT is a
    // 32-bit core cycle counter software-extended to 64 bits, so a wrap not observed
    // within one 2^32-cycle window (~17.9 s at 240 MHz) is lost; a native 64-bit counter
    // has no software wrap word to miss. CCOUNT is also gated by WAITI, so the idle path
    // freezes it on every idle, while the TIMG runs off APB, which keeps running in
    // plain WAITI.
    constexpr uintptr_t TIMG0_T0CONFIG = mmap::TIMG0_BASE + reg::timg::T0CONFIG_OFF;
    constexpr uintptr_t TIMG0_T0LO = mmap::TIMG0_BASE + reg::timg::T0LO_OFF;
    constexpr uintptr_t TIMG0_T0HI = mmap::TIMG0_BASE + reg::timg::T0HI_OFF;
    constexpr uintptr_t TIMG0_T0UPDATE = mmap::TIMG0_BASE + reg::timg::T0UPDATE_OFF;
    constexpr uintptr_t TIMG0_T0LOADLO = mmap::TIMG0_BASE + reg::timg::T0LOADLO_OFF;
    constexpr uintptr_t TIMG0_T0LOADHI = mmap::TIMG0_BASE + reg::timg::T0LOADHI_OFF;
    constexpr uintptr_t TIMG0_T0LOAD = mmap::TIMG0_BASE + reg::timg::T0LOAD_OFF;

    // Prescaler off the 80 MHz APB (fixed on the PLL for both 160/240 MHz CPU; see
    // clock_init_240mhz). reg::timg::DIVIDER=2 gives the highest resolution while
    // dodging the field's special-cased 0/1: 80/2 = 40 MHz -> 25 ns/tick. A 64-bit
    // counter at 40 MHz wraps in ~4600 years, so there is no wrap concern at all.
    constexpr uint32_t TIMG_HZ = reg::system::APB_CLOCK_HZ / reg::timg::DIVIDER; // 40 MHz

    // ticks -> ns reciprocal multiply: ns = ticks*1e9/HZ via mult = (1e9<<32)/HZ,
    // ns = (ticks*mult)>>32, done as a 64x64->64 split so the product never overflows.
    // HZ is a compile-time constant here (APB is fixed on the PLL), so the one divide
    // folds at build time.
    constexpr uint64_t TIMG_NS_MULT = kickos::arch_clk_recip_q32(TIMG_HZ);

    void timg_clock_init()
    {
        // Boot-order constraint: arch_clock_now MUST NOT run before this, and this
        // MUST run AFTER clock_init_240mhz (the counter rate is derived off the
        // 80 MHz PLL APB; running it on the 40 MHz XTAL APB would tick at half rate).
        // The TIMG0 APB clock is already live (the ROM armed its MWDT and
        // clock_init_240mhz's wait_slow_cycle drives TIMG0 RTCCALICFG), so no DPORT
        // peripheral-clock ungate is needed here.
        // Free-running up-counter: no alarm, no autoreload, prescaler = reg::timg::DIVIDER.
        r32(TIMG0_T0CONFIG) = reg::timg::T0_INCREASE | (reg::timg::DIVIDER << reg::timg::T0_DIVIDER_SHIFT);
        r32(TIMG0_T0LOADLO) = 0;
        r32(TIMG0_T0LOADHI) = 0;
        r32(TIMG0_T0LOAD) = 1; // any write loads the counter from {LOADHI,LOADLO} = 0
        r32(TIMG0_T0CONFIG) =
            reg::timg::T0_EN | reg::timg::T0_INCREASE | (reg::timg::DIVIDER << reg::timg::T0_DIVIDER_SHIFT);
    }

    // Read the 64-bit T0 count. The live counter is NOT directly readable: write
    // T0UPDATE to latch it into the T0LO/T0HI shadow regs, THEN read LO+HI. A bare
    // LO/HI read without the latch is stale. The whole latch-then-read runs under the
    // crit section because the LO/HI shadow is one shared resource: an interleaved
    // reader's UPDATE landing between the LO and HI reads tears the pair across a
    // low-word rollover. On the classic ESP32 T0UPDATE has no ready/self-clearing bit
    // (that is an S2/S3 addition); a single write latches synchronously.
    uint64_t timg_ticks()
    {
        arch_irq_state_t s = arch_irq_save();
        r32(TIMG0_T0UPDATE) = 1;
        uint32_t lo = r32(TIMG0_T0LO);
        uint32_t hi = r32(TIMG0_T0HI);
        arch_irq_restore(s);
        return (static_cast<uint64_t>(hi) << 32) | lo;
    }

    // --- Buffered console TX backend (console_tx.h). The ring drains via the UART0
    // TX-empty interrupt; slot_free/push touch the FIFO + status regs, irq_enable/
    // disable gate reg::uart::TXFIFO_EMPTY_INT AT THE PERIPHERAL; the CPU line's own
    // INTENABLE bit is the kernel's mask and is not touched here. ---
    uint32_t uart0_txfifo_cnt()
    {
        return (r32(reg::uart::STATUS) >> reg::uart::TXFIFO_CNT_SHIFT) & reg::uart::TXFIFO_CNT_MASK;
    }

    int esp32_tx_slot_free(void)
    {
        return uart0_txfifo_cnt() < reg::uart::TXFIFO_LIMIT;
    }
    void esp32_tx_push(uint8_t b) { r32(reg::uart::FIFO) = b; }
    void esp32_tx_irq_enable(void)
    {
        r32(reg::uart::INT_ENA) = r32(reg::uart::INT_ENA) | reg::uart::TXFIFO_EMPTY_INT;
    }
    void esp32_tx_irq_disable(void)
    {
        r32(reg::uart::INT_ENA) = r32(reg::uart::INT_ENA) & ~reg::uart::TXFIFO_EMPTY_INT;
    }

    constexpr uint32_t CONSOLE_TX_SIZE = 512; // power of two; > kprintf's 256B buffer
    char console_tx_buf[CONSOLE_TX_SIZE];
    console_tx_backend const esp32_console_backend = {
        esp32_tx_slot_free, esp32_tx_push, esp32_tx_irq_enable, esp32_tx_irq_disable};

    // UART0 sub-source -> logical line. Every entry currently names the ONE grouped line
    // (design-m4.6-irq-driver.md sections 5.1 and 7.7): the kernel-owned mask is CPU int
    // 13's INTENABLE bit, which cannot separate sub-sources, so splitting them across
    // lines would let masking one silently mask the others.
    struct uart0_route
    {
        uint32_t bit;
        int line;
    };
    constexpr uart0_route UART0_LINES[] = {
        {reg::uart::TXFIFO_EMPTY_INT, irq::CONSOLE_TX_LINE},
        {reg::uart::RXFIFO_FULL_INT, irq::CONSOLE_TX_LINE},
        {reg::uart::RXFIFO_OVF_INT, irq::CONSOLE_TX_LINE},
        {reg::uart::FRM_ERR_INT, irq::CONSOLE_TX_LINE},
        {reg::uart::PARITY_ERR_INT, irq::CONSOLE_TX_LINE},
    };

    constexpr uint32_t uart0_routed_mask()
    {
        uint32_t m = 0;
        for (auto const& row : UART0_LINES)
        {
            m = m | row.bit;
        }
        return m;
    }

    void uart0_irq_setup()
    {
        // The console owns UART0, so silence every source (the ROM polls, no IRQs) and
        // ack anything it left latched. Critical: CPU int 13 is armed below while
        // the ring is still unarmed, so a stale ROM-enabled source would storm the
        // level-1 dispatcher. console_tx_write re-enables ONLY TXFIFO_EMPTY, later.
        r32(reg::uart::INT_ENA) = 0;
        r32(reg::uart::INT_CLR) = 0xFFFFFFFFu;

        uint32_t conf1 = r32(reg::uart::CONF1);
        conf1 &= ~(reg::uart::TXFIFO_EMPTY_THRHD_MASK << reg::uart::TXFIFO_EMPTY_THRHD_SHIFT);
        conf1 |= (reg::uart::TXFIFO_EMPTY_THRHD & reg::uart::TXFIFO_EMPTY_THRHD_MASK)
                 << reg::uart::TXFIFO_EMPTY_THRHD_SHIFT;
        r32(reg::uart::CONF1) = conf1;

        r32(reg::dport::PRO_UART_INTR_MAP) = irq::UART0_CPU_INT;
        kickos_lx6_bind_dev_int(static_cast<int>(irq::UART0_CPU_INT), irq::CONSOLE_TX_LINE);
    }
}

extern "C"
{

// --- Device dispatch: one asserted CPU interrupt -> 0..N logical lines --------
// ISR context, called from the level-1 entry (arch/xtensa/lx6). Every UART0 sub-source
// shares one interrupt-matrix source and one CPU interrupt, so this is where they are
// told apart; 0 posts is a valid outcome.
// INT_ST is already INT_RAW & INT_ENA, so a disabled source cannot appear here. The
// driver owns every clear: this posts and returns, per RULE L1.
void kickos_lx6_dispatch_dev(int cpu_int)
{
    if (cpu_int != static_cast<int>(irq::UART0_CPU_INT))
    {
        return;
    }
    uint32_t const st = r32(reg::uart::INT_ST);
    // A source with no row in UART0_LINES has nothing that will ever clear it, and the
    // level-1 handler re-enters on the still-asserted CPU interrupt forever. It never
    // reaches the kernel's spurious accounting, because that is only entered through
    // kickos_isr_irq and an unroutable source posts no line, so this is a live-lock, not
    // a degraded line. Silence it HERE. The window's MMIO grant lets an unprivileged
    // driver enable any sub-source (RXFIFO_TOUT is the obvious one), so refusing to route
    // it must cost that driver its interrupt, never the machine.
    uint32_t const stray = st & ~uart0_routed_mask();
    if (stray != 0)
    {
        r32(reg::uart::INT_ENA) = r32(reg::uart::INT_ENA) & ~stray;
        r32(reg::uart::INT_CLR) = stray;
    }
    uint32_t posted = 0;
    for (auto const& row : UART0_LINES)
    {
        uint32_t const seen = 1u << static_cast<unsigned>(row.line);
        if ((st & row.bit) == 0 or (posted & seen) != 0)
        {
            continue;
        }
        posted = posted | seen;
        kickos_isr_irq(row.line);
    }
}

// --- Console reclaim: force UART0 back to a polled-ready channel --------------
// Runs from kpanic_enter, possibly in a partial nested-fault state, after a userspace
// driver has owned the whole UART0 window. Straight-line ABSOLUTE stores only: no reads
// of driver-mutable state, no loops, no baud derived from a clock the fault may have
// left wrong, and running it twice lands on the same registers.
// The pads are not restored because they cannot be lost: arch_pinmux_set refuses GPIO1
// and GPIO3.
void arch_console_reclaim(void)
{
    // Silence first. A stale enabled source would storm the level-1 handler through the
    // whole panic dump, and INT_ENA=0 makes every INT_ST bit read 0 whatever is latched.
    r32(reg::uart::INT_ENA) = 0;
    r32(reg::uart::INT_CLR) = 0xFFFFFFFFu;

    // Framing and clock select, plus a FIFO reset in the same absolute word so the dead
    // driver's queued bytes do not bury the dump. The two RST bits are R/W, not
    // self-clearing, so the second store is what releases them.
    r32(reg::uart::CONF0) =
        reg::uart::CONF0_8N1 | reg::uart::CONF0_TXFIFO_RST | reg::uart::CONF0_RXFIFO_RST;
    r32(reg::uart::CONF0) = reg::uart::CONF0_8N1;

    // Thresholds back to the bring-up values; also clears RX_TOUT_EN and RX_FLOW_EN,
    // the latter of which would gate TX on a CTS this board does not wire.
    r32(reg::uart::CONF1) =
        ((reg::uart::TXFIFO_EMPTY_THRHD & reg::uart::TXFIFO_EMPTY_THRHD_MASK)
         << reg::uart::TXFIFO_EMPTY_THRHD_SHIFT)
        | ((reg::uart::RXFIFO_FULL_THRHD & reg::uart::RXFIFO_FULL_THRHD_MASK)
           << reg::uart::RXFIFO_FULL_THRHD_SHIFT);

    // Baud off the fixed 80 MHz APB, the same constant folding clock_init_240mhz does.
    // SystemCoreClock is deliberately not consulted: it is writable state.
    constexpr uint32_t CLKDIV16 = (reg::system::APB_CLOCK_HZ << 4) / reg::uart::CONSOLE_BAUD;
    r32(reg::uart::CLKDIV) = ((CLKDIV16 & 0xFu) << reg::uart::CLKDIV_FRAC_SHIFT)
                             | ((CLKDIV16 >> 4) & reg::uart::CLKDIV_INT_MASK);
}

}

extern "C"
{

// --- Kernel diagnostic LED: the onboard LED on GPIO2 (active-high; DOIT ESP32
//     DevKit v1 / NodeMCU-32S blue LED). Register map in regs/gpio.h. ---
void arch_diag_led_init(void)
{
    r32(reg::gpio::IO_MUX_GPIO2) = reg::gpio::IO_MUX_GPIO_FUNC;
    r32(reg::gpio::ENABLE_W1TS) = reg::gpio::LED_BIT;
    r32(reg::gpio::OUT_W1TC) = reg::gpio::LED_BIT; // start dark
}

void arch_diag_led_set(int on)
{
    if (on)
    {
        r32(reg::gpio::OUT_W1TS) = reg::gpio::LED_BIT;
    }
    else
    {
        r32(reg::gpio::OUT_W1TC) = reg::gpio::LED_BIT;
    }
}

// Pins arch_pinmux_set refuses (EBUSY). GPIO1/GPIO3 = the U0 console TX/RX.
// GPIO6..11 drive the SPI flash the image executes from (XIP), so remuxing ANY of them
// stops execution dead.
static bool esp32_pin_kernel_owned(uint32_t pin)
{
    return pin == 1u or pin == 3u or (pin >= 6u and pin <= 11u);
}

// One-shot pin-function config (KOS_SYS_PINMUX_SET). port must be 0 (the WROOM has a
// single GPIO bank). func = the raw IO_MUX_GPIOn word (MCU_SEL | drive | FUN_IE),
// written verbatim to the pad's IO_MUX register. The GPIO number indexes
// reg::gpio::IO_MUX_OFF, whose offsets are scrambled in silicon (never
// pin*4); a 0 offset is a nonexistent/unbonded GPIO and fails EINVAL. GPIO-matrix
// signal routing (the second half of a full mux) is DEFERRED; this is the IO_MUX layer
// only. Validation runs before the register write.
int arch_pinmux_set(uint32_t port, uint32_t pin, uint32_t func)
{
    if (port != 0u or pin > 39u)
    {
        return -KOS_EINVAL;
    }
    uint32_t const off = reg::gpio::IO_MUX_OFF[pin];
    if (off == 0u)
    {
        return -KOS_EINVAL; // nonexistent (20/24/28..31) or unbonded on WROOM (37/38)
    }
    if (esp32_pin_kernel_owned(pin))
    {
        return -KOS_EBUSY;
    }
    r32(mmap::IO_MUX_BASE + off) = func;
    return 0;
}

void arch_init(void)
{
    // FP: the LX6 single-precision FPU (coprocessor 0) is enabled for all threads
    // (kickos_lx6_init sets CPENABLE). The FP data registers are caller-saved on Xtensa
    // (the compiler spills live f-regs around any call), so the COOPERATIVE switch needs
    // no FP handling; only the PREEMPTIVE path banks them, in the level-1 interrupt frame
    // that saves/restores f0-f15+FCR+FSR (startup.S). Double stays soft-float
    // (__muldf3): the LX6 FPU is single-only.
    wdt_disable();
    clock_init_240mhz(); // 40 MHz XTAL -> 240 MHz PLL; updates SystemCoreClock + UART0 baud
    timg_clock_init();   // 64-bit monotonic time base; AFTER the PLL (rate is off APB)
    kickos_lx6_init();
    uart0_irq_setup(); // route + arm the UART0 TX-empty interrupt for the console ring
}

// Monotonic clock override: convert the free-running TIMG0 T0 64-bit count (40 MHz,
// off the fixed 80 MHz APB) to ns via the cached reciprocal multiply, replacing the
// CCOUNT-backed arch_clock_now fallback (arch/xtensa/lx6) whose 32-bit + software-wrap
// source loses a wrap unobserved within ~17.9 s and stalls under WAITI. Only the
// scheduler's monotonic clock moves: arch_trace_now + the KICKOS_BENCH switch.S
// timestamps intentionally stay on raw CCOUNT (a cycle-accurate trace source; an
// intermittent skew there is a tolerable telemetry sample, fatal only to the clock).
uint64_t arch_clock_now(void)
{
    uint64_t ticks = timg_ticks();
    return kickos::arch_clk_mul_q32(ticks, TIMG_NS_MULT);
}

void arch_console_write(char const* buf, size_t n)
{
    console_tx_write(buf, n); // buffered; the routing guard (console.cc) keeps this thread-only
}

// Synchronous polled writer for the panic / fault / pre-arm path (console.cc selects it
// when the ring is unarmed or in ISR/panic context); it replaces a fallback that would
// re-enter the buffered writer.
void arch_console_write_sync(char const* buf, size_t n)
{
    for (size_t i = 0; i < n; i++)
    {
        uint32_t spin = 0;
        while (((r32(reg::uart::STATUS) >> reg::uart::TXFIFO_CNT_SHIFT) & reg::uart::TXFIFO_CNT_MASK) >=
               reg::uart::TXFIFO_LIMIT)
        {
            if (++spin > 200000u)
            {
                return; // bounded: a wedged UART must not hang the panic path (drop)
            }
        }
        r32(reg::uart::FIFO) = static_cast<uint8_t>(buf[i]);
    }
}

console_tx_backend const* arch_console_tx_backend(char** storage, uint32_t* size, int* irq_line)
{
    *storage = console_tx_buf;
    *size = CONSOLE_TX_SIZE;
    *irq_line = irq::CONSOLE_TX_LINE;
    return &esp32_console_backend;
}

void arch_shutdown(int status)
{
    (void)status; // no exit on bare metal
    // RSIL 15 then WAITI 15: mask everything (incl. below NMI) and park. WAITI writes
    // PS.INTLEVEL from its immediate, so it must be 15, not 0 (waiti 0 would unmask
    // everything the rsil masked).
    __asm volatile("rsil a0, 15" ::: "a0", "memory");
    while (true)
    {
        __asm volatile("waiti 15");
    }
}

void Reset_Handler(void)
{
    // The image links .data at its VMA, so LMA == VMA and this loop is a no-op.
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

}
