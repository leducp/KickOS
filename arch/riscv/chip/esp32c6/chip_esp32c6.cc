// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// ESP32-C6 HP node backend: CLINT timer, UART0 console, interrupt routing, diagnostic LED,
// APM partition gate. Register addresses from ESP32-C6 TRM v1.2.

#if KICKOS_AMP_OWN_IMAGE
#if KICKOS_AMP_NODE_ID == 1
#define KICKOS_C6_LP_NODE 1
#endif
#endif
#ifndef KICKOS_C6_LP_NODE
#define KICKOS_C6_LP_NODE 0
#endif

#if !KICKOS_C6_LP_NODE

#include <kickos/arch/arch.h>
#include <arch_reboot_tail.h>
#include <kickos/diag.h>
#include "clint_rv32.h"
#include "crt_tail.h"
#include "pin_guard.h"
#include <kickos/arch/amp_shared.h>
#include <kickos/arch/rv_trap_ids.h>
#include <kickos/config/limits.h> // KICKOS_POLL_SPIN_MAX
#include <kickos/console_tx.h>
#include <kickos/io/mmio.h>
#include <kickos/kernel.h>
#include <kickos/sys/abi.h> // KOS_E* taxonomy (arch_pinmux_set)
#include <kickos/sys/atomic.h>

#include <stdint.h>

#include <kickos/chip_mmap.h>
#include "apm_rows.h"
#include "board_pins.h"
#include "irq.h"
#include "mtime_conv.h"
#include "regs/apm.h"
#include "regs/clint.h"
#include "regs/uart.h"
#include "esp_uart_console.h"
#include "regs/wdt.h"
#include "regs/intmtx.h"
#include "regs/intpri.h"
#include "regs/plic.h"
#include "regs/rmt.h"
#include "regs/pcr.h"
#include "regs/gpio.h"
#include "regs/io_mux.h"

namespace mmap = kickos::esp32c6::mmap;
namespace reg = kickos::esp32c6::reg;
namespace irq = kickos::esp32c6::irq;

extern "C"
{
    void kickos_rv32_init(void);
    extern volatile uint32_t* g_clint_msip;

    extern uint32_t _sidata, _sdata, _edata, _sbss, _ebss;
#if !KICKOS_HAVE_MPU && !KICKOS_AMP_OWN_IMAGE
    // Defined only in c6lpprobe's link (lp_probe_esp32c6.cc).
    void kickos_c6_lp_probe_boot(void) __attribute__((weak));
    // Defined only in c6txidle's link (txidle_probe_esp32c6.cc).
    void kickos_c6_txidle_probe(void) __attribute__((weak));
#endif
#if KICKOS_AMP_OWN_IMAGE
    extern uint8_t kickos_c6_amp_lp_stub_start[];
    extern uint8_t kickos_c6_amp_lp_stub_end[];
    extern kickos::Atomic<uint32_t, kickos::Order::RELAXED> kickos_c6_amp_rtc_hz;
#endif

#if KICKOS_HAVE_MPU
    // App-data NAPOT region (esp32c6.ld). LMA == VMA is asserted by the linker script, so
    // the .appdata copy is a no-op; the .appbss + pad zeroing is the real work.
    extern uint32_t _appdata_lma, __kickos_appdata_start, __kickos_appbss_start,
        __kickos_appdata_end;
#endif

    // CPU_CLK, read off PCR before the constructors run (mtime_conv.h); the ROM's PLL until then.
    uint32_t SystemCoreClock = kickos::esp32c6::PLL_ROOT_HZ;
}

namespace
{

    using kickos::esp32c6::mtime_ns_to_ticks;
    using kickos::esp32c6::mtime_ticks_to_ns;

    // MTIME's rate as MTIME_TOP_HZ >> g_mtime_shift, set before the constructors run.
    uint32_t g_mtime_shift = 0u;

    // -DKICKOS_C6_EARLY_MARK=1 emits one raw UART0 byte per boot stage (A..H). Must touch
    // no global: it runs before .data/.bss are live.
#ifndef KICKOS_C6_EARLY_MARK
#define KICKOS_C6_EARLY_MARK 0
#endif
#if KICKOS_C6_EARLY_MARK
    void c6_early_mark(char c)
    {
        uint32_t spin = 0;
        while (((r32(reg::uart::STATUS) >> reg::uart::TXFIFO_CNT_S) & reg::uart::TXFIFO_CNT_MASK) >=
               reg::uart::TXFIFO_LIMIT)
        {
            if (++spin > 200000u)
            {
                return;
            }
        }
        r32(reg::uart::FIFO) = static_cast<uint8_t>(c);
    }
#else
    inline void c6_early_mark(char) {}
#endif
    // TX-empty interrupt threshold. The condition is level but the INT_RAW bit is a latch:
    // enabling INT_ENA on an idle channel raises at once, and the source stays asserted
    // after the FIFO refills until INT_CLR is written (esp_uart_console.h).
    constexpr uint32_t CONSOLE_TXFIFO_EMPTY_THRHD = 32;

    // The window arch_console_reclaim rewrites, and the one a userspace console driver is
    // granted (c6uart). ONE constant: a reclaim reaching outside the window it reports would
    // rewrite registers whose holder was never checked. UART1 sits at base + 0x1000.
    constexpr uintptr_t CONSOLE_WIN_BASE = mmap::UART0_BASE;
    static_assert(KICKOS_BOARD_CONSOLE_BASE == CONSOLE_WIN_BASE, "the board's console is not the UART this backend drives");
    static_assert(KICKOS_BOARD_CONSOLE_TX_SELECT == 0 and KICKOS_BOARD_CONSOLE_RX_SELECT == 0,
                  "the console pads keep IO MUX function 0, their reset value, which this backend leaves");
    constexpr size_t CONSOLE_WIN_SIZE = KICKOS_BOARD_CONSOLE_SIZE;

    // Every offset the reclaim body writes must lie inside that window.
    static_assert(reg::uart::OFF_INT_ENA < CONSOLE_WIN_SIZE
                      and reg::uart::OFF_INT_CLR < CONSOLE_WIN_SIZE
                      and reg::uart::OFF_CLKDIV < CONSOLE_WIN_SIZE
                      and reg::uart::OFF_CONF0 < CONSOLE_WIN_SIZE
                      and reg::uart::OFF_SWFC_CONF0 < CONSOLE_WIN_SIZE
                      and reg::uart::OFF_IDLE_CONF < CONSOLE_WIN_SIZE
                      and reg::uart::OFF_RS485_CONF < CONSOLE_WIN_SIZE
                      and reg::uart::OFF_CLK_CONF < CONSOLE_WIN_SIZE
                      and reg::uart::OFF_REG_UPDATE < CONSOLE_WIN_SIZE,
                  "arch_console_reclaim writes outside the window it reports");

    // The ROM's baud divisor, captured by arch_init: its correct value depends on the PCR
    // clock source, which cannot be derived from the window. 0 = not captured, and the
    // reclaim leaves the divisor alone.
    constinit uint32_t g_console_clkdiv = 0;

    // A UART core whose synchronisation never completes must cost the panic path a bounded
    // delay, not the dump.
    constexpr uint32_t REG_UPDATE_SPIN_MAX = 200000u;

    void uart_reg_update_wait()
    {
        for (uint32_t i = 0; i < REG_UPDATE_SPIN_MAX; i++)
        {
            if ((r32(reg::uart::REG_UPDATE) & reg::uart::REG_UPDATE_BIT) == 0)
            {
                return;
            }
        }
    }

    // --- Inject doorbell. A level FROM_CPU source routed through the matrix to a CPU
    //     interrupt, which the C6 vectors as mcause = ID. ONE doorbell carries every logical
    //     inject line (arch keeps g_inject_line).

    // Enable, type, per-int priority and threshold go through the undocumented 0x2000_1000
    // window, INTPRI only for the FROM_CPU triggers. Do not "restore" this to INTPRI
    // (regs/plic.h).

    // Must be one of the C6's external IDs (1-2, 5-6, 8-31; CLINT owns 0/3/4/7) and distinct
    // from the switch.S demux (3=msip, 7=mtip).
    constexpr uint32_t DOORBELL_CPU_INT = KICKOS_RV_INJECT_DOORBELL_CPU_INT;
    constexpr uint32_t DOORBELL_PRIO = 7; // 1..15; same level as the device routes below

    // --- Real-device line routing. A line absent from the table stays on the software
    //     doorbell. map_reg selects cpu_int for one interrupt-matrix source (TRM 10.4.1).
    struct dev_route
    {
        int line;
        uintptr_t map_reg;
        uint32_t cpu_int;
        uint32_t prio; // 1..15
    };
    constexpr dev_route DEV_ROUTES[] = {
        {irq::UART0_TX_LINE, reg::intmtx::UART0_MAP, KICKOS_RV_DEV_CPU_INT, 7},
    };
    constexpr uint32_t DEV_ROUTE_COUNT = sizeof(DEV_ROUTES) / sizeof(DEV_ROUTES[0]);

    dev_route const* dev_route_of(int line)
    {
        for (uint32_t i = 0; i < DEV_ROUTE_COUNT; i++)
        {
            if (DEV_ROUTES[i].line == line)
            {
                return &DEV_ROUTES[i];
            }
        }
        return nullptr;
    }

    // --- UART0 sub-source demux table. INT_ST bit -> logical line (TRM Register 27.4).
    //     Every entry names the SAME grouped line, for the reason irq.h records.
    struct uart_subsource
    {
        uint32_t st_bit;
        int line;
    };
    constexpr uart_subsource UART0_SUBSOURCES[] = {
        {reg::uart::TXFIFO_EMPTY_INT, irq::UART0_TX_LINE},
        {reg::uart::RXFIFO_FULL_INT, irq::UART0_TX_LINE},
        {reg::uart::RXFIFO_TOUT_INT, irq::UART0_TX_LINE},
        {reg::uart::RXFIFO_OVF_INT, irq::UART0_TX_LINE},
        {reg::uart::FRM_ERR_INT, irq::UART0_TX_LINE},
        {reg::uart::PARITY_ERR_INT, irq::UART0_TX_LINE},
    };
    constexpr uint32_t UART0_SUBSOURCE_COUNT =
        sizeof(UART0_SUBSOURCES) / sizeof(UART0_SUBSOURCES[0]);
    // The demux tracks posted lines in a uint32_t bitmap, so every routed line must fit.
    static_assert(irq::UART0_TX_LINE >= 0 and irq::UART0_TX_LINE < 32,
                  "a demuxed logical line must fit the software-controller line space");

    // Done ONCE, while the kernel still owns the block: a repeat after the grant to a
    // userspace driver would write that driver's UART_INT_ENA/UART_INT_CLR.
    bool g_uart0_quiesced = false;

    void uart0_quiesce_once()
    {
        if (g_uart0_quiesced)
        {
            return;
        }
        g_uart0_quiesced = true;
        r32(reg::uart::INT_ENA) = 0;
        r32(reg::uart::INT_CLR) = 0xFFFFFFFFu;
        uint32_t conf1 = r32(reg::uart::CONF1);
        conf1 &= ~(reg::uart::TXFIFO_EMPTY_THRHD_MASK << reg::uart::TXFIFO_EMPTY_THRHD_S);
        conf1 |= (CONSOLE_TXFIFO_EMPTY_THRHD & reg::uart::TXFIFO_EMPTY_THRHD_MASK)
                 << reg::uart::TXFIFO_EMPTY_THRHD_S;
        r32(reg::uart::CONF1) = conf1;
    }

    // --- Watchdogs (regs/wdt.h; TRM ch.14 MWDT, ch.15 RWDT/SWD): the ROM arms them and
    //     each resets the part within seconds.
    void timg_mwdt_disable(uintptr_t base)
    {
        r32(base + reg::wdt::TIMG_WDTWPROTECT) = reg::wdt::WKEY;
        r32(base + reg::wdt::TIMG_WDTCONFIG0) &= ~(reg::wdt::TIMG_WDT_EN | reg::wdt::TIMG_WDT_FLASHBOOT);
        r32(base + reg::wdt::TIMG_WDTWPROTECT) = 0;
    }

    void wdt_disable_all()
    {
        timg_mwdt_disable(mmap::TIMG0_BASE);
        timg_mwdt_disable(mmap::TIMG1_BASE);
        r32(mmap::RTC_WDT_BASE + reg::wdt::RTC_WDT_WPROTECT) = reg::wdt::WKEY;
        r32(mmap::RTC_WDT_BASE + reg::wdt::RTC_WDT_CONFIG0) &= ~(reg::wdt::RTC_WDT_EN | reg::wdt::RTC_WDT_FLASHBOOT);
        r32(mmap::RTC_WDT_BASE + reg::wdt::RTC_WDT_WPROTECT) = 0;
        r32(mmap::RTC_WDT_BASE + reg::wdt::RTC_SWD_WPROTECT) = reg::wdt::WKEY;
        r32(mmap::RTC_WDT_BASE + reg::wdt::RTC_SWD_CONFIG) |= reg::wdt::RTC_SWD_DISABLE;
        r32(mmap::RTC_WDT_BASE + reg::wdt::RTC_SWD_WPROTECT) = 0;
    }

    // Entered with MIE still 0: the controller is configured with interrupts globally
    // disabled. mie bit N gates CPU int N.
    void inject_doorbell_init()
    {
        r32(reg::intmtx::FROM_CPU_0_MAP) = DOORBELL_CPU_INT;
        r32(reg::plic::MXINT_PRI_BASE + 4u * DOORBELL_CPU_INT) = DOORBELL_PRIO;
        r32(reg::plic::MXINT_TYPE) &= ~(1u << DOORBELL_CPU_INT); // level
        r32(reg::plic::MXINT_THRESH) = 0;                       // mask nothing: prio >= 0 always holds
        r32(reg::plic::MXINT_ENABLE) |= (1u << DOORBELL_CPU_INT);
        __asm volatile("fence" ::: "memory");                    // settle before MIE is enabled
        __asm volatile("csrs mie, %0" ::"r"(1u << DOORBELL_CPU_INT) : "memory");
    }

    // --- Diagnostic LED: the board's WS2812B (VDD tied to 3V3, no enable pin), through the
    //     RMT. GPIO bit-bang FAILS here: the register-write latency exceeds the ~400 ns bit
    //     high-time. Panic path: single frame, polled, no interrupts/DMA.

    // WS2812B pulse widths in RMT ticks. Clock: XTAL 40 MHz / group 1 / div_cnt 2 =
    // 20 MHz -> 50 ns/tick. Each 32-bit RAM word holds two {duration:15, level:1}
    // pulses (pulse0 = bits[15:0], pulse1 = bits[31:16]); a bit = high pulse then low
    // pulse. Periods land on 1.25 us; all within WS2812B tolerance.
    constexpr uint32_t RMT_DIV_CNT = 2;
    constexpr uint32_t WS_T0H = 8;    // 400 ns
    constexpr uint32_t WS_T0L = 17;   // 850 ns  (bit period 1.25 us)
    constexpr uint32_t WS_T1H = 16;   // 800 ns
    constexpr uint32_t WS_T1L = 9;    // 450 ns  (bit period 1.25 us)
    constexpr uint32_t WS_RESET = 1200; // 60 us low latch (>50 us), then end marker

    // R/W part of CH0CONF0 (WT bits held 0): div_cnt, 1 RAM block, idle drives low.
    constexpr uint32_t RMT_CH0_CFG =
        (RMT_DIV_CNT << reg::rmt::DIV_CNT_S) | (1u << reg::rmt::MEM_SIZE_S) | reg::rmt::IDLE_OUT_EN;

    inline uint32_t ws_word(uint32_t thigh, uint32_t tlow)
    {
        return thigh | (1u << 15) | (tlow << 16);
    }

    // Sent MSB first; the byte->channel mapping is the pixel's (this board is RGB, see
    // arch_diag_led_set).
    void rmt_send_ws2812(uint32_t color)
    {
        volatile uint32_t* ram = reinterpret_cast<volatile uint32_t*>(reg::rmt::CH0_RAM);
        for (int i = 0; i < 24; i++)
        {
            uint32_t bit = (color >> (23 - i)) & 1u;
            if (bit)
            {
                ram[i] = ws_word(WS_T1H, WS_T1L);
            }
            else
            {
                ram[i] = ws_word(WS_T0H, WS_T0L);
            }
        }
        // Latch entry: a long low, then a {0,0} pulse (duration 0 = stop marker).
        ram[24] = WS_RESET; // pulse0 = low 60 us; pulse1 = {0,0}

        r32(reg::rmt::INT_CLR) = reg::rmt::CH0_TX_END;
        r32(reg::rmt::CH0CONF0) = RMT_CH0_CFG | reg::rmt::MEM_RD_RST | reg::rmt::APB_MEM_RST;
        r32(reg::rmt::CH0CONF0) = RMT_CH0_CFG;
        r32(reg::rmt::CH0CONF0) = RMT_CH0_CFG | reg::rmt::CONF_UPDATE;
        r32(reg::rmt::CH0CONF0) = RMT_CH0_CFG | reg::rmt::TX_START;

        // Bounded so a wedged RMT never hangs the fault path.
        uint32_t spin = 0;
        while ((r32(reg::rmt::INT_RAW) & reg::rmt::CH0_TX_END) == 0)
        {
            if (++spin > 2000000u)
            {
                break;
            }
        }
    }
}

extern "C"
{

bool arch_console_write_sync(char const* buf, size_t n)
{
    return kickos::espuart::console::write_sync(buf, n);
}

// STATUS.TXFIFO_CNT reaching 0 leaves up to one frame still clocking out of the shifter;
// FSM_STATUS.ST_UTX_OUT idle means the transmitter is done.
void arch_console_flush_sync(void)
{
    uint32_t spin = 0;
    while (true)
    {
        uint32_t const queued = (r32(reg::uart::STATUS) >> reg::uart::TXFIFO_CNT_S)
                                & reg::uart::TXFIFO_CNT_MASK;
        uint32_t const tx_fsm = (r32(reg::uart::FSM_STATUS) >> reg::uart::ST_UTX_OUT_S)
                                & reg::uart::ST_UTX_OUT_MASK;
        if (queued == 0 and tx_fsm == reg::uart::ST_UTX_OUT_IDLE)
        {
            return;
        }
        if (++spin > KICKOS_POLL_SPIN_MAX)
        {
            return; // a wedged UART drops the tail, never hangs
        }
    }
}

uint64_t arch_clock_now(void)
{
    return mtime_ticks_to_ns(kickos::rv32::clint::read(reg::clint::MTIME), g_mtime_shift);
}

void arch_timer_arm(uint64_t deadline_ns)
{
    kickos::rv32::clint::arm(reg::clint::MTIMECMP, mtime_ns_to_ticks(deadline_ns, g_mtime_shift));
}

void arch_timer_disarm(void)
{
    kickos::rv32::clint::disarm(reg::clint::MTIMECMP);
}

// The ESP32-C6 HP core traps (illegal instruction) on `csrw mcounteren`, so the
// generic rv32 bring-up must not write it.
int arch_rv_has_mcounteren(void) { return 0; }

// The arch fallback raises SSIP, a no-op on this M/U-only core. The logical line is already in
// g_inject_line.
void arch_rv_inject_deliver(int line)
{
    (void)line;
    r32(reg::intpri::FROM_CPU_0) = 1;
}

// De-assert the level source so it does not re-fire on mret; the fence settles the APB write.
void arch_rv_ext_eoi(void)
{
    r32(reg::intpri::FROM_CPU_0) = 0;
    __asm volatile("fence" ::: "memory");
}

// The console is UART0 (CH343P bridge): the native USB-Serial-JTAG gates output on the host
// draining CDC and re-enumerates on reset. The ring drains via the TXFIFO_EMPTY interrupt on
// KICKOS_RV_DEV_CPU_INT.
console_tx_backend const* arch_console_tx_backend(int* irq_line)
{
#if !KICKOS_HAVE_MPU && !KICKOS_AMP_OWN_IMAGE
    // After the banner and before the ring, so the witness's line is the boot's own.
    if (kickos_c6_txidle_probe != nullptr)
    {
        kickos_c6_txidle_probe();
    }
#endif
    *irq_line = irq::UART0_TX_LINE;
    return &kickos::espuart::console::backend;
}

// The whole UART0 block c6uart is granted; the IRQ thread holds the grant, not the service
// thread whose death notes the console dead.
void arch_console_reclaim_window(uintptr_t* base, size_t* size)
{
    *base = CONSOLE_WIN_BASE;
    *size = CONSOLE_WIN_SIZE;
}

// Panic-path reclaim: force UART0 back to a polled-ready 8N1 channel after a userspace driver
// may have garbled every register in its window. Privileged, IRQs masked. MUST be idempotent
// and re-entrant: absolute stores only, no read-modify-write, since a nested-fault re-entry
// would repeat an RMW on a garbled value. Each store prevents a named silent-loss failure.
//
// The TX FIFO is NOT reset: after a panic from KERNEL_OWNED it holds bytes the TX ISR already
// removed from the ring, which kpanic_enter's flush cannot re-send; resetting would cut a hole
// in the log, keeping them costs ~11 ms of stale bytes ahead of the dump.
void arch_console_reclaim(void)
{
    // Silence first: a stale enabled source would storm the dispatcher through the whole
    // dump, and INT_ENA=0 makes every INT_ST bit read 0 whatever is latched.
    r32(reg::uart::INT_ENA) = 0;
    r32(reg::uart::INT_CLR) = 0xFFFFFFFFu;

    // TX_SCLK_EN clear stops the transmitter dead and TX_RST_CORE set holds it in reset.
    r32(reg::uart::CLK_CONF) = reg::uart::CLK_CONF_RUN;

    // The _SYNC stores below reach the UART core only on the final REG_UPDATE write, and the
    // PREVIOUS synchronisation must have completed first (TRM 27.5.2.2) or they are dropped.
    // The wait is bounded and read-only, so a re-entry repeats it harmlessly.
    uart_reg_update_wait();
    if (g_console_clkdiv != 0)
    {
        r32(reg::uart::CLKDIV) = g_console_clkdiv;
    }
    // TX_FLOW_EN alone gates every byte on a CTS this board does not wire, TXFIFO_RST held
    // asserted transmits nothing, and TXD_INV corrupts every frame.
    r32(reg::uart::CONF0) = reg::uart::CONF0_8N1;
    r32(reg::uart::SWFC_CONF0) = reg::uart::SWFC_CONF0_IDLE; // FORCE_XOFF stops the transmitter
    r32(reg::uart::RS485_CONF) = 0;                          // RS485 gates TX on a busy receiver
    r32(reg::uart::IDLE_CONF) = reg::uart::IDLE_CONF_DEFAULT; // TX_IDLE_NUM stretches to 1023 bit times
    r32(reg::uart::REG_UPDATE) = reg::uart::REG_UPDATE_BIT;
}

// The UART's own TXFIFO_EMPTY enable is toggled per burst by the block's owner.
void arch_rv_hw_unmask(int line)
{
    dev_route const* const r = dev_route_of(line);
    if (r == nullptr)
    {
        return;
    }
    if (line == irq::UART0_TX_LINE)
    {
        uart0_quiesce_once(); // a ROM-enabled source would storm the moment MIE is set
    }
    r32(r->map_reg) = r->cpu_int;
    r32(reg::plic::pri(r->cpu_int)) = r->prio;
    r32(reg::plic::MXINT_TYPE) &= ~(1u << r->cpu_int);   // level (cleared from source)
    r32(reg::plic::MXINT_ENABLE) |= (1u << r->cpu_int);
    __asm volatile("fence" ::: "memory"); // let the APB controller writes settle (TRM section 1.6.3.2)
    __asm volatile("csrs mie, %0" ::"r"(1u << r->cpu_int) : "memory");
}

// COARSE: the enable gates a whole CPU interrupt, so every matrix source routed to it is
// masked together (UART0 is the only one). Sub-source masking would need UART_INT_ENA, inside
// the block granted to the driver, so the C6 UART is one grouped logical line.
void arch_rv_hw_mask(int line)
{
    dev_route const* const r = dev_route_of(line);
    if (r == nullptr)
    {
        return;
    }
    // mie first: it takes effect immediately, while the controller write is APB and settles
    // over several cycles.
    __asm volatile("csrc mie, %0" ::"r"(1u << r->cpu_int) : "memory");
    r32(reg::plic::MXINT_ENABLE) &= ~(1u << r->cpu_int);
    __asm volatile("fence" ::: "memory"); // TRM section 1.6.3.2: settle before MIE is restored
}

// Reads UART_INT_ST once and posts each logical line an asserted sub-source belongs to, at
// most once per pass. INT_CLR is left to the block's owner: clearing here would consume
// latches (overrun/framing/parity counts) it must see. An asserted source outside the table
// still gets a post, so a bound handler masks it instead of re-entering forever.
void kickos_rv_ext_dispatch_dev(void)
{
    uint32_t const st = r32(reg::uart::INT_ST); // INT_RAW & INT_ENA (TRM Register 27.4)
    if (st == 0)
    {
        return;
    }
    uint32_t posted = 0;
    for (uint32_t i = 0; i < UART0_SUBSOURCE_COUNT; i++)
    {
        if ((st & UART0_SUBSOURCES[i].st_bit) == 0)
        {
            continue;
        }
        uint32_t const bit = 1u << UART0_SUBSOURCES[i].line;
        if ((posted & bit) != 0)
        {
            continue;
        }
        posted |= bit;
        kickos_isr_irq(UART0_SUBSOURCES[i].line);
    }
    if (posted == 0)
    {
        kickos_isr_irq(irq::UART0_TX_LINE);
    }
}

void arch_diag_led_init(void)
{
    r32(reg::pcr::RMT_CONF) |= reg::pcr::RMT_CLK_EN;
    r32(reg::pcr::RMT_CONF) |= reg::pcr::RMT_RST_EN;
    r32(reg::pcr::RMT_CONF) &= ~reg::pcr::RMT_RST_EN;
    // SCLK_SEL 3 = XTAL 40 MHz, group divisor 1 (DIV_NUM field 0).
    r32(reg::pcr::RMT_SCLK_CONF) =
        (3u << reg::pcr::RMT_SCLK_SEL_S) | (0u << reg::pcr::RMT_SCLK_DIV_NUM_S) | reg::pcr::RMT_SCLK_EN;

    r32(reg::rmt::SYS_CONF) |= reg::rmt::APB_FIFO_MASK;
    r32(reg::rmt::CH0CONF0) = RMT_CH0_CFG;                     // carrier_en defaults 1 -> cleared here
    r32(reg::rmt::CH0CONF0) = RMT_CH0_CFG | reg::rmt::CONF_UPDATE;

    r32(reg::gpio::func_out_sel_cfg(KICKOS_BOARD_LED_BIT)) = reg::gpio::RMT_SIG_OUT0_IDX;
    r32(reg::gpio::ENABLE_W1TS) = 1u << KICKOS_BOARD_LED_BIT;
    r32(reg::io_mux::gpio(KICKOS_BOARD_LED_BIT)) = reg::io_mux::MCU_SEL_GPIO | reg::io_mux::FUN_DRV_2;

    rmt_send_ws2812(0); // start dark
}

// The board's LED2 is RGB-ordered (red is the MSB byte), not the usual GRB.
void arch_diag_led_set(int on)
{
    uint32_t rgb = 0;
    if (on)
    {
        rgb = 0xFF0000u;
    }
    rmt_send_ws2812(rgb);
}

// Covers BOTH stages a pad passes: the IO_MUX pad function and the GPIO matrix out-sel. The
// matrix stage must be covered or the kernel-owned refusal is bypassable by aiming a
// peripheral signal at a kernel pin. func packs both (reg::gpio::PINMUX_*).
int arch_pinmux_set(uint32_t port, uint32_t pin, uint32_t func)
{
    if (port != 0u or pin > 30u)
    {
        return -KOS_EINVAL;
    }
    if ((func & reg::gpio::PINMUX_RESERVED) != 0u)
    {
        return -KOS_EINVAL;
    }
    uint32_t const out_sel = (func >> reg::gpio::PINMUX_OUT_SEL_S) & reg::gpio::PINMUX_OUT_SEL_MASK;
    if (out_sel > reg::gpio::PINMUX_OUT_SEL_MAX)
    {
        return -KOS_EINVAL;
    }
    if (kickos::board_pin_kernel_owned(port, pin))
    {
        return -KOS_EBUSY;
    }
    // Signal first, pad last: the pad must not be driven by whatever signal the ROM
    // left selected, even for the few cycles between the two writes.
    if ((func & reg::gpio::PINMUX_MATRIX_EN) != 0u)
    {
        r32(reg::gpio::func_out_sel_cfg(pin)) = out_sel;
    }
    r32(reg::io_mux::gpio(pin)) = func & reg::gpio::PINMUX_IO_MUX_MASK;
    return 0;
}

// Function clock feeding a peripheral block, read live from PCR (a reserved block, so a UART
// window holder cannot read it).
//
// TWO LEAVES CANNOT BE READ BACK and answer 0 rather than a guess: RC_FAST_CLK is adjustable
// (TRM 8.2.3) and its only register, PCR_FOSC_FREQ, is HRO; select 0 is no clock. A wrong
// branch clock silently garbles the wire. The crystal is readable (PCR_CLK_XTAL_FREQ, MHz).
uint32_t arch_periph_clock_hz(uintptr_t base)
{
    if (base != mmap::UART0_BASE)
    {
        return 0;
    }
    uint32_t const conf = r32(reg::pcr::UART0_SCLK_CONF);
    if ((conf & reg::pcr::UART0_SCLK_EN) == 0u)
    {
        return 0; // function clock gated
    }
    uint32_t src = 0;
    uint32_t const sel = (conf >> reg::pcr::UART0_SCLK_SEL_S) & reg::pcr::UART0_SCLK_SEL_MASK;
    if (sel == reg::pcr::SCLK_SEL_XTAL)
    {
        uint32_t const mhz =
            (r32(reg::pcr::SYSCLK_CONF) >> reg::pcr::CLK_XTAL_FREQ_S) & reg::pcr::CLK_XTAL_FREQ_MASK;
        src = mhz * 1000000u;
    }
    else if (sel == reg::pcr::SCLK_SEL_PLL_F80M)
    {
        src = reg::pcr::PLL_F80M_HZ;
    }
    if (src == 0u)
    {
        return 0;
    }
    // The TRM contradicts itself on which of DIV_A and DIV_B is the numerator (section 8.5.1
    // vs I2C 29.4.1 and RMT 37.3.3). With both zero the readings agree and the divisor is
    // DIV_NUM + 1; a non-zero fraction answers 0.
    uint32_t const div_a = (conf >> reg::pcr::UART0_SCLK_DIV_A_S) & reg::pcr::UART0_SCLK_DIV_A_MASK;
    uint32_t const div_b = (conf >> reg::pcr::UART0_SCLK_DIV_B_S) & reg::pcr::UART0_SCLK_DIV_B_MASK;
    if (div_a != 0u or div_b != 0u)
    {
        return 0;
    }
    uint32_t const div_num =
        (conf >> reg::pcr::UART0_SCLK_DIV_NUM_S) & reg::pcr::UART0_SCLK_DIV_NUM_MASK;
    return src / (div_num + 1u);
}

static void apm_write(uintptr_t at, uint32_t value)
{
    r32(at) = value;
}

// A refusal ahead of arch_init must not leave the ROM-armed watchdogs to reset into it again.
static void c6_refuse_n(char const* msg, size_t n)
{
    kpanic_enter();
    (void)kickos::kconsole_write(msg, n);
    wdt_disable_all();
    arch_shutdown(1);
}

static void c6_refuse(char const* msg)
{
    size_t n = 0;
    while (msg[n] != '\0')
    {
        n++;
    }
    c6_refuse_n(msg, n);
}

// Node 1's rows stay inert until arch_amp_release_peers moves the LP CPU to their mode.
static void apm_program_gate(void)
{
    namespace apm = kickos::esp32c6::apm;
    apm::Image image;
    apm::Refusal const refusal = apm::image_of(kickos_gate_rows, kickos_gate_row_count, image);
    char const* why = nullptr;
    switch (refusal)
    {
        case apm::Refusal::NONE:
        {
            break;
        }
        case apm::Refusal::GATE:
        {
            why = "KickOS: ESP32-C6 partition gate: a row names no APM gate\n";
            break;
        }
        case apm::Refusal::NODE:
        {
            why = "KickOS: ESP32-C6 partition gate: a row names a node with no security mode\n";
            break;
        }
        case apm::Refusal::BUDGET:
        {
            why = "KickOS: ESP32-C6 partition gate: more rows than an APM gate has regions\n";
            break;
        }
        case apm::Refusal::RANGE:
        {
            why = "KickOS: ESP32-C6 partition gate: a row's range is not an APM region\n";
            break;
        }
        case apm::Refusal::ACCESS:
        {
            why = "KickOS: ESP32-C6 partition gate: a row's access is not R, W and X\n";
            break;
        }
    }
    if (why != nullptr)
    {
        c6_refuse(why);
    }
    apm::program(image, apm_write);
    __asm volatile("fence" ::: "memory");
}

// MTIME counts CPU_CLK, so a CPU clock that is not 160 MHz >> n stops the boot.
//
// noinline: tests/static/check_image_rules.sh (c6_hp) finds its call in Reset_Handler.
static __attribute__((noinline)) void mtime_rate_init(void)
{
    uint32_t const sys = r32(reg::pcr::SYSCLK_CONF);
    uint32_t const cpu = r32(reg::pcr::CPU_FREQ_CONF);
    uint32_t const hz = kickos::esp32c6::cpu_clk_hz(
        (sys >> reg::pcr::SOC_CLK_SEL_S) & reg::pcr::SOC_CLK_SEL_MASK,
        (sys >> reg::pcr::CLK_XTAL_FREQ_S) & reg::pcr::CLK_XTAL_FREQ_MASK,
        (cpu >> reg::pcr::CPU_LS_DIV_NUM_S) & reg::pcr::CPU_LS_DIV_NUM_MASK,
        (cpu >> reg::pcr::CPU_HS_DIV_NUM_S) & reg::pcr::CPU_HS_DIV_NUM_MASK,
        (cpu & reg::pcr::CPU_HS_120M_FORCE) != 0u);
    int const shift = kickos::esp32c6::mtime_shift_of(hz);
    if (shift < 0)
    {
        char why[kickos::esp32c6::CLOCK_REFUSAL_MAX];
        c6_refuse_n(why, kickos::esp32c6::clock_refusal(why, hz, sys, cpu));
    }
    g_mtime_shift = static_cast<uint32_t>(shift);
    SystemCoreClock = hz;
}

void arch_init(void)
{
    wdt_disable_all(); // or the ROM-armed watchdogs reset the part in seconds
    c6_early_mark('E'); // watchdogs disabled

    // Before anything unprivileged exists: arch_console_reclaim has no other way back to a
    // working baud (see g_console_clkdiv).
    g_console_clkdiv = r32(reg::uart::CLKDIV);

    g_clint_msip = &r32(reg::clint::MSIP);
#if KICKOS_BENCH
    // The C6 traps on `rdcycle`; MTIME counts CPU cycles. Set before any switch.
    extern volatile uint32_t* g_bench_cycle_src;
    g_bench_cycle_src = &r32(reg::clint::MTIME);
#endif
    arch_timer_disarm();
    r32(reg::clint::MTIMECTL) = reg::clint::MTIMECTL_MTCE | reg::clint::MTIMECTL_MTIE;

    kickos_rv32_init();
    apm_program_gate();
    c6_early_mark('F');  // mtvec + mie + permissive bootstrap PMP installed
    inject_doorbell_init();
    c6_early_mark('G');  // inject doorbell wired

#if KICKOS_AMP_OWN_IMAGE
    // LP_TRIGGER_HP sets PMU_SW_INT. Route that level to an HP CPU vector
    // separate from the UART and software-inject vectors.
    r32(0x600B0164u) |= 1u << 29;
    r32(reg::intmtx::PMU_MAP) = KICKOS_RV_AMP_DOORBELL_CPU_INT;
    r32(reg::plic::MXINT_PRI_BASE + 4u * KICKOS_RV_AMP_DOORBELL_CPU_INT) = DOORBELL_PRIO;
    r32(reg::plic::MXINT_TYPE) &= ~(1u << KICKOS_RV_AMP_DOORBELL_CPU_INT);
    r32(reg::plic::MXINT_ENABLE) |= 1u << KICKOS_RV_AMP_DOORBELL_CPU_INT;
    __asm volatile("fence iorw, iorw" ::: "memory");
    __asm volatile("csrs mie, %0" : : "r"(1u << KICKOS_RV_AMP_DOORBELL_CPU_INT) : "memory");
#endif

#if !KICKOS_HAVE_MPU && !KICKOS_AMP_OWN_IMAGE
    if (kickos_c6_lp_probe_boot != nullptr)
    {
        kickos_c6_lp_probe_boot();
    }
#endif
}

// LP_AON_SYS_CFG_REG.HPSYS_SW_RESET (LP_AON +0x34, bit 31): a software reset of the HP system.
int arch_reboot(uint32_t outcome)
{
    if (outcome != KOS_SHUTDOWN_REBOOT)
    {
        return -KOS_ENOSYS;
    }
    constexpr uintptr_t LP_AON_SYS_CFG = mmap::LP_AON_BASE + 0x34u;
    constexpr uint32_t HPSYS_SW_RESET = 1u << 31;
    __asm volatile("csrci mstatus, 0x8" ::: "memory");
    r32(LP_AON_SYS_CFG) |= HPSYS_SW_RESET;
    kickos::arch::reset_settle();
    kickos::arch::reboot_failed(kickos::diag::kResetNotTaken);
}

void arch_shutdown(int status)
{
    (void)status;
    __asm volatile("csrci mstatus, 0x8" ::: "memory"); // mask interrupts (clear MIE)
    while (true)
    {
        __asm volatile("wfi");
    }
}

void Reset_Handler(void)
{
    c6_early_mark('A'); // reset entry reached (gp/sp/tp already set by _start)
    // LMA == VMA (ROM loader), so this copy is a no-op.
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
#if KICKOS_AMP_OWN_IMAGE && KICKOS_AMP_NODE_ID == 0
    arch_amp_shared_zero();
#endif
    c6_early_mark('B'); // .data copied + .bss zeroed
#if KICKOS_HAVE_MPU
    uint32_t* asrc = &_appdata_lma;
    uint32_t* adst = &__kickos_appdata_start;
    while (adst < &__kickos_appbss_start) // .appdata: LMA == VMA on this chip (see decl)
    {
        *adst++ = *asrc++;
    }
    for (uint32_t* b = &__kickos_appbss_start; b < &__kickos_appdata_end; b++)
    {
        *b = 0;
    }
    c6_early_mark('C'); // .appdata copied + .appbss zeroed (enforcement symbols sane)
#endif
    mtime_rate_init(); // a constructor may read SystemCoreClock
    kickos_crt_ctors();
    c6_early_mark('D'); // C++ static constructors (init_array) ran
    arch_init();
    c6_early_mark('H'); // arch_init returned, kmain next
    kickos::kmain(0, nullptr);
    arch_shutdown(0);
}

}

#if KICKOS_AMP_OWN_IMAGE
void arch_amp_release_peers(void)
{
    uintptr_t const stub_begin = reinterpret_cast<uintptr_t>(kickos_c6_amp_lp_stub_start);
    uintptr_t const stub_end = reinterpret_cast<uintptr_t>(kickos_c6_amp_lp_stub_end);
    uintptr_t const image_base = KICKOS_AMP_PARTITION_BASE + KICKOS_AMP_NODE_SHARE;
    uintptr_t const stub_size = stub_end - stub_begin;
    // The ROM placed both nodes' LOAD segments; node 0 copies only the reset vector to LP SRAM.
    if (r32(image_base) == 0u or stub_size == 0u or stub_size > 1024u
        or (stub_size & 3u) != 0u)
    {
        kpanic_enter();
        kickos::kputs("KickOS: ESP32-C6 AMP LP segment absent or stub invalid\n");
        arch_shutdown(1);
    }
    // The LP RTC rate depends on the ROM's clock selection and oscillator trim.
    constexpr uintptr_t rtc = 0x600B0C00u;
    auto rtc_ticks = [rtc]() -> uint64_t {
        r32(rtc + 0x10u) |= 1u << 28;
        uint32_t const lo = r32(rtc + 0x14u);
        uint32_t const hi = r32(rtc + 0x18u) & 0xFFFFu;
        return (static_cast<uint64_t>(hi) << 32) | lo;
    };
    uint64_t const start_ticks = rtc_ticks();
    uint64_t const start_ns = arch_clock_now();
    while (arch_clock_now() - start_ns < 20000000ull)
    {
    }
    uint64_t const end_ticks = rtc_ticks();
    uint64_t const elapsed_ns = arch_clock_now() - start_ns;
    uint32_t const rtc_hz = static_cast<uint32_t>(
        ((end_ticks - start_ticks) * 1000000000ull) / elapsed_ns);
    // LP_SLOW_CLK is RC_SLOW (136 kHz nominal) or a 32 kHz source: half the slowest to twice
    // the fastest.
    if (rtc_hz < 16000u or rtc_hz > 272000u)
    {
        kpanic_enter();
        kickos::kputs("KickOS: ESP32-C6 LP RTC rate invalid\n");
        arch_shutdown(1);
    }
    kickos_c6_amp_rtc_hz = rtc_hz;
    kickos::kprintf("# c6amp: LP RTC %u Hz\n", static_cast<unsigned>(rtc_hz));
    constexpr uintptr_t lp_mem = 0x50000000u;
    for (uintptr_t i = 0; i < stub_size; i += 4u)
    {
        r32(lp_mem + i) = r32(stub_begin + i);
    }
    r32(lp_mem + 0x204u) = 0;
    r32(lp_mem + 0x208u) = 0;
    r32(lp_mem + 0x20Cu) = 0;
    __asm volatile("fence iorw, iorw" ::: "memory");
    // Before the wake: node 1's rows are at its NODE_MODE, and no row grants its reset REE2.
    r32(reg::apm::LP_TEE_M0_MODE_CTRL) = kickos::esp32c6::apm::NODE_MODE[1];
    r32(0x600B1048u) = (r32(0x600B1048u) & ~(1u << 31)) | (1u << 30);
    r32(0x600B0174u) |= 1u << 31;
    r32(0x600B017Cu) |= 3u << 30;
    r32(0x600B0180u) |= 1u;
    __asm volatile("fence iorw, iorw" ::: "memory");
    r32(0x600B0184u) = 1u << 31;
}
#endif

#endif // !KICKOS_C6_LP_NODE
