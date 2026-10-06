// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Core-local CLINT (TRM ch.1.7): MSIP = the deferred-switch software interrupt
// (mcause 3), MTIME/MTIMECMP = the tickless timer (mcause 7). MTIME does not run
// until MTIMECTL.MTCE is set. The console is UART0, bridged to the host by the
// board's CH343P, NOT the native USB Serial/JTAG (see arch_console_write). Device
// IRQs are enabled through the undocumented 0x2000_1000 controller window, not the
// documented INTPRI block (see regs/plic.h and inject_doorbell_init).
//
// Register addresses: ESP32-C6 TRM v1.2 (memory map Table 5.3-2; CLINT ch.1.7;
// watchdogs ch.14/15; UART ch.27; INTMTX ch.10 + section 1.6). Hand-rolled, no
// ESP-IDF/HAL sources.

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
#include <kickos/arch/pin_guard.h>
#include <kickos/arch/amp_shared.h>
#include <kickos/arch/rv_trap_ids.h>
#include <kickos/config/limits.h> // KICKOS_POLL_SPIN_MAX
#include <kickos/console_tx.h>
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

namespace kickos
{
    int kmain(int argc, char** argv);
}

extern "C"
{
    void kickos_rv32_init(void);
    extern volatile uint32_t* g_clint_msip;

    extern uint32_t _sidata, _sdata, _edata, _sbss, _ebss;
#if !KICKOS_HAVE_MPU && !KICKOS_AMP_OWN_IMAGE
    // Defined by lp_probe_esp32c6.cc, which only c6lpprobe's link extracts.
    void kickos_c6_lp_probe_boot(void) __attribute__((weak));
    // Defined by txidle_probe_esp32c6.cc, which only c6txidle's link extracts.
    void kickos_c6_txidle_probe(void) __attribute__((weak));
#endif
#if KICKOS_AMP_OWN_IMAGE
    extern uint8_t kickos_c6_amp_lp_stub_start[];
    extern uint8_t kickos_c6_amp_lp_stub_end[];
    extern kickos::Atomic<uint32_t, kickos::Order::RELAXED> kickos_c6_amp_rtc_hz;
#endif
    extern void (*__init_array_start[])();
    extern void (*__init_array_end[])();

    // DWARF EH frame table (esp32c6.ld) + the libgcc registrar. The -nostartfiles link
    // drops crtbegin, so its frame_dummy never registers .eh_frame and a full-C++ app
    // must register it by hand at boot. WEAK ref: a freestanding image references no
    // _Unwind_*, the registrar object is never pulled, and the call is skipped.
    extern uint32_t __eh_frame_start;
    // NOT one of the bounds include/kickos/klink.h makes strong: this symbol is libgcc's
    // and optional by libgcc's own contract, so no linker script can state it.
    void __register_frame(void*) __attribute__((weak));
#if KICKOS_HAVE_MPU
    // App-data NAPOT region (esp32c6.ld). .appdata holds the app + C++-runtime .data and
    // the gp small-data window. No AT clause on any loadable section of this chip (the ROM
    // loader places every segment at its VMA), so LMA == VMA is pinned by an ASSERT and the
    // copy is a no-op, like .data's; the .appbss + pad zero after it is the real work.
    extern uint32_t _appdata_lma, __kickos_appdata_start, __kickos_appbss_start,
        __kickos_appdata_end;
#endif

    // CPU_CLK, read off PCR by arch_init (mtime_conv.h).
    uint32_t SystemCoreClock = 0u;
}

namespace
{
    inline volatile uint32_t* r32p(uintptr_t a) { return reinterpret_cast<volatile uint32_t*>(a); }
    inline volatile uint32_t& r32(uintptr_t a) { return *r32p(a); }

    using kickos::esp32c6::mtime_ns_to_ticks;
    using kickos::esp32c6::mtime_ticks_to_ns;

    // MTIME's rate as MTIME_TOP_HZ >> g_mtime_shift, set by arch_init before MTIME counts.
    uint32_t g_mtime_shift = 0u;

    // --- UART0 console (regs/uart.h; TRM ch.27; base 0x6000_0000), on the board's console
    //     pins. The ROM already sets UART0 up (baud/pins) for its own
    //     boot log, so pushing bytes needs no setup: poll STATUS.TXFIFO_CNT for room,
    //     write the FIFO. FIFO depth 128.

    // --- Early boot markers (raw UART0, pre-console). Default OFF: build with
    //     -DKICKOS_C6_EARLY_MARK=1 to emit a byte at each boot stage (A..H). The ROM
    //     leaves UART0 up and _start sets gp/sp before Reset_Handler, so a byte reaches
    //     the TX FIFO before the console exists. Touches no global, so it is safe before
    //     .data/.bss are live. Bounded spin: a wedged FIFO never blocks boot.
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
    // UART0 TX-empty interrupt (buffered console ring). The CONDITION is level (TX FIFO
    // count below CONF1.TXFIFO_EMPTY_THRHD, TRM section 27.4.11), but the INT_RAW bit it
    // sets is a LATCH, so enabling INT_ENA on an idle channel raises at once AND the source
    // stays asserted after the FIFO refills until INT_CLR is written (c6_tx_push).
    constexpr uint32_t CONSOLE_TXFIFO_EMPTY_THRHD = 32;   // re-fire when the FIFO drains to <=32

    // The window arch_console_reclaim rewrites, and the one a userspace console driver is
    // granted (c6uart). ONE constant: a reclaim
    // reaching outside the window it reports would rewrite registers whose holder was
    // never checked. UART1 sits at base + 0x1000, outside it.
    constexpr uintptr_t CONSOLE_WIN_BASE = mmap::UART0_BASE;
    static_assert(KICKOS_BOARD_CONSOLE_BASE == CONSOLE_WIN_BASE, "the board's console is not the UART this backend drives");
    static_assert(KICKOS_BOARD_CONSOLE_TX_SELECT == 0 and KICKOS_BOARD_CONSOLE_RX_SELECT == 0,
                  "the console pads keep IO MUX function 0, their reset value, which this backend leaves");
    constexpr size_t CONSOLE_WIN_SIZE = 0x1000u;

    // Every offset the reclaim body writes must lie inside that window. Adding a store
    // outside it fails to build instead of silently widening the reclaim's reach.
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

    // The ROM's baud divisor, captured by arch_init. KickOS never programs UART0, so this
    // is the only record of the working value: the divisor's correct setting depends on
    // the clock source PCR selected, which is not derivable from anything in the window.
    // 0 means arch_init has not run yet, and the reclaim then leaves the divisor alone.
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

    // --- Watchdogs (regs/wdt.h; TRM ch.14 MWDT, ch.15 RWDT/SWD). ALL must be disabled
    //     or the ROM-armed WDTs reset the part within seconds. Common unlock key 0x50D83AA1.

    // --- Interrupt matrix (INTMTX) + local interrupt controller (INTPRI). The C6 has no
    //     S-mode, so the arch's SSIP inject channel is a no-op here and a REAL machine
    //     interrupt is raised instead. A software-settable FROM_CPU source (level) is
    //     routed through the matrix to a dedicated CPU interrupt ID, which the C6 core
    //     vectors as mcause = ID, not the standard mcause = 11. ONE doorbell carries every
    //     logical inject line (arch keeps g_inject_line).

    // Enable, type, per-int priority and threshold are all driven through the
    // undocumented 0x2000_1000 controller window (regs/plic.h), not the documented INTPRI
    // block at 0x600C_5000; INTPRI is touched only for its FROM_CPU source triggers.
    // Do not "restore" this to INTPRI without a bench read-back (regs/plic.h).

    // Dedicated CPU interrupt ID for the inject doorbell. Must be one of the C6's
    // external IDs (1-2, 5-6, 8-31; local CLINT owns 0/3/4/7) and not collide with
    // the switch.S demux (3=msip, 7=mtip). Shared with switch.S's .Lext arm via
    // rv_trap_ids.h.
    constexpr uint32_t DOORBELL_CPU_INT = KICKOS_RV_INJECT_DOORBELL_CPU_INT;
    constexpr uint32_t DOORBELL_PRIO = 7; // 1..15; same level as the device routes below

    // --- Real-device line routing. One entry per logical line that reaches hardware; a
    //     line absent from the table has no routing and stays on the software doorbell.
    //     map_reg selects the target CPU interrupt for one interrupt-matrix source (TRM
    //     section 10.4.1); cpu_int is then configured in the CPU interrupt controller.
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

    // UART0 quiesce, done ONCE and only while the kernel still owns the block: silence
    // every source, drop the ROM's latches, and own the TX-empty threshold. Repeating it
    // on a later rearm would write UART_INT_ENA/UART_INT_CLR after the block has been
    // granted to a userspace driver, which is the driver's register, not the kernel's.
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
        // RTC (LP) watchdog.
        r32(mmap::RTC_WDT_BASE + reg::wdt::RTC_WDT_WPROTECT) = reg::wdt::WKEY;
        r32(mmap::RTC_WDT_BASE + reg::wdt::RTC_WDT_CONFIG0) &= ~(reg::wdt::RTC_WDT_EN | reg::wdt::RTC_WDT_FLASHBOOT);
        r32(mmap::RTC_WDT_BASE + reg::wdt::RTC_WDT_WPROTECT) = 0;
        // Super watchdog (SWD): set the disable bit (its own write-protect key).
        r32(mmap::RTC_WDT_BASE + reg::wdt::RTC_SWD_WPROTECT) = reg::wdt::WKEY;
        r32(mmap::RTC_WDT_BASE + reg::wdt::RTC_SWD_CONFIG) |= reg::wdt::RTC_SWD_DISABLE;
        r32(mmap::RTC_WDT_BASE + reg::wdt::RTC_SWD_WPROTECT) = 0;
    }

    // One-time inject-doorbell wiring, entered with MIE still 0, per the TRM's
    // "configure the interrupt controller with interrupts globally disabled" rule. The
    // C6's mie bit N gates CPU int N, with bits 3/7 = the standard msip/mtip. FROM_CPU_0
    // is left de-asserted.
    void inject_doorbell_init()
    {
        r32(reg::intmtx::FROM_CPU_0_MAP) = DOORBELL_CPU_INT;     // route the source -> CPU int
        r32(reg::plic::MXINT_PRI_BASE + 4u * DOORBELL_CPU_INT) = DOORBELL_PRIO;
        r32(reg::plic::MXINT_TYPE) &= ~(1u << DOORBELL_CPU_INT); // level
        r32(reg::plic::MXINT_THRESH) = 0;                       // mask nothing: prio >= 0 always holds
        r32(reg::plic::MXINT_ENABLE) |= (1u << DOORBELL_CPU_INT); // enable at the controller
        __asm volatile("fence" ::: "memory");                    // settle before MIE is enabled
        __asm volatile("csrs mie, %0" ::"r"(1u << DOORBELL_CPU_INT) : "memory");
    }

    // --- Diagnostic LED: the board's addressable WS2812B (VDD tied to 3V3, no enable pin).
    //     GPIO bit-bang FAILS here: the register-write latency exceeds
    //     the WS2812B ~400 ns bit high-time even at 160 MHz, so software cannot form
    //     valid bits (LED latched solid white). The RMT peripheral (regs/rmt.h) clocks
    //     the pulse train in hardware. Panic path: single frame, polled, no
    //     interrupts/DMA.

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
        // pulse0 = high for thigh (level 1), pulse1 = low for tlow (level 0).
        return thigh | (1u << 15) | (tlow << 16);
    }

    // Encode a 24-bit colour into the channel-0 RAM and transmit it (blocking poll).
    // Sent MSB first; the byte->channel mapping is the pixel's (this board is RGB, see
    // arch_diag_led_set).
    void rmt_send_ws2812(uint32_t color)
    {
        volatile uint32_t* ram = reinterpret_cast<volatile uint32_t*>(reg::rmt::CH0_RAM);
        for (int i = 0; i < 24; i++)
        {
            uint32_t bit = (color >> (23 - i)) & 1u; // MSB first
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

        r32(reg::rmt::INT_CLR) = reg::rmt::CH0_TX_END;                        // clear stale done flag
        r32(reg::rmt::CH0CONF0) = RMT_CH0_CFG | reg::rmt::MEM_RD_RST | reg::rmt::APB_MEM_RST; // reset RAM pointers
        r32(reg::rmt::CH0CONF0) = RMT_CH0_CFG;
        r32(reg::rmt::CH0CONF0) = RMT_CH0_CFG | reg::rmt::CONF_UPDATE;        // latch config
        r32(reg::rmt::CH0CONF0) = RMT_CH0_CFG | reg::rmt::TX_START;           // go

        // Blocking (panic ctx: interrupts masked, no DMA). Bounded so a wedged RMT never
        // hangs the fault path; ~25 words * 1.25 us + 60 us latch is < 100 us.
        uint32_t spin = 0;
        while ((r32(reg::rmt::INT_RAW) & reg::rmt::CH0_TX_END) == 0)
        {
            if (++spin > 2000000u)
            {
                break;
            }
        }
    }

#define KICKOS_RESERVED_RUN(port_base, first, last) or ((port_base) == mmap::GPIO_BASE and pin >= (first) and pin <= (last))
    constexpr bool c6_pin_kernel_owned(uint32_t pin)
    {
        return pin == KICKOS_BOARD_LED_BIT or pin == KICKOS_BOARD_CONSOLE_TX_BIT or pin == KICKOS_BOARD_CONSOLE_RX_BIT
            KICKOS_BOARD_RESERVED_RUNS(KICKOS_RESERVED_RUN);
    }
#undef KICKOS_RESERVED_RUN

#define KICKOS_KERNEL_PIN(port_base, bit) or ((port_base) == mmap::GPIO_BASE and pin == (bit))
    constexpr bool c6_pin_listed(uint32_t, uint32_t pin)
    {
        return false KICKOS_BOARD_KERNEL_PINS(KICKOS_KERNEL_PIN);
    }
#undef KICKOS_KERNEL_PIN
    static_assert(kickos::refuses_exactly([](uint32_t, uint32_t pin) { return c6_pin_kernel_owned(pin); },
                                          c6_pin_listed, 1u, 31u),
                  "arch_pinmux_set refuses other pins than the board's kernel pins");
}

extern "C"
{

// --- Console: UART0, bridged to the host by the on-board CH343P. The native
//     USB-Serial-JTAG at 0x6000_F000 does not reliably deliver output once the app takes
//     over: it is gated on the host draining CDC and it re-enumerates on reset. UART0 has
//     neither behaviour.
int arch_console_write(char const* buf, size_t n)
{
    return console_tx_insert_line(buf, n, KICKOS_CONSOLE_CRLF);
}

#if KICKOS_AMP_OWN_IMAGE
int arch_console_write_retry(char const* buf, size_t n, bool* cr_pending)
{
    // The LP node relays its report through the shared ring; only HP owns
    // UART0, so this writer either queues the whole line or queues none.
    if (cr_pending != nullptr)
    {
        *cr_pending = false;
    }
    return arch_console_write(buf, n);
}
#endif

// Synchronous polled writer for the panic / fault / pre-arm path (console.cc picks it when
// the ring is unarmed or in ISR/panic context); it replaces a fallback TU that would
// re-enter the buffered writer. Bounded so a wedged UART cannot hang the panic path.
void arch_console_write_sync(char const* buf, size_t n)
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
                return; // FIFO not draining -> drop (never block the kernel)
            }
        }
        r32(reg::uart::FIFO) = static_cast<uint8_t>(buf[i]);
    }
}

// Block until UART0 is transmission-complete. STATUS.TXFIFO_CNT reaching 0 is only
// buffer-empty and leaves up to one frame still clocking out of the shifter;
// FSM_STATUS.ST_UTX_OUT is the transmitter state machine itself (C6 TRM section 27.7.1,
// Register 27.24).
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
            return; // bounded, as arch.h requires: a wedged UART drops the tail, never hangs
        }
    }
}

// --- Tickless clock: the 64-bit CLINT MTIME -> ns -------------------------------
uint64_t arch_clock_now(void)
{
    volatile uint32_t* mt = r32p(reg::clint::MTIME);
    uint32_t hi, lo, hi2;
    do
    {
        hi = mt[1];
        lo = mt[0];
        hi2 = mt[1];
    } while (hi != hi2);
    uint64_t t = (static_cast<uint64_t>(hi) << 32) | lo;
    return mtime_ticks_to_ns(t, g_mtime_shift);
}

// --- One-shot next-event timer: CLINT MTIMECMP (fires when MTIME >= MTIMECMP) ----
void arch_timer_arm(uint64_t deadline_ns)
{
    uint64_t ticks = mtime_ns_to_ticks(deadline_ns, g_mtime_shift);
    volatile uint32_t* cmp = r32p(reg::clint::MTIMECMP);
    cmp[1] = 0xFFFFFFFFu; // park high half so no spurious match between the two stores
    cmp[0] = static_cast<uint32_t>(ticks);
    cmp[1] = static_cast<uint32_t>(ticks >> 32);
}

void arch_timer_disarm(void)
{
    volatile uint32_t* cmp = r32p(reg::clint::MTIMECMP);
    cmp[0] = 0xFFFFFFFFu;
    cmp[1] = 0xFFFFFFFFu;
}

// The ESP32-C6 HP core traps (illegal instruction) on `csrw mcounteren`, so the
// generic rv32 bring-up must not write it.
int arch_rv_has_mcounteren(void) { return 0; }

// Inject-delivery backend (the arch fallback TU raises SSIP, which is a
// no-op on this M/U-only core). Assert the FROM_CPU_0 level source -> CPU int 31
// fires (mcause=31 -> switch.S .Lext). The logical line is already in g_inject_line.
void arch_rv_inject_deliver(int line)
{
    (void)line;
    r32(reg::intpri::FROM_CPU_0) = 1;
}

// EOI, run at the head of the .Lext trap: de-assert the level source so it does not
// re-fire on mret, then fence so the de-assert settles (INTPRI is APB, multi-cycle).
void arch_rv_ext_eoi(void)
{
    r32(reg::intpri::FROM_CPU_0) = 0;
    __asm volatile("fence" ::: "memory");
}

// --- Buffered console TX backend (console_tx.h). The ring drains via UART0's TXFIFO_EMPTY
// interrupt, routed through the interrupt matrix to a real CPU int (KICKOS_RV_DEV_CPU_INT,
// distinct from the software-inject doorbell). The source is a latch, dropped by the
// INT_CLR write in c6_tx_push.
static int c6_tx_slot_free(void)
{
    if (((r32(reg::uart::STATUS) >> reg::uart::TXFIFO_CNT_S) & reg::uart::TXFIFO_CNT_MASK) <
        reg::uart::TXFIFO_LIMIT)
    {
        return 1;
    }
    return 0;
}
static void c6_tx_push(uint8_t b)
{
    r32(reg::uart::FIFO) = b;
    // Drop the TX-empty latch after every push. UART_INT_RAW is self-set and cleared only
    // by INT_CLR, so once the FIFO has passed the threshold the latch, and with it the
    // matrix source, stays asserted on a condition that is no longer true. The kernel
    // drain runs with the line UNMASKED (irq_attach, not a tier-1 binding), so an
    // undropped latch re-enters the dispatcher forever whenever the FIFO fills before the
    // ring empties.
    r32(reg::uart::INT_CLR) = reg::uart::TXFIFO_EMPTY_INT;
}
static void c6_tx_irq_enable(void)
{
    r32(reg::uart::INT_CLR) = reg::uart::TXFIFO_EMPTY_INT;                              // clear any stale latch
    r32(reg::uart::INT_ENA) = r32(reg::uart::INT_ENA) | reg::uart::TXFIFO_EMPTY_INT;   // enable TX-empty
}
static void c6_tx_irq_disable(void)
{
    r32(reg::uart::INT_ENA) = r32(reg::uart::INT_ENA) & ~reg::uart::TXFIFO_EMPTY_INT;
}

static char console_tx_buf[KICKOS_CONSOLE_TX_SIZE];
console_tx_backend const c6_console_backend = {
    c6_tx_slot_free, c6_tx_push, c6_tx_irq_enable, c6_tx_irq_disable};

console_tx_backend const* arch_console_tx_backend(char** storage, uint32_t* size, int* irq_line)
{
#if !KICKOS_HAVE_MPU && !KICKOS_AMP_OWN_IMAGE
    // After the banner and before the ring, so the witness's line is the boot's own.
    if (kickos_c6_txidle_probe != nullptr)
    {
        kickos_c6_txidle_probe();
    }
#endif
    *storage = console_tx_buf;
    *size = KICKOS_CONSOLE_TX_SIZE;
    *irq_line = irq::UART0_TX_LINE;
    return &c6_console_backend;
}

// UART0 @ 0x6000_0000 (TRM v1.2, memory map Table 5.3-2), the whole block c6uart is
// granted. On that driver the IRQ thread holds the grant, not the service thread whose
// death notes the console dead.
void arch_console_reclaim_window(uintptr_t* base, size_t* size)
{
    *base = CONSOLE_WIN_BASE;
    *size = CONSOLE_WIN_SIZE;
}

// Panic-path reclaim (console.cc D6): force UART0 back to a known polled-ready 8N1 channel
// after a userspace driver may have garbled every writable register in its granted window.
// Runs with IRQs masked, privileged; MUST be idempotent + re-entrant, so it is straight-line
// ABSOLUTE stores only, NO read-modify-write: an RMW on a garbled value is not safe to
// repeat from a nested-fault re-entry.
//
// Reclaim depth = the in-window registers a driver can set to cause SILENT LOSS, each named
// below with the failure it prevents. There are no init values to restore beyond the baud
// divisor: KickOS never programs UART0, it inherits the ROM's setup. The clock source and
// its divider (PCR) and the pad mux (IO MUX) are privileged blocks outside the window, out
// of the driver's reach.
//
// The TX FIFO is deliberately NOT reset, unlike the esp32 body: it is 128 bytes deep, and on
// a panic from KERNEL_OWNED it holds bytes the TX ISR pushed and already removed from the
// ring, which kpanic_enter's flush therefore cannot re-send. Dropping them would cut a hole
// in the middle of the log; keeping them costs ~11 ms of stale bytes ahead of the dump.
void arch_console_reclaim(void)
{
    // Silence first: a stale enabled source would storm the dispatcher through the whole
    // dump, and INT_ENA=0 makes every INT_ST bit read 0 whatever is latched.
    r32(reg::uart::INT_ENA) = 0;
    r32(reg::uart::INT_CLR) = 0xFFFFFFFFu;

    // TX_SCLK_EN clear stops the transmitter dead and TX_RST_CORE set holds it in reset.
    r32(reg::uart::CLK_CONF) = reg::uart::CLK_CONF_RUN;

    // The one reclaim body here that is not straight-line. The _SYNC stores below reach the
    // UART core only on the REG_UPDATE write that ends the body, and waiting for the PREVIOUS
    // synchronisation first is the TRM's own step (section 27.5.2.2): skip it and these
    // stores are dropped rather than applied. The wait is bounded and read-only, so a
    // nested-fault re-entry repeats it harmlessly.
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

// Route + enable a real device line: aim its interrupt-matrix source at the line's CPU
// interrupt, configure that CPU int (level, priority) and enable it at the controller and
// in mie. A line with no route stays on the software doorbell (no-op). The UART's own
// TXFIFO_EMPTY enable is toggled per-burst by whoever owns the block
// (c6_tx_irq_enable/disable for the console).
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

// The kernel-owned half of the pair: clear exactly what arch_rv_hw_unmask set, so a mask
// really disarms the line instead of only setting the arch's software bit.
//
// COARSE: the enable gates a whole CPU interrupt, so every interrupt-matrix source routed
// to that CPU int is masked together. Per-MATRIX-SOURCE masking is kernel-owned (write 0 to
// the source's INTMTX map register, TRM section 10.3.3.3) but UART0 is the only source on
// this CPU int. Per-SUB-SOURCE masking would need UART_INT_ENA, which lives inside the
// register block granted to the driver and which the kernel must therefore not write; hence
// one grouped logical line for the C6 UART.
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

// Real-device dispatch (switch.S .Lextdev). Reads UART_INT_ST once and posts every logical
// line an asserted sub-source belongs to, at most once per pass.
//
// It does NOT write the UART block: clearing INT_CLR here would consume a latch that the
// owner of the block has to see (an overrun/framing/parity count would vanish), and on a
// tier-1 binding the storm is already prevented by irq_event_isr's mask. An asserted source
// outside the table still gets a post, so a bound handler masks and counts it instead of
// re-entering forever.
void kickos_rv_ext_dispatch_dev(void)
{
    uint32_t const st = r32(reg::uart::INT_ST); // INT_RAW & INT_ENA (TRM Register 27.4)
    if (st == 0)
    {
        return;
    }
    uint32_t posted = 0; // logical lines already posted in this pass
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

static_assert(KICKOS_BOARD_LED_ADDRESSABLE == 1, "this backend drives a WS2812, an addressable LED");

// --- Kernel diagnostic LED: the board's WS2812B, driven by RMT channel 0.
void arch_diag_led_init(void)
{
    // Ungate + reset the RMT, then select its source clock. PCR owns both on the C6.
    r32(reg::pcr::RMT_CONF) |= reg::pcr::RMT_CLK_EN;           // APB register clock
    r32(reg::pcr::RMT_CONF) |= reg::pcr::RMT_RST_EN;           // assert peripheral reset
    r32(reg::pcr::RMT_CONF) &= ~reg::pcr::RMT_RST_EN;          // deassert
    // XTAL 40 MHz source, group divisor 1 (DIV_NUM field 0), function clock enabled.
    r32(reg::pcr::RMT_SCLK_CONF) =
        (3u << reg::pcr::RMT_SCLK_SEL_S) | (0u << reg::pcr::RMT_SCLK_DIV_NUM_S) | reg::pcr::RMT_SCLK_EN;

    r32(reg::rmt::SYS_CONF) |= reg::rmt::APB_FIFO_MASK;        // access channel RAM directly
    // Channel 0: div_cnt=2 (-> 20 MHz tick), 1 RAM block, idle drives low (WS2812 reset),
    // carrier off. Latch it.
    r32(reg::rmt::CH0CONF0) = RMT_CH0_CFG;                     // carrier_en defaults 1 -> cleared here
    r32(reg::rmt::CH0CONF0) = RMT_CH0_CFG | reg::rmt::CONF_UPDATE;

    // Route RMT ch-0 TX to the LED's pin: GPIO matrix out-sel = signal 71, output enable,
    // IO_MUX pad on the GPIO function with a driver.
    r32(reg::gpio::func_out_sel_cfg(KICKOS_BOARD_LED_BIT)) = reg::gpio::RMT_SIG_OUT0_IDX;
    r32(reg::gpio::ENABLE_W1TS) = 1u << KICKOS_BOARD_LED_BIT;
    r32(reg::io_mux::gpio(KICKOS_BOARD_LED_BIT)) = reg::io_mux::MCU_SEL_GPIO | reg::io_mux::FUN_DRV_2;

    rmt_send_ws2812(0); // start dark
}

// The board's LED2 is RGB-ordered (first byte = red), NOT the usual GRB: confirmed on
// silicon (0x00FF00 lit green), so red is the MSB byte 0xFF0000.
void arch_diag_led_set(int on)
{
    uint32_t rgb = 0;
    if (on)
    {
        rgb = 0xFF0000u;
    }
    rmt_send_ws2812(rgb);
}

// One-shot pin-function config (KOS_SYS_PINMUX_SET), covering BOTH permission stages a
// pad passes on this family: the IO_MUX pad function and the GPIO matrix out-sel that
// picks which internal signal drives it. Leaving the matrix stage out would make the
// kernel-owned refusal below bypassable: a caller could aim a peripheral signal at
// the console pins or the LED's without ever touching their IO_MUX word. func packs both stages;
// the encoding is chip-local (reg::gpio::PINMUX_*).
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
    if (c6_pin_kernel_owned(pin))
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

// Branch-clock oracle (arch.h): report the function clock feeding a peripheral block so a
// userspace driver derives its own divisor. On this chip the select AND the divider live in
// PCR, which is a Rule 7 reserved block, so the holder of a UART window cannot read either
// and this is the only way it learns its own rate. Every field is read LIVE: a select the
// ROM or a later bootloader changed is reflected, and nothing here is compiled in.
//
// TWO LEAVES CANNOT BE READ BACK, and both answer 0 rather than a guess. RC_FAST_CLK is
// "17.5 MHz by default ... with adjustable frequency" (TRM section 8.2.3) and the only
// register naming it, PCR_FOSC_FREQ, is HRO and reads 20; a wrong branch clock silently
// garbles the wire, so a UART sourced from it has no reportable rate. Select 0 is no clock
// at all. The crystal IS readable (PCR_CLK_XTAL_FREQ, RO, MHz).
uint32_t arch_periph_clock_hz(uintptr_t base)
{
    if (base != mmap::UART0_BASE)
    {
        return 0;
    }
    uint32_t const conf = r32(reg::pcr::UART0_SCLK_CONF);
    if ((conf & reg::pcr::UART0_SCLK_EN) == 0u)
    {
        return 0; // the function clock is gated: the block is not counting anything
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
    // The TRM contradicts itself on which of DIV_A and DIV_B is the numerator: section 8.5.1
    // calls DIV_A the denominator, while the two chapters that actually write the equation
    // for an identical PCR divider (I2C section 29.4.1, RMT section 37.3.3) put DIV_A over
    // DIV_B. With both fields zero the two readings agree and the divisor is DIV_NUM + 1; a
    // non-zero fraction is refused rather than resolved from the wrong one.
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

static void c6_refuse(char const* msg)
{
    size_t n = 0;
    while (msg[n] != '\0')
    {
        n++;
    }
    arch_console_write_sync(msg, n);
    arch_shutdown(1);
}

// Every node's rows at once: node 1's stay inert until arch_amp_release_peers moves the LP
// CPU to their mode.
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

// CPU_CLK as the boot path left it, which MTIME counts. A source with no exact rate would leave
// every sleep and timeout wrong, so it stops the boot.
static void mtime_rate_init(void)
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
        c6_refuse("KickOS: ESP32-C6 CPU clock is not 160 MHz >> n, so MTIME has no exact rate\n");
    }
    g_mtime_shift = static_cast<uint32_t>(shift);
    SystemCoreClock = hz;
}

void arch_init(void)
{
    wdt_disable_all(); // or the ROM-armed watchdogs reset the part in seconds
    c6_early_mark('E'); // watchdogs disabled
    mtime_rate_init();

    // Before anything unprivileged exists: arch_console_reclaim has no other way back to a
    // working baud (see g_console_clkdiv).
    g_console_clkdiv = r32(reg::uart::CLKDIV);

    g_clint_msip = r32p(reg::clint::MSIP);   // the deferred-switch software interrupt
#if KICKOS_BENCH
    // The C6 traps on `rdcycle`; MTIME counts CPU cycles. Set before any switch.
    extern volatile uint32_t* g_bench_cycle_src;
    g_bench_cycle_src = r32p(reg::clint::MTIME);
#endif
    arch_timer_disarm();               // MTIMECMP = max: no timer fire until armed
    r32(reg::clint::MTIMECTL) = reg::clint::MTIMECTL_MTCE | reg::clint::MTIMECTL_MTIE; // start the counter + enable

    kickos_rv32_init();  // vectored mtvec + mie(MSIE|MTIE|SSIE) + PMP (no mcounteren here)
    apm_program_gate();
    c6_early_mark('F');  // mtvec + mie + permissive bootstrap PMP installed
    inject_doorbell_init(); // wire the interrupt matrix FROM_CPU doorbell (device IRQs)
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

void arch_shutdown(int status)
{
    (void)status; // no exit on bare metal
    __asm volatile("csrci mstatus, 0x8" ::: "memory"); // mask interrupts (clear MIE)
    while (true)
    {
        __asm volatile("wfi");
    }
}

// --- C-runtime bring-up (the reset entry) ----------------------------------
void Reset_Handler(void)
{
    c6_early_mark('A'); // reset entry reached (gp/sp/tp already set by _start)
    // The ROM loader copies the image segments to SRAM at their VMAs, so LMA == VMA and
    // this loop is a no-op.
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
    if (__register_frame != nullptr) // weak: null in a freestanding image (see decl)
    {
        __register_frame(&__eh_frame_start); // DWARF EH: register before ctors/throws
    }
    for (void (**fn)() = __init_array_start; fn != __init_array_end; fn++)
    {
        (*fn)();
    }
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
    // The ESP32-C6 ROM placed both node LOAD segments from the one flashed
    // partition image. Node 0 copies only the small reset vector into LP SRAM.
    if (r32(image_base) == 0u or stub_size == 0u or stub_size > 1024u
        or (stub_size & 3u) != 0u)
    {
        constexpr char msg[] = "KickOS: ESP32-C6 AMP LP segment absent or stub invalid\n";
        arch_console_write_sync(msg, sizeof(msg) - 1u);
        arch_shutdown(1);
    }
    // Measure the LP RTC clock against HP's clock before wake. Its silicon rate
    // depends on the ROM's clock selection and oscillator trim.
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
        constexpr char msg[] = "KickOS: ESP32-C6 LP RTC rate invalid\n";
        arch_console_write_sync(msg, sizeof(msg) - 1u);
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
