// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// ESP32-D0WDQ6 (WROOM-32) chip backend. Register addresses are clean-room facts
// transcribed from the ESP32 TRM (register base addresses per TRM 1.5.3 "System
// and Memory / peripheral base"; UART/WDT register offsets per the UART and
// Watchdog chapters). Hand-rolled, no ESP-IDF/HAL sources.
//
// arch_init raises the CPU to the 240 MHz PLL (clock_init_240mhz); the ROM leaves
// UART0 (GPIO1 TX / GPIO3 RX) at 115200 and the console recomputes its divider for
// the 80 MHz APB after the switch. arch_console_write polls the UART0 TX FIFO.

#include <kickos/arch/arch.h>
#include <kickos/arch/clk_q32.h> // shared Q32 tickless-clock reciprocal + multiply
#include <kickos/console_tx.h>

#include <stdint.h>

namespace kickos
{
    int kmain(int argc, char** argv);
}

extern "C"
{
    void kickos_lx6_init(void);

    // Arm a chip device interrupt (here UART0 TX-empty) that the interrupt matrix
    // routes to a CPU interrupt number, binding the logical line its drain ISR is
    // attached to (arch/xtensa/lx6). Enables the CPU interrupt in INTENABLE once;
    // the per-transfer gate is the peripheral bit toggled by irq_enable/irq_disable.
    void kickos_lx6_bind_console_int(int cpu_int, int line);

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

    // --- UART0 (TRM UART chapter). ROM leaves it configured at 115200 8N1. ---
    constexpr uintptr_t UART0_BASE = 0x3FF40000;
    constexpr uintptr_t UART0_FIFO = UART0_BASE + 0x00;
    constexpr uintptr_t UART0_INT_ENA = UART0_BASE + 0x0C; // per-source interrupt enable
    constexpr uintptr_t UART0_INT_CLR = UART0_BASE + 0x10; // write-1-to-clear
    constexpr uintptr_t UART0_STATUS = UART0_BASE + 0x1C;
    constexpr uintptr_t UART0_CONF1 = UART0_BASE + 0x24;
    // STATUS.TXFIFO_CNT is bits [23:16]: bytes currently in the 128-entry TX FIFO.
    constexpr uint32_t TXFIFO_CNT_SHIFT = 16;
    constexpr uint32_t TXFIFO_CNT_MASK = 0xFF;
    constexpr uint32_t TXFIFO_LIMIT = 126; // leave headroom below the 128-deep FIFO

    // UART interrupt bit 1 = TX FIFO below the empty threshold. A LEVEL source: it
    // stays asserted while txfifo_cnt < CONF1.TXFIFO_EMPTY_THRHD, so INT_ENA (not
    // INT_CLR) is what gates it -- the K64F TDRE twin. This is the ONLY UART source
    // the console arms; the drain ISR clears it by dropping INT_ENA when the ring
    // empties.
    constexpr uint32_t UART_TXFIFO_EMPTY_INT = 1u << 1;
    // CONF1.TXFIFO_EMPTY_THRHD: fire while the FIFO holds fewer than this many bytes.
    // The TX FIFO is 128 deep, so the field is 7-bit (mask 0x7F), not 8. 64 keeps the
    // FIFO fed without one interrupt per byte -- it fits either width, so this mask is
    // conservative rather than behavior-changing. Exact field width/position is
    // HW-unverified (review checklist: "CONF1 THRHD width").
    constexpr uint32_t TXFIFO_EMPTY_THRHD_SHIFT = 8;
    constexpr uint32_t TXFIFO_EMPTY_THRHD_MASK = 0x7F;
    constexpr uint32_t TXFIFO_EMPTY_THRHD = 64;

    // Interrupt matrix (DPORT): route the UART0 peripheral interrupt SOURCE to a CPU
    // interrupt NUMBER on the PRO CPU. UART0's map register is DPORT+0x18C (peripheral
    // source 34). Target CPU interrupt 13: an external, LEVEL-triggered, level-1 line
    // (bit 13 is in KICKOS_L1_INT_MASK) and free here -- the timer owns internal int
    // 6, the software doorbell internal int 7, neither matrix-routable.
    constexpr uintptr_t DPORT_PRO_UART_INTR_MAP = 0x3FF0018C;
    constexpr uint32_t UART0_CPU_INT = 13;

    // Logical kernel IRQ line the console_tx drain ISR is bound to (irq_table index).
    // DISTINCT namespace from the CPU interrupt number above: on this arch the arch.h
    // irq_* seam is a software controller over logical lines, decoupled from the
    // physical Xtensa interrupts. Kept clear of the selftest's injected lines
    // (5/6/7/9/11) and the bench line (20).
    constexpr int CONSOLE_TX_LINE = 30;

    // --- Watchdogs. The ROM (running the image in flash-boot mode) leaves THREE
    //     watchdogs armed: the RTC WDT and the two Timer Group MWDTs (TIMG0, TIMG1).
    //     Each must be fully disabled or it resets the part within seconds of
    //     bring-up. Each register file is unlocked by writing its 32-bit write-
    //     protect key (default 0x50D83AA1), edited, then re-locked (write 0).
    //
    //     Clearing WDT_EN alone is NOT enough: the ROM arms the stage-0 watchdog
    //     via the separate FLASHBOOT_MOD_EN bit (a flash-boot watchdog independent
    //     of WDT_EN), which stays live until explicitly cleared. So each WDT needs
    //     both WDT_EN and FLASHBOOT_MOD_EN cleared. Bits/offsets are from the ESP32
    //     TRM (RTC_CNTL + TIMG watchdog register chapters). NOTE: the classic ESP32
    //     has NO RTC super-watchdog (SWD) --
    //     that block (RTC_CNTL_SWD_*) was introduced on the ESP32-S2 and later, so
    //     there is nothing more to disable here. ---
    constexpr uint32_t WDT_WKEY = 0x50D83AA1;         // RTC + TIMG MWDT write-protect key
    constexpr uint32_t WDT_EN = 1u << 31;             // {RTC,TIMG}_WDTCONFIG0.WDT_EN
    constexpr uint32_t RTC_WDT_FLASHBOOT_EN = 1u << 10;  // RTC_CNTL_WDT_FLASHBOOT_MOD_EN
    constexpr uint32_t TIMG_WDT_FLASHBOOT_EN = 1u << 14; // TIMG_WDT_FLASHBOOT_MOD_EN

    constexpr uintptr_t RTC_CNTL_BASE = 0x3FF48000;
    constexpr uintptr_t RTC_WDTCONFIG0 = RTC_CNTL_BASE + 0x8C;
    constexpr uintptr_t RTC_WDTWPROTECT = RTC_CNTL_BASE + 0xA4;

    // Timer groups are 0x1000 apart (TIMG0 @ 0x3FF5F000, TIMG1 @ 0x3FF60000).
    constexpr uintptr_t TIMG0_BASE = 0x3FF5F000;
    constexpr uintptr_t TIMG1_BASE = 0x3FF60000;
    constexpr uintptr_t TIMG_WDTCONFIG0_OFF = 0x48;
    constexpr uintptr_t TIMG_WDTWPROTECT_OFF = 0x64;

    void timg_wdt_disable(uintptr_t base)
    {
        r32(base + TIMG_WDTWPROTECT_OFF) = WDT_WKEY;
        r32(base + TIMG_WDTCONFIG0_OFF) &= ~(WDT_EN | TIMG_WDT_FLASHBOOT_EN);
        r32(base + TIMG_WDTWPROTECT_OFF) = 0;
    }

    void wdt_disable()
    {
        r32(RTC_WDTWPROTECT) = WDT_WKEY;
        r32(RTC_WDTCONFIG0) &= ~(WDT_EN | RTC_WDT_FLASHBOOT_EN);
        r32(RTC_WDTWPROTECT) = 0;

        timg_wdt_disable(TIMG0_BASE);
        timg_wdt_disable(TIMG1_BASE);
    }

    // --- CPU clock: raise the core from the ROM's 40 MHz XTAL to the 240 MHz PLL.
    //     The classic ESP32 makes 240 MHz from the 480 MHz BBPLL divided by 2. The
    //     BBPLL analog register file is NOT memory-mapped -- it is reached over the
    //     chip's internal "reg-I2C" bus, whose bit-level transaction lives in the
    //     ESP32 ROM. We call that ROM routine at its fixed entry (0x400041A4, the
    //     same symbol the IDF links as _regi2c_impl_write) rather than re-implement
    //     the analog-master protocol. Register addresses/bitfields and the 480 MHz/
    //     40 MHz-XTAL analog values are clean-room facts from the ESP32 TRM (RTC_CNTL
    //     + DPORT clock chapters, analog-PLL description). ---
    constexpr uint32_t CPU_CLOCK_HZ = 240000000u;
    constexpr uint32_t APB_CLOCK_HZ = 80000000u; // fixed 80 MHz on PLL (both 160/240 CPU)
    constexpr uint32_t CONSOLE_BAUD = 115200u;   // the baud the ROM left UART0 at

    constexpr uintptr_t RTC_CNTL_OPTIONS0 = RTC_CNTL_BASE + 0x00;
    constexpr uintptr_t RTC_CNTL_DBIAS_REG = RTC_CNTL_BASE + 0x7C; // core-voltage (dbias)
    constexpr uintptr_t RTC_CNTL_CLK_CONF = RTC_CNTL_BASE + 0x70;
    constexpr uint32_t RTC_CNTL_BIAS_I2C_FORCE_PD = 1u << 18; // internal reg-I2C bus PD
    constexpr uint32_t RTC_CNTL_BB_I2C_FORCE_PD = 1u << 6;
    constexpr uint32_t RTC_CNTL_BBPLL_I2C_FORCE_PD = 1u << 8;
    constexpr uint32_t RTC_CNTL_BBPLL_FORCE_PD = 1u << 10;
    constexpr uint32_t DIG_DBIAS_SHIFT = 11; // RTC_CNTL_DIG_DBIAS_WAK [13:11]
    constexpr uint32_t DIG_DBIAS_MASK = 0x7;
    constexpr uint32_t DIG_DBIAS_1V25 = 7;   // core voltage required for 240 MHz
    constexpr uint32_t SOC_CLK_SEL_SHIFT = 27; // RTC_CNTL_SOC_CLK_SEL [28:27]
    constexpr uint32_t SOC_CLK_SEL_MASK = 0x3;
    constexpr uint32_t SOC_CLK_SEL_PLL = 1;    // 0=XTAL 1=PLL 2=CK8M 3=APLL

    constexpr uintptr_t DPORT_CPU_PER_CONF = 0x3FF0003C;
    constexpr uint32_t CPUPERIOD_SEL_240 = 2; // CPUPERIOD_SEL [1:0]: 0=80 1=160 2=240

    // The classic-ESP32 BBPLL has NO memory-mapped lock/ready bit (esp-idf itself
    // times the PLL enable with a fixed delay). Its real robustness primitive is a
    // slow-clock-domain barrier: start a TIMG0 RTC calibration for 0 slow cycles and
    // wait for RDY, which the hardware sets on the next RTC-slow edge -- so the analog
    // writes we just made have provably latched across the clock-domain crossing
    // before we switch the CPU onto the PLL. RTCCALICFG = TIMG0 + 0x68.
    constexpr uintptr_t TIMG0_RTCCALICFG = TIMG0_BASE + 0x68;
    constexpr uint32_t RTC_CALI_START = 1u << 31; // CLK_SEL=[14:13]=0 (RTC_SLOW), MAX=[30:16]=0
    constexpr uint32_t RTC_CALI_RDY = 1u << 15;

    // Internal analog-I2C bus gate: bits [17:8] gate the analog blocks; bit 17 is
    // the BBPLL. Reset gates all, then ungate the BBPLL block before reg-I2C access.
    constexpr uintptr_t ANA_CONFIG_REG = 0x6000E044;
    constexpr uint32_t ANA_CONFIG_ALL_GATES = 0x3FFu << 8;
    constexpr uint32_t ANA_CONFIG_BBPLL_GATE = 1u << 17;

    // UART0 fractional clock divider: integer part [19:0], 1/16 fraction [23:20].
    constexpr uintptr_t UART0_CLKDIV = UART0_BASE + 0x14;

    // BBPLL over reg-I2C: block id 0x66, host id 4 (ESP32 TRM analog-PLL block).
    constexpr uint8_t I2C_BBPLL = 0x66;
    constexpr uint8_t I2C_BBPLL_HOSTID = 4;

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
        // ROM _regi2c_impl_write(block, host_id, reg_add, data) -- windowed ABI,
        // fixed ROM address. Does the whole analog reg-I2C transaction internally.
        auto rom_regi2c_write =
            reinterpret_cast<void (*)(uint8_t, uint8_t, uint8_t, uint8_t)>(0x400041A4u);
        rom_regi2c_write(I2C_BBPLL, I2C_BBPLL_HOSTID, reg_add, data);
    }

    // Wait one RTC-slow cycle so pending analog/RTC writes latch across the clock
    // domain. Bounded: if RDY never sets (e.g. slow clock stopped) it returns after
    // the cap rather than hanging -- the caller's fixed settle delay still covers it.
    void wait_slow_cycle()
    {
        r32(TIMG0_RTCCALICFG) = 0;               // CLK_SEL=RTC_SLOW, MAX=0, clear RDY/START
        r32(TIMG0_RTCCALICFG) = RTC_CALI_START;  // RDY sets on the next slow edge
        for (uint32_t i = 0; i < 200000u; i++)
        {
            if ((r32(TIMG0_RTCCALICFG) & RTC_CALI_RDY) != 0)
            {
                return;
            }
        }
    }

    void clock_init_240mhz()
    {
        // Open the internal reg-I2C bus to the BBPLL: reset gates all analog blocks,
        // then ungate BBPLL (bit 17).
        r32(ANA_CONFIG_REG) |= ANA_CONFIG_ALL_GATES;
        r32(ANA_CONFIG_REG) &= ~ANA_CONFIG_BBPLL_GATE;

        // Power up the reg-I2C bus and the BBPLL analog block (clear force-power-down).
        r32(RTC_CNTL_OPTIONS0) &= ~RTC_CNTL_BIAS_I2C_FORCE_PD;
        r32(RTC_CNTL_OPTIONS0) &=
            ~(RTC_CNTL_BB_I2C_FORCE_PD | RTC_CNTL_BBPLL_FORCE_PD | RTC_CNTL_BBPLL_I2C_FORCE_PD);
        wait_slow_cycle(); // the power-up must latch before the reg-I2C config writes

        // BBPLL reset/calibration defaults (byte offsets 0/1/4/10/12 in the block).
        bbpll_write(0, 0x18);  // IR_CAL_DELAY
        bbpll_write(1, 0x20);  // IR_CAL_EXT_CAP
        bbpll_write(4, 0x9A);  // OC_ENB_FCAL
        bbpll_write(10, 0x00); // OC_ENB_VCON
        bbpll_write(12, 0x00); // BBADC_CAL_7_0

        // Raise core voltage to 1.25 V BEFORE locking the PLL -- 240 MHz is unstable
        // at the XTAL-boot voltage. Still on the 40 MHz XTAL here, so 40 cyc/us.
        uint32_t dbias = r32(RTC_CNTL_DBIAS_REG);
        dbias &= ~(DIG_DBIAS_MASK << DIG_DBIAS_SHIFT);
        dbias |= DIG_DBIAS_1V25 << DIG_DBIAS_SHIFT;
        r32(RTC_CNTL_DBIAS_REG) = dbias;
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
        r32(DPORT_CPU_PER_CONF) = CPUPERIOD_SEL_240;
        uint32_t clk = r32(RTC_CNTL_CLK_CONF);
        clk &= ~(SOC_CLK_SEL_MASK << SOC_CLK_SEL_SHIFT);
        clk |= SOC_CLK_SEL_PLL << SOC_CLK_SEL_SHIFT;
        r32(RTC_CNTL_CLK_CONF) = clk;

        // CCOUNT/CCOMPARE0 now tick at 240 MHz: publish it so arch_xtensa.cc's
        // ns<->cycle math (which reads SystemCoreClock live) stays coherent.
        SystemCoreClock = CPU_CLOCK_HZ;
        delay_us(30, 240); // settle at the new clock (240 cyc/us now)

        // APB doubled 40->80 MHz, so the ROM's UART0 divider now halves the baud.
        // Drain any in-flight byte, then recompute CLKDIV for 80 MHz APB. clkdiv is
        // in 1/16 units: integer=[19:0], fraction=[23:20].
        while (((r32(UART0_STATUS) >> TXFIFO_CNT_SHIFT) & TXFIFO_CNT_MASK) != 0)
        {
        }
        uint32_t clkdiv16 = (APB_CLOCK_HZ << 4) / CONSOLE_BAUD;
        uint32_t integer = clkdiv16 >> 4;
        uint32_t frac = clkdiv16 & 0xF;
        r32(UART0_CLKDIV) = (frac << 20) | (integer & 0xFFFFF);
    }

    // --- Monotonic clock: TIMG0 timer T0, a 64-bit free-running up-counter -------
    // Replaces the weak CCOUNT-backed arch_clock_now (arch/xtensa/lx6). CCOUNT is a
    // 32-bit core cycle counter software-extended to 64 bits: a wrap not observed
    // within one 2^32-cycle window (~17.9 s at 240 MHz) is lost -- the same narrow-
    // counter + software-wrap-word class just moved off DWT on K64F (PIT) and off
    // CCU4 on XMC. A native 64-bit HW counter has NO software wrap word, so a
    // missed/aliased read cannot manufacture a phantom wrap. Bonus: CCOUNT is gated
    // by WAITI (the idle path clock-gates the core, freezing CCOUNT every idle); the
    // TIMG runs off APB, which keeps running in plain WAITI -- so this also removes
    // the lose-time-on-every-idle error the CCOUNT source carries.
    //
    // Register map (ESP32 TRM ch.18 "Timer Group", TIMGn_T0* at TIMG0_BASE offsets;
    // consistent with the WDT block at +0x48 already used here for the MWDT, so T0
    // owns +0x00..+0x20, T1 +0x24..+0x44, WDT +0x48+).
    constexpr uintptr_t TIMG0_T0CONFIG = TIMG0_BASE + 0x00; // TIMGn_T0CONFIG_REG
    constexpr uintptr_t TIMG0_T0LO = TIMG0_BASE + 0x04;     // latched low 32 bits (RO)
    constexpr uintptr_t TIMG0_T0HI = TIMG0_BASE + 0x08;     // latched high 32 bits (RO)
    constexpr uintptr_t TIMG0_T0UPDATE = TIMG0_BASE + 0x0C; // write -> latch count to LO/HI
    constexpr uintptr_t TIMG0_T0LOADLO = TIMG0_BASE + 0x18; // reload value, low
    constexpr uintptr_t TIMG0_T0LOADHI = TIMG0_BASE + 0x1C; // reload value, high
    constexpr uintptr_t TIMG0_T0LOAD = TIMG0_BASE + 0x20;   // write -> counter <- {HI,LO}

    // TIMGn_T0CONFIG_REG fields (ESP32 TRM ch.18): EN[31] start, INCREASE[30] count
    // up, AUTORELOAD[29] reload-on-alarm, DIVIDER[28:13] 16-bit APB prescaler,
    // ALARM_EN[10]. Free-running == INCREASE=1, AUTORELOAD=0, ALARM_EN=0.
    constexpr uint32_t TIMG_T0_EN = 1u << 31;
    constexpr uint32_t TIMG_T0_INCREASE = 1u << 30;
    constexpr uint32_t TIMG_T0_DIVIDER_SHIFT = 13;

    // Prescaler off the 80 MHz APB (fixed on the PLL for both 160/240 MHz CPU; see
    // clock_init_240mhz). Divider 2 is the minimum unambiguous value -- the divider
    // field treats 0/1 as special-cased on this silicon, so 2 sidesteps that trap
    // and gives the highest resolution: 80/2 = 40 MHz -> 25 ns/tick. A 64-bit
    // counter at 40 MHz wraps in ~4600 years, so there is no wrap concern at all.
    constexpr uint32_t TIMG_DIVIDER = 2;
    constexpr uint32_t TIMG_HZ = APB_CLOCK_HZ / TIMG_DIVIDER; // 40 MHz

    // ticks -> ns reciprocal multiply (the K64F pit pattern): ns = ticks*1e9/HZ via
    // mult = (1e9<<32)/HZ, ns = (ticks*mult)>>32, done as a 64x64->64 split so the
    // product never overflows. HZ is a compile-time constant here (APB is fixed on
    // the PLL), so the one divide folds at build time -- unlike K64F, whose bus
    // clock could be one of two runtime values.
    constexpr uint64_t TIMG_NS_MULT = kickos::arch_clk_recip_q32(TIMG_HZ);

    void timg_clock_init()
    {
        // Boot-order constraint: arch_clock_now MUST NOT run before this, and this
        // MUST run AFTER clock_init_240mhz (the counter rate is derived off the
        // 80 MHz PLL APB; running it on the 40 MHz XTAL APB would tick at half rate).
        // The TIMG0 APB clock is already live -- the ROM armed its MWDT and
        // clock_init_240mhz's wait_slow_cycle drives TIMG0 RTCCALICFG -- so no DPORT
        // peripheral-clock ungate is needed here.
        // Free-running up-counter: no alarm, no autoreload, prescaler = TIMG_DIVIDER.
        r32(TIMG0_T0CONFIG) = TIMG_T0_INCREASE | (TIMG_DIVIDER << TIMG_T0_DIVIDER_SHIFT);
        r32(TIMG0_T0LOADLO) = 0;
        r32(TIMG0_T0LOADHI) = 0;
        r32(TIMG0_T0LOAD) = 1; // any write loads the counter from {LOADHI,LOADLO} = 0
        r32(TIMG0_T0CONFIG) =
            TIMG_T0_EN | TIMG_T0_INCREASE | (TIMG_DIVIDER << TIMG_T0_DIVIDER_SHIFT);
    }

    // Read the 64-bit T0 count. The live counter is NOT directly readable: write
    // T0UPDATE to latch it into the T0LO/T0HI shadow regs, THEN read LO+HI (the
    // LTMR64 twin). A bare LO/HI read without the latch is stale. The whole
    // latch-then-read runs under the crit section: the LO/HI shadow is one shared
    // resource, so an interleaved reader's UPDATE landing between our LO and HI
    // reads would tear the pair across a low-word rollover -- exactly the torn read
    // this change exists to remove. On the classic ESP32 T0UPDATE has no ready/self-
    // clearing bit (that is an S2/S3 addition); a single write latches synchronously.
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
    // disable gate UART_TXFIFO_EMPTY_INT AT THE PERIPHERAL (the CPU line stays armed
    // in INTENABLE). ---
    int esp32_tx_slot_free(void)
    {
        return ((r32(UART0_STATUS) >> TXFIFO_CNT_SHIFT) & TXFIFO_CNT_MASK) < TXFIFO_LIMIT;
    }
    void esp32_tx_push(uint8_t b) { r32(UART0_FIFO) = b; }
    void esp32_tx_irq_enable(void)
    {
        r32(UART0_INT_ENA) = r32(UART0_INT_ENA) | UART_TXFIFO_EMPTY_INT;
    }
    void esp32_tx_irq_disable(void)
    {
        r32(UART0_INT_ENA) = r32(UART0_INT_ENA) & ~UART_TXFIFO_EMPTY_INT;
    }

    constexpr uint32_t CONSOLE_TX_SIZE = 512; // power of two; > kprintf's 256B buffer
    char console_tx_buf[CONSOLE_TX_SIZE];
    console_tx_backend const esp32_console_backend = {
        esp32_tx_slot_free, esp32_tx_push, esp32_tx_irq_enable, esp32_tx_irq_disable};

    // One-time UART0 TX-interrupt wiring: TX-empty disabled + any ROM-left latch
    // cleared, the empty threshold set, the peripheral source routed through the
    // matrix to CPU interrupt 13, and that CPU line armed in INTENABLE (the arch
    // records the CPU int + logical line for the level-1 dispatcher). No source
    // asserts until console_tx_write sets INT_ENA, which only runs once the ring is
    // armed and the drain ISR attached (console_buffer_init, later in kmain).
    void uart0_irq_setup()
    {
        // The console owns UART0, so silence every source (the ROM polls -- no IRQs)
        // and ack anything it left latched. Critical: CPU int 13 is armed below while
        // the ring is still unarmed, so a stale ROM-enabled source would storm the
        // level-1 dispatcher. console_tx_write re-enables ONLY TXFIFO_EMPTY, later.
        r32(UART0_INT_ENA) = 0;
        r32(UART0_INT_CLR) = 0xFFFFFFFFu;

        uint32_t conf1 = r32(UART0_CONF1);
        conf1 &= ~(TXFIFO_EMPTY_THRHD_MASK << TXFIFO_EMPTY_THRHD_SHIFT);
        conf1 |= (TXFIFO_EMPTY_THRHD & TXFIFO_EMPTY_THRHD_MASK) << TXFIFO_EMPTY_THRHD_SHIFT;
        r32(UART0_CONF1) = conf1;

        r32(DPORT_PRO_UART_INTR_MAP) = UART0_CPU_INT;
        kickos_lx6_bind_console_int(static_cast<int>(UART0_CPU_INT), CONSOLE_TX_LINE);
    }
}

extern "C"
{

// --- Kernel diagnostic LED: the onboard LED on GPIO2 (active-high; DOIT ESP32
//     DevKit v1 / NodeMCU-32S blue LED). GPIO
//     bank-0 write-1-to-set/clear registers; GPIO2's pad is switched to the GPIO
//     function via its IO_MUX register (MCU_SEL=GPIO(2), driver strength 2, input
//     disabled). Base 0x3FF4_4000 (GPIO) / 0x3FF4_9000 (IO_MUX).
constexpr uintptr_t GPIO_OUT_W1TS = 0x3FF44008;
constexpr uintptr_t GPIO_OUT_W1TC = 0x3FF4400C;
constexpr uintptr_t GPIO_ENABLE_W1TS = 0x3FF44024;
constexpr uintptr_t IO_MUX_GPIO2 = 0x3FF49000 + 0x40;
constexpr uint32_t LED_BIT = 1u << 2;
constexpr uint32_t IO_MUX_GPIO_FUNC = (2u << 12) | (2u << 10); // MCU_SEL=GPIO, DRV=2, IE off

void arch_diag_led_init(void)
{
    r32(IO_MUX_GPIO2) = IO_MUX_GPIO_FUNC;
    r32(GPIO_ENABLE_W1TS) = LED_BIT;
    r32(GPIO_OUT_W1TC) = LED_BIT; // start dark
}

void arch_diag_led_set(int on)
{
    if (on)
    {
        r32(GPIO_OUT_W1TS) = LED_BIT;
    }
    else
    {
        r32(GPIO_OUT_W1TC) = LED_BIT;
    }
}

void arch_init(void)
{
    // FP: the LX6 single-precision FPU (coprocessor 0) IS enabled for all threads
    // (kickos_lx6_init sets CPENABLE), so `float` works here exactly as it does on
    // the Cortex-M4F boards and via soft-float on the M0/M3 boards -- a thread that
    // compiles clean on one KickOS board must not fault on another. The FP data
    // registers are caller-saved on Xtensa (the compiler spills live f-regs around
    // any call), so the COOPERATIVE switch needs no FP handling; only the PREEMPTIVE
    // path banks them -- the level-1 interrupt frame saves/restores f0-f15+FCR+FSR
    // (startup.S). Double stays soft-float (__muldf3): the LX6 FPU is single-only.
    wdt_disable();
    clock_init_240mhz(); // 40 MHz XTAL -> 240 MHz PLL; updates SystemCoreClock + UART0 baud
    timg_clock_init();   // 64-bit monotonic time base; AFTER the PLL (rate is off APB)
    kickos_lx6_init();
    uart0_irq_setup(); // route + arm the UART0 TX-empty interrupt for the console ring
}

// Monotonic clock override: convert the free-running TIMG0 T0 64-bit count (40 MHz,
// off the fixed 80 MHz APB) to ns via the cached reciprocal multiply, replacing the
// weak CCOUNT-backed arch_clock_now (arch/xtensa/lx6) whose 32-bit + software-wrap
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

// Synchronous polled writer -- the panic / fault / pre-arm path (console.cc selects
// it when the ring is unarmed or in ISR/panic context). Overrides the weak default
// that would otherwise re-enter the buffered arch_console_write.
void arch_console_write_sync(char const* buf, size_t n)
{
    for (size_t i = 0; i < n; i++)
    {
        uint32_t spin = 0;
        while (((r32(UART0_STATUS) >> TXFIFO_CNT_SHIFT) & TXFIFO_CNT_MASK) >= TXFIFO_LIMIT)
        {
            if (++spin > 200000u)
            {
                return; // bounded: a wedged UART must not hang the panic path (drop)
            }
        }
        r32(UART0_FIFO) = static_cast<uint8_t>(buf[i]);
    }
}

console_tx_backend const* arch_console_tx_backend(char** storage, uint32_t* size, int* irq_line)
{
    *storage = console_tx_buf;
    *size = CONSOLE_TX_SIZE;
    *irq_line = CONSOLE_TX_LINE;
    return &esp32_console_backend;
}

void arch_shutdown(int status)
{
    (void)status; // no exit on bare metal
    // RSIL 15 then WAITI 15: mask everything (incl. below NMI) and park -- the
    // RP2040/K64F "cpsid i; wfi" twin. WAITI writes PS.INTLEVEL from its immediate,
    // so it must be 15, not 0 (waiti 0 would unmask everything the rsil masked).
    __asm volatile("rsil a0, 15" ::: "a0", "memory");
    while (true)
    {
        __asm volatile("waiti 15");
    }
}

void Reset_Handler(void)
{
    // .data is already in RAM (the image links data at its VMA), but keep the copy
    // loop for uniformity with the ARM ports -- it is a no-op when LMA == VMA.
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
