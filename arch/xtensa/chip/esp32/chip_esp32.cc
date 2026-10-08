// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// ESP32 (WROOM-32) chip backend. Register addresses are clean-room facts
// transcribed from the ESP32 TRM v5.8 (peripheral base addresses per Table 3.3-6 in
// chapter 3, "System and Memory"; UART/WDT register offsets per the UART and Watchdog
// chapters). Hand-rolled, no ESP-IDF/HAL sources.

#include <kickos/arch/arch.h>
#include "crt_tail.h"
#include "pin_guard.h"
#include <kickos/arch/doorbell_protocol.h>
#include <kickos/arch/lx6_doorbell.h>
#include <kickos/arch/clk_q32.h>
#include <kickos/config/limits.h>
#include <kickos/console_tx.h>
#include <kickos/sys/abi.h>

#include <stdint.h>

#include <kickos/chip_cpuid.h>
#include <kickos/chip_mmap.h>
#include "board_pins.h"
#include "irq.h"
#include "routing.h"
#include "regs/uart.h"
#include "regs/timg.h"
#include "regs/rtc_cntl.h"
#include "regs/dport.h"
#include "regs/gpio.h"
#include "regs/system.h"

namespace mmap = kickos::esp32::mmap;
namespace reg = kickos::esp32::reg;
namespace irq = kickos::esp32::irq;

extern "C"
{
    void kickos_lx6_init(void);
#if KICKOS_NUM_CORES > 1
    void _kickos_lx6_core1_entry(void);
    void kfault_terminate(void) __attribute__((noreturn));
#endif

    // Add a (CPU interrupt, logical line) device route and arm that CPU interrupt in
    // INTENABLE, which then serves as the line's kernel-owned mask (RULE L1). The
    // per-transfer gate stays at the peripheral, in the driver-owned reg::uart::INT_ENA.
    void kickos_lx6_bind_dev_int(int cpu_int, int line, int core);

    extern uint32_t _sidata, _sdata, _edata, _sbss, _ebss;

    // The ROM loader leaves the CPU on the 40 MHz crystal; clock_init_240mhz rewrites this.
    uint32_t SystemCoreClock = 40000000u;
}

namespace
{
    inline volatile uint32_t& r32(uintptr_t a) { return *reinterpret_cast<volatile uint32_t*>(a); }

#if KICKOS_NUM_CORES > 1
    // Sized far over: an expiry means the mechanism failed, not that a core was slow.
    constexpr uint64_t ESP32_ARRIVAL_WAIT_NS = 1000ull * 1000ull * 1000ull;

    char const NO_ARRIVE[] = "KickOS: esp32 APP_CPU released and never arrived, core ";
    char const NL[] = "\n";
    // These print through arch_console_write_sync: every site runs before kmain arms the
    // console ring (arch_init) or inside an interrupt, where the buffered writer drops.
    char const ARRIVED[] = "# cores: ";
    char const ARRIVED_TAIL[] = " arrived\n";

    void hex1(uint32_t v)
    {
        char c = static_cast<char>('0' + (v & 0xFu));
        if ((v & 0xFu) > 9u)
        {
            c = static_cast<char>('a' + (v & 0xFu) - 10u);
        }
        arch_console_write_sync(&c, 1);
    }
#endif

    // Point ONE core's matrix bank at `cpu_int` for `source` and sink it in every other
    // core's bank. The banks are independent (TRM v5.8 8.1), so a source left mapped in both
    // is taken by both. The sink write is what disconnects the core a previous route named;
    // against a map register's reset value of 16, itself one of the six sinks (the shared
    // map-register diagram, p.262), it changes nothing.
    void route_source(uint32_t source, uint32_t core, uint32_t cpu_int)
    {
        uint32_t pro = reg::dport::INTR_MAP_SINK;
        uint32_t app = reg::dport::INTR_MAP_SINK;
        if (core == 0u)
        {
            pro = cpu_int;
        }
        else
        {
            app = cpu_int;
        }
        r32(reg::dport::pro_intr_map(source)) = pro;
        r32(reg::dport::app_intr_map(source)) = app;
    }

    // The ROM leaves the RTC WDT and both TIMG MWDTs armed, each resetting the part within
    // seconds. Clearing WDT_EN alone is not enough: FLASHBOOT_MOD_EN arms an independent
    // flash-boot watchdog. The classic ESP32 has no RTC super-watchdog (RTC_CNTL_SWD_* first
    // appears on the ESP32-S2).
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

    // 240 MHz = 480 MHz BBPLL / 2. The BBPLL analog registers are NOT memory-mapped: they sit
    // on the internal reg-I2C bus, whose transaction lives in ROM (ROM_REGI2C_WRITE).

    // The BBPLL has no memory-mapped lock/ready bit. The barrier is a TIMG0 RTC calibration
    // for 0 slow cycles: RDY sets on the next RTC-slow edge, so the analog writes have
    // latched across the clock-domain crossing before the CPU moves onto the PLL.
    constexpr uintptr_t TIMG0_RTCCALICFG = mmap::TIMG0_BASE + reg::timg::RTCCALICFG_OFF;

    inline uint32_t rd_ccount()
    {
        uint32_t c;
        __asm volatile("rsr.ccount %0" : "=r"(c));
        return c;
    }

    // CCOUNT ticks at the CPU clock: `mhz` must be the clock live at the call site.
    inline void delay_us(uint32_t us, uint32_t mhz)
    {
        uint32_t start = rd_ccount();
        uint32_t want = us * mhz;
        while ((rd_ccount() - start) < want)
        {
        }
    }

    // _regi2c_impl_write(block, host_id, reg_add, data), a windowed-ABI ROM routine.
    constexpr uintptr_t ROM_REGI2C_WRITE = 0x400041A4u;

    void bbpll_write(uint8_t reg_add, uint8_t data)
    {
        auto rom_regi2c_write =
            reinterpret_cast<void (*)(uint8_t, uint8_t, uint8_t, uint8_t)>(ROM_REGI2C_WRITE);
        rom_regi2c_write(reg::system::I2C_BBPLL, reg::system::I2C_BBPLL_HOSTID, reg_add, data);
    }

    // Bounded: if RDY never sets (slow clock stopped), the caller's fixed settle delay
    // still covers it.
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
        r32(reg::system::ANA_CONFIG) |= reg::system::ANA_CONFIG_ALL_GATES;
        r32(reg::system::ANA_CONFIG) &= ~reg::system::ANA_CONFIG_BBPLL_GATE;

        r32(reg::rtc_cntl::OPTIONS0) &= ~reg::rtc_cntl::BIAS_I2C_FORCE_PD;
        r32(reg::rtc_cntl::OPTIONS0) &= ~(reg::rtc_cntl::BB_I2C_FORCE_PD |
                                          reg::rtc_cntl::BBPLL_FORCE_PD |
                                          reg::rtc_cntl::BBPLL_I2C_FORCE_PD);
        wait_slow_cycle(); // the power-up must latch before the reg-I2C config writes

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

        // The 480/2 divider must be set before the source flips onto the PLL.
        r32(reg::dport::CPU_PER_CONF) = reg::dport::CPUPERIOD_SEL_240;
        uint32_t clk = r32(reg::rtc_cntl::CLK_CONF);
        clk &= ~(reg::rtc_cntl::SOC_CLK_SEL_MASK << reg::rtc_cntl::SOC_CLK_SEL_SHIFT);
        clk |= reg::rtc_cntl::SOC_CLK_SEL_PLL << reg::rtc_cntl::SOC_CLK_SEL_SHIFT;
        r32(reg::rtc_cntl::CLK_CONF) = clk;

        // CCOUNT/CCOMPARE0 now tick at 240 MHz: publish it so arch_xtensa.cc's
        // ns<->cycle math (which reads SystemCoreClock live) stays coherent.
        SystemCoreClock = reg::system::CPU_CLOCK_HZ;
        delay_us(30, 240);

        // APB doubled 40->80 MHz, so the ROM's UART0 divider now halves the baud.
        while (((r32(reg::uart::STATUS) >> reg::uart::TXFIFO_CNT_S) & reg::uart::TXFIFO_CNT_MASK) != 0)
        {
        }
        r32(reg::uart::CLKDIV) = reg::uart::clkdiv(reg::system::APB_CLOCK_HZ, reg::uart::CONSOLE_BAUD);
    }

    // TIMG0 T0 is the monotonic clock, not the CCOUNT fallback (arch/xtensa/lx6): CCOUNT is
    // per core and unsynchronised, and its software-extended wrap is lost if not observed
    // within ~17.9 s at 240 MHz.
    constexpr uintptr_t TIMG0_T0CONFIG = mmap::TIMG0_BASE + reg::timg::T0CONFIG_OFF;
    constexpr uintptr_t TIMG0_T0LO = mmap::TIMG0_BASE + reg::timg::T0LO_OFF;
    constexpr uintptr_t TIMG0_T0HI = mmap::TIMG0_BASE + reg::timg::T0HI_OFF;
    constexpr uintptr_t TIMG0_T0UPDATE = mmap::TIMG0_BASE + reg::timg::T0UPDATE_OFF;
    constexpr uintptr_t TIMG0_T0LOADLO = mmap::TIMG0_BASE + reg::timg::T0LOADLO_OFF;
    constexpr uintptr_t TIMG0_T0LOADHI = mmap::TIMG0_BASE + reg::timg::T0LOADHI_OFF;
    constexpr uintptr_t TIMG0_T0LOAD = mmap::TIMG0_BASE + reg::timg::T0LOAD_OFF;

    // APB is 80 MHz on the PLL whatever the CPU divider. DIVIDER=2, not 1: the field
    // special-cases 0 and 1.
    constexpr uint32_t TIMG_HZ = reg::system::APB_CLOCK_HZ / reg::timg::DIVIDER;

    constexpr uint64_t TIMG_NS_MULT = kickos::arch_clk_recip_q32(TIMG_HZ);

    void timg_clock_init()
    {
        // Runs before any arch_clock_now and after clock_init_240mhz: on the 40 MHz XTAL
        // APB the counter ticks at half rate. No DPORT clock ungate: the ROM armed TIMG0's
        // MWDT, so its APB clock is live.
        r32(TIMG0_T0CONFIG) = reg::timg::T0_INCREASE | (reg::timg::DIVIDER << reg::timg::T0_DIVIDER_SHIFT);
        r32(TIMG0_T0LOADLO) = 0;
        r32(TIMG0_T0LOADHI) = 0;
        r32(TIMG0_T0LOAD) = 1; // any write loads the counter from {LOADHI,LOADLO} = 0
        r32(TIMG0_T0CONFIG) =
            reg::timg::T0_EN | reg::timg::T0_INCREASE | (reg::timg::DIVIDER << reg::timg::T0_DIVIDER_SHIFT);
    }

    // The live counter is not directly readable: write T0UPDATE to latch it into the T0LO/T0HI
    // shadow regs, then read LO+HI; a bare LO/HI read without the latch is stale. On the classic ESP32 T0UPDATE has no ready/self-clearing bit (that is an
    // S2/S3 addition), so a single write latches synchronously.
    //
    // The shadow is one resource for both CPUs: a peer's UPDATE can land between this core's LO
    // and HI reads, and no local interrupt mask excludes it, tearing the pair across a low-word
    // rollover. A matching pair of HI reads means LO belongs to a latch carrying that HI. HI
    // advances once per 2^32 ticks, about 107 s at 40 MHz.
    uint64_t timg_ticks()
    {
        while (true)
        {
            r32(TIMG0_T0UPDATE) = 1;
            uint32_t const hi1 = r32(TIMG0_T0HI);
            uint32_t const lo = r32(TIMG0_T0LO);
            uint32_t const hi2 = r32(TIMG0_T0HI);
            if (hi1 == hi2)
            {
                return (static_cast<uint64_t>(hi1) << 32) | lo;
            }
        }
    }

    // irq_enable/disable gate TXFIFO_EMPTY_INT AT THE PERIPHERAL; the CPU line's INTENABLE bit
    // is the kernel's mask and is not touched here. The source is a latch, dropped by the
    // INT_CLR writes in esp32_tx_push and esp32_tx_irq_enable.
    uint32_t uart0_txfifo_cnt()
    {
        return (r32(reg::uart::STATUS) >> reg::uart::TXFIFO_CNT_S) & reg::uart::TXFIFO_CNT_MASK;
    }

    int esp32_tx_slot_free(void)
    {
        return uart0_txfifo_cnt() < reg::uart::TXFIFO_LIMIT;
    }
    void esp32_tx_push(uint8_t b)
    {
        r32(reg::uart::FIFO) = b;
        // Drop the TX-empty latch after every push. UART_INT_RAW is self-set and cleared only
        // by INT_CLR (regs/uart.h), so once the FIFO has passed the threshold the latch, and
        // with it the matrix source, stays asserted on a condition that is no longer true.
        // The kernel drain runs with the line UNMASKED (irq_attach, not a tier-1 binding), so
        // an undropped latch re-enters the dispatcher forever whenever the FIFO fills before
        // the ring empties.
        r32(reg::uart::INT_CLR) = reg::uart::TXFIFO_EMPTY_INT;
    }
    void esp32_tx_irq_enable(void)
    {
        r32(reg::uart::INT_CLR) = reg::uart::TXFIFO_EMPTY_INT; // any latch left from a stopped burst
        r32(reg::uart::INT_ENA) = r32(reg::uart::INT_ENA) | reg::uart::TXFIFO_EMPTY_INT;
    }
    void esp32_tx_irq_disable(void)
    {
        r32(reg::uart::INT_ENA) = r32(reg::uart::INT_ENA) & ~reg::uart::TXFIFO_EMPTY_INT;
    }
    char console_tx_buf[KICKOS_CONSOLE_TX_SIZE];
    console_tx_backend const esp32_console_backend = {
        esp32_tx_slot_free, esp32_tx_push, esp32_tx_irq_enable, esp32_tx_irq_disable};

    // The window arch_console_reclaim rewrites. UART0 owns 0x3FF4_0000 to 0x3FF4_0FFF
    // (TRM Table 3.3-6); UART1 starts at 0x3FF5_0000.
    constexpr uintptr_t CONSOLE_WIN_BASE = mmap::UART0_BASE;
    static_assert(KICKOS_BOARD_CONSOLE_BASE == CONSOLE_WIN_BASE, "the board's console is not the UART this backend drives");
    static_assert(KICKOS_BOARD_CONSOLE_TX_SELECT == 0 and KICKOS_BOARD_CONSOLE_RX_SELECT == 0,
                  "the console pads keep IO MUX function 0, their reset value, which this backend leaves");
    constexpr size_t CONSOLE_WIN_SIZE = KICKOS_BOARD_CONSOLE_SIZE;

    static_assert(reg::uart::OFF_INT_ENA < CONSOLE_WIN_SIZE
                      and reg::uart::OFF_INT_CLR < CONSOLE_WIN_SIZE
                      and reg::uart::OFF_CONF0 < CONSOLE_WIN_SIZE
                      and reg::uart::OFF_CONF1 < CONSOLE_WIN_SIZE
                      and reg::uart::OFF_CLKDIV < CONSOLE_WIN_SIZE,
                  "arch_console_reclaim writes outside the window it reports");

    // UART0 sub-source -> logical line. Every entry names the one grouped line:
    // the kernel-owned mask is CPU int 13's INTENABLE bit, which cannot separate
    // sub-sources, so splitting them across lines would let masking one silently mask the
    // others.
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

#if KICKOS_KERNEL_CORES > 1
    constexpr bool uart0_posts_one_line()
    {
        for (auto const& row : UART0_LINES)
        {
            if (row.line != irq::CONSOLE_TX_LINE)
            {
                return false;
            }
        }
        return true;
    }
    // A claim routes CPU int 13 whole, so a second line posted from it would follow a claim of
    // the bound one away from its own. kickos_lx6_bind_dev_int refuses the bind half at boot.
    static_assert(uart0_posts_one_line(),
                  "every line CPU int 13 posts moves with the one bound to it");
#endif

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
        // CPU int 13 is armed below while the ring is still unarmed, so a stale ROM-enabled
        // source would storm the level-1 dispatcher.
        r32(reg::uart::INT_ENA) = 0;
        r32(reg::uart::INT_CLR) = 0xFFFFFFFFu;

        uint32_t conf1 = r32(reg::uart::CONF1);
        conf1 &= ~(reg::uart::TXFIFO_EMPTY_THRHD_MASK << reg::uart::TXFIFO_EMPTY_THRHD_S);
        conf1 |= (reg::uart::TXFIFO_EMPTY_THRHD & reg::uart::TXFIFO_EMPTY_THRHD_MASK)
                 << reg::uart::TXFIFO_EMPTY_THRHD_S;
        r32(reg::uart::CONF1) = conf1;

        route_source(irq::UART0_SRC, irq::CONSOLE_CORE, irq::UART0_CPU_INT);
        kickos_lx6_bind_dev_int(static_cast<int>(irq::UART0_CPU_INT), irq::CONSOLE_TX_LINE,
                                static_cast<int>(irq::CONSOLE_CORE));
    }
}

extern "C"
{

// ISR context. Every UART0 sub-source shares one interrupt-matrix source and one CPU
// interrupt, so this is where they are told apart; 0 posts is a valid outcome.
// INT_ST is already INT_RAW & INT_ENA, so a disabled source cannot appear here. The
// driver owns every clear: this posts and returns, per RULE L1.
void kickos_lx6_dispatch_dev(int cpu_int)
{
    if (cpu_int != static_cast<int>(irq::UART0_CPU_INT))
    {
        return;
    }
    uint32_t const st = r32(reg::uart::INT_ST);
    // A source with no row in UART0_LINES has nothing that will ever clear it, and posts no
    // line, so it never reaches the kernel's spurious accounting: the level-1 handler would
    // re-enter forever. Silence it HERE. The window's MMIO grant lets a driver enable any
    // sub-source (RXFIFO_TOUT), so an unrouted one must cost that driver its interrupt,
    // never the machine.
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

#if KICKOS_NUM_CORES > 1
// Called with the line masked, possibly while its source asserts. Nothing is stranded on the
// core the source leaves only because the input is Level-Triggered (TRM v5.8 Table 8.3-2): that
// core's pending bit follows the input, where an Edge-Triggered one would stay latched.
void kickos_lx6_route_dev_int(int cpu_int, int core)
{
    constexpr uint32_t EDGE_INPUTS = (1u << 10) | (1u << 22) | (1u << 28) | (1u << 30);
    static_assert(((1u << irq::UART0_CPU_INT) & EDGE_INPUTS) == 0,
                  "a routed device line needs a Level-Triggered CPU input");
    if (cpu_int == static_cast<int>(irq::UART0_CPU_INT))
    {
        route_source(irq::UART0_SRC, static_cast<uint32_t>(core), irq::UART0_CPU_INT);
    }
}
#endif

void arch_console_reclaim_window(uintptr_t* base, size_t* size)
{
    *base = CONSOLE_WIN_BASE;
    *size = CONSOLE_WIN_SIZE;
}

// Runs from kpanic_enter, possibly in a partial nested-fault state, after a userspace
// driver has owned the whole UART0 window. Straight-line ABSOLUTE stores only: no reads
// of driver-mutable state, no loops, no baud derived from a clock the fault may have
// left wrong, and running it twice lands on the same registers.
// The pads are not restored because they cannot be lost: arch_pinmux_set refuses the console
// pins.
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
         << reg::uart::TXFIFO_EMPTY_THRHD_S)
        | ((reg::uart::RXFIFO_FULL_THRHD & reg::uart::RXFIFO_FULL_THRHD_MASK)
           << reg::uart::RXFIFO_FULL_THRHD_S);

    // Baud off the fixed 80 MHz APB, the same constant folding clock_init_240mhz does.
    // SystemCoreClock is deliberately not consulted: it is writable state.
    r32(reg::uart::CLKDIV) = reg::uart::clkdiv(reg::system::APB_CLOCK_HZ, reg::uart::CONSOLE_BAUD);
}

}

namespace
{
    static_assert(KICKOS_BOARD_LED_BIT < 32u, "the LED's OUT and ENABLE bits are in the low GPIO bank's registers");

    constexpr uint32_t LED_BIT = 1u << KICKOS_BOARD_LED_BIT;

    constexpr uintptr_t led_out(bool level)
    {
        if (level)
        {
            return reg::gpio::OUT_W1TS;
        }
        return reg::gpio::OUT_W1TC;
    }

    constexpr bool LED_LIT = KICKOS_BOARD_LED_ACTIVE_LOW == 0;
}

extern "C"
{

void arch_diag_led_init(void)
{
    r32(mmap::IO_MUX_BASE + reg::gpio::IO_MUX_OFF[KICKOS_BOARD_LED_BIT]) = reg::gpio::IO_MUX_GPIO_FUNC;
    r32(reg::gpio::ENABLE_W1TS) = LED_BIT;
    r32(led_out(not LED_LIT)) = LED_BIT;
}

void arch_diag_led_set(int on)
{
    if (on)
    {
        r32(led_out(LED_LIT)) = LED_BIT;
    }
    else
    {
        r32(led_out(not LED_LIT)) = LED_BIT;
    }
}

// One-shot pin-function config (KOS_SYS_PINMUX_SET), the IO_MUX layer only: no GPIO-matrix
// routing. func is the raw IO_MUX_GPIOn word (MCU_SEL | drive | FUN_IE), written verbatim.
// IO_MUX_OFF is scrambled in silicon (never pin*4); a 0 offset is a nonexistent or unbonded
// GPIO.
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
    if (kickos::board_pin_kernel_owned(port, pin))
    {
        return -KOS_EBUSY;
    }
    r32(mmap::IO_MUX_BASE + off) = func;
    return 0;
}

// Branch-clock oracle (arch.h): report the function clock feeding a peripheral block so a
// userspace driver derives its own divisor. RTC_CNTL and DPORT are reserved blocks, so the
// holder of a UART window cannot read the tree itself.
//
// This chip publishes no APB frequency and no crystal frequency. APB is specified only as a
// function of the CPU clock source (TRM v5.8 Table 7.2-4 p.169), and on the PLL branch it is
// 80 MHz for every CPUPERIOD_SEL, independent of the crystal. That branch is a genuine
// register-derived answer and it is the only one this returns.
//
// The XTAL and RC_FAST branches make APB equal to CPU_CLK, whose numerator is the crystal,
// and the crystal is readable from no register on this part (TRM section 7.2.2 p.167 gives
// only a 2..40 MHz range; there is no CLK_XTAL_FREQ field like the C6's). It can only be
// estimated against the untrimmed internal RC oscillator through the TIMG calibration unit,
// which is several percent out and would also collide with the bring-up's use of
// RTCCALICFG as a clock-domain barrier. APLL sits behind the analog reg-I2C bus. All three
// answer 0: a wrong branch clock silently garbles the wire.
uint32_t arch_periph_clock_hz(uintptr_t base)
{
    if (base != mmap::UART0_BASE)
    {
        return 0;
    }
    uint32_t const sel = (r32(reg::rtc_cntl::CLK_CONF) >> reg::rtc_cntl::SOC_CLK_SEL_SHIFT)
                         & reg::rtc_cntl::SOC_CLK_SEL_MASK;
    if (sel != reg::rtc_cntl::SOC_CLK_SEL_PLL)
    {
        return 0;
    }
    return reg::system::APB_CLOCK_HZ;
}

#if KICKOS_NUM_CORES > 1
// PRID (Special Register 235) is loaded from pins, and the ISA states only that a processor's
// value is TYPICALLY 0..NPROCESSORS-1. This die's two are neither, and the TRM documents no
// per-core identity register: PRO_CPU reads 0x0000cdcd and APP_CPU 0x0000abab, measured on
// silicon.
//
// Refuses an unknown value: the kernel indexes per-core arrays with the answer. It parks
// instead of reporting because a secondary reaches this before its console exists.
uint32_t arch_cpu_id(void)
{
    constexpr uint32_t PRID_PRO_CPU = 0x0000cdcdu;
    constexpr uint32_t PRID_APP_CPU = 0x0000ababu;

    // Pins the assembly core index, which extracts one bit of PRID (chip_cpuid.h), against
    // these two values.
    static_assert(((PRID_PRO_CPU >> KICKOS_CHIP_CPUID_PRID_BIT) & 1u) == 0u,
                  "the assembly core index reads core 0 out of this bit of PRID");
    static_assert(((PRID_APP_CPU >> KICKOS_CHIP_CPUID_PRID_BIT) & 1u) == 1u,
                  "the assembly core index reads core 1 out of this bit of PRID");

    uint32_t prid = 0;
    __asm volatile("rsr.prid %0" : "=a"(prid));
    if (prid == PRID_PRO_CPU)
    {
        return 0u;
    }
    if (prid == PRID_APP_CPU)
    {
        return 1u;
    }
    while (true)
    {
        // waiti 15, never waiti 0: waiti writes PS.INTLEVEL from its immediate.
        __asm volatile("waiti 15");
    }
}
#endif

#if KICKOS_NUM_CORES > 1
// Release APP_CPU and WAIT FOR IT TO ARRIVE. Returns the number of cores that arrived,
// counting this one.
//
// Six writes are needed, not two: the clock gate resets closed, and the software stall is
// split across two RTC_CNTL registers whose release value the manual never states, only the
// value that stalls.
//
// Released is not arrived: every write below can succeed against a core that then executes
// nothing useful. The release ends in a bounded wait on a byte the far core writes only once
// it has seated its own vectors, coprocessor, mask and doorbell route.
uint32_t kickos_esp32_release_secondaries(void)
{
    uint32_t const entry = reinterpret_cast<uint32_t>(&_kickos_lx6_core1_entry);

    // 1. The boot address the ROM jumps to (Register 12.8).
    r32(reg::dport::APPCPU_CTRL_D) = entry;
    // 2 and 3. The software stall, both halves (Registers 9.1 and 9.34).
    r32(reg::rtc_cntl::OPTIONS0) =
        r32(reg::rtc_cntl::OPTIONS0) & ~reg::rtc_cntl::SW_STALL_APPCPU_C0_MASK;
    r32(reg::rtc_cntl::SW_CPU_STALL) =
        r32(reg::rtc_cntl::SW_CPU_STALL) & ~reg::rtc_cntl::SW_STALL_APPCPU_C1_MASK;
    // 4. The clock gate, which resets CLOSED (Register 12.6).
    r32(reg::dport::APPCPU_CTRL_B) = r32(reg::dport::APPCPU_CTRL_B) | reg::dport::APPCPU_CLKGATE_EN;
    // 5. Runstall (Register 12.7).
    r32(reg::dport::APPCPU_CTRL_C) = r32(reg::dport::APPCPU_CTRL_C) & ~reg::dport::APPCPU_RUNSTALL;
    // 6. Reset, asserted then released (Register 12.5, which resets ASSERTED).
    r32(reg::dport::APPCPU_CTRL_A) = r32(reg::dport::APPCPU_CTRL_A) | reg::dport::APPCPU_RESETTING;
    r32(reg::dport::APPCPU_CTRL_A) = r32(reg::dport::APPCPU_CTRL_A) & ~reg::dport::APPCPU_RESETTING;

    uint32_t arrived = 1; // this core
    for (uint32_t core = 1; core < KICKOS_NUM_CORES; core++)
    {
        uint64_t const deadline = arch_clock_now() + ESP32_ARRIVAL_WAIT_NS;
        while (kickos_lx6_core_arrived(core) == 0u)
        {
            if (arch_clock_now() > deadline)
            {
                arch_console_write_sync(NO_ARRIVE, sizeof(NO_ARRIVE) - 1);
                hex1(core);
                arch_console_write_sync(NL, sizeof(NL) - 1);
                kfault_terminate();
            }
        }
        arrived++;
    }
    return arrived;
}
#endif

void arch_init(void)
{
    wdt_disable();
    clock_init_240mhz();
    timg_clock_init();   // AFTER the PLL: its rate is off APB
    kickos_lx6_init();
    uart0_irq_setup();
#if KICKOS_NUM_CORES > 1
    // After the console is routed, so the release can report.
    uint32_t const arrived = kickos_esp32_release_secondaries();
    arch_console_write_sync(ARRIVED, sizeof(ARRIVED) - 1);
    hex1(arrived);
    arch_console_write_sync(ARRIVED_TAIL, sizeof(ARRIVED_TAIL) - 1);
    // After arrival: the check needs every peer's route live and its mask open.
    kickos_doorbell_selfcheck();
#endif
}

// The KICKOS_BENCH switch.S timestamps stay on raw CCOUNT: the top-level CMakeLists refuses
// that knob above one kernel core, so there is only ever one base to read.
uint64_t arch_clock_now(void)
{
    uint64_t ticks = timg_ticks();
    return kickos::arch_clk_mul_q32(ticks, TIMG_NS_MULT);
}

// TIMG0 T0, not CCOUNT: a cross-core trace needs one time base. The decoder derives the rate
// from the SESSION record, which carries this tick beside an arch_clock_now anchor.
uint32_t arch_trace_now(void)
{
    return static_cast<uint32_t>(timg_ticks());
}

int arch_console_write(char const* buf, size_t n)
{
    return console_tx_insert_line(buf, n, KICKOS_CONSOLE_CRLF);
}

// Overrides a fallback that would re-enter the buffered writer from the panic path.
bool arch_console_write_sync(char const* buf, size_t n)
{
    for (size_t i = 0; i < n; i++)
    {
        uint32_t spin = 0;
        while (((r32(reg::uart::STATUS) >> reg::uart::TXFIFO_CNT_S) & reg::uart::TXFIFO_CNT_MASK) >=
               reg::uart::TXFIFO_LIMIT)
        {
            // A local bound, not KICKOS_POLL_SPIN_MAX: this is a per-byte loop on the
            // panic path.
            if (++spin > 200000u)
            {
                return false; // bounded: a wedged UART must not hang the panic path (drop)
            }
        }
        r32(reg::uart::FIFO) = static_cast<uint8_t>(buf[i]);
    }
    return true;
}

// Block until UART0 is transmission-complete. STATUS.TXFIFO_CNT reaching 0 is only
// buffer-empty and leaves up to one frame still clocking out of the shifter;
// STATUS.ST_UTX_OUT is the transmitter state machine itself, idle only at its TX_IDLE
// encoding (TRM Register 19.8).
void arch_console_flush_sync(void)
{
    uint32_t spin = 0;
    while (true)
    {
        uint32_t const status = r32(reg::uart::STATUS);
        uint32_t const queued = (status >> reg::uart::TXFIFO_CNT_S) & reg::uart::TXFIFO_CNT_MASK;
        uint32_t const tx_fsm = (status >> reg::uart::ST_UTX_OUT_S) & reg::uart::ST_UTX_OUT_MASK;
        if (queued == 0 and tx_fsm == reg::uart::ST_UTX_OUT_IDLE)
        {
            return;
        }
        if (++spin > KICKOS_POLL_SPIN_MAX)
        {
            return; // bounded, as arch.h requires: a wedged UART drops the tail, never hangs
        }
    }
}

console_tx_backend const* arch_console_tx_backend(char** storage, uint32_t* size, int* irq_line)
{
    *storage = console_tx_buf;
    *size = KICKOS_CONSOLE_TX_SIZE;
    *irq_line = irq::CONSOLE_TX_LINE;
    return &esp32_console_backend;
}

void arch_shutdown(int status)
{
    (void)status;
    // WAITI writes PS.INTLEVEL from its immediate, so it must be 15, not 0 (waiti 0 would
    // unmask everything the rsil masked).
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
    kickos_crt_tail();
}

}
