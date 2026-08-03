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

#include <kickos/arch/arch.h>
#include <kickos/arch/rv_trap_ids.h>
#include <kickos/console_tx.h>
#include <kickos/sys/abi.h> // KOS_E* taxonomy (arch_pinmux_set)

#include <stdint.h>

#include "mmap.h"
#include "irq.h"
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
    extern void (*__init_array_start[])();
    extern void (*__init_array_end[])();

    // DWARF EH frame table (esp32c6.ld) + the libgcc registrar. The -nostartfiles link
    // drops crtbegin, so its frame_dummy never registers .eh_frame and a full-C++ app
    // must register it by hand at boot. WEAK ref: a freestanding image references no
    // _Unwind_*, the registrar object is never pulled, and the call is skipped.
    extern uint32_t __eh_frame_start;
    void __register_frame(void*) __attribute__((weak));
#if KICKOS_HAVE_MPU
    // App-data NAPOT region (esp32c6.ld). .appdata holds the app + C++-runtime .data and
    // the gp small-data window; its VMA jumps to the pow2 window base above the NOLOAD
    // kernel .bss, so LMA != VMA and it needs a copy (like .data) before .appbss + pad
    // are zeroed.
    extern uint32_t _appdata_lma, __kickos_appdata_start, __kickos_appbss_start,
        __kickos_appdata_end;
#endif

    // Core clock in Hz. MEASURED ~160 MHz on silicon: the ROM first-stage loader brings
    // up the PLL and leaves the CPU on it, so this is NOT XTAL/1 = 40 MHz. Derived from
    // the MTIME-rate measurement below (MTIME is core-clocked); the CPU clock is not
    // independently measurable here (the C6 traps on rdcycle). KickOS does not configure
    // the clock, it inherits the ROM's PLL setup.
    uint32_t SystemCoreClock = 160000000u;
}

namespace
{
    inline volatile uint32_t* r32p(uintptr_t a) { return reinterpret_cast<volatile uint32_t*>(a); }
    inline volatile uint32_t& r32(uintptr_t a) { return *r32p(a); }

    // MTIME is core-clocked ~160 MHz (reg::clint::MTIME_HZ): 1e9/160e6 = 6.25 ns/tick =
    // 25/4 exactly. An integer ns-per-tick (=6) would truncate and run every
    // sleep/timestamp 4.17% long, so convert with the exact 25/4 ratio in 64-bit
    // (overflows only past ~1460 yr at 160 MHz).
    inline uint64_t mtime_ticks_to_ns(uint64_t ticks)
    {
        return ticks * 25ull / 4ull;
    }
    inline uint64_t mtime_ns_to_ticks(uint64_t ns)
    {
        return ns * 4ull / 25ull;
    }

    // --- UART0 console (regs/uart.h; TRM ch.27; base 0x6000_0000), on GPIO16/17 behind
    //     the board's CH343P (U4). The ROM already sets UART0 up (baud/pins) for its own
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
    constexpr uint32_t CONSOLE_TX_SIZE = 512;             // ring; power of two; > kprintf's 256B buf

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
    constexpr uint32_t DOORBELL_PRIO = 7; // 1..15; sole external source, so uncontended

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

    // One-time inject-doorbell wiring. Called from arch_init with MIE still 0, per the
    // TRM's "configure the interrupt controller with interrupts globally disabled" rule;
    // boot runs MIE=0 until arch_start mret's the first thread. The C6's mie bit N gates
    // CPU int N, with bits 3/7 = the standard msip/mtip. FROM_CPU_0 is left de-asserted.
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

    // --- Diagnostic LED: onboard WS2812B (board LED2, DI on GPIO8, VDD tied to 3V3,
    //     no enable pin). GPIO bit-bang FAILS here: the register-write latency exceeds
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
}

extern "C"
{

// --- Console: UART0, bridged to the host by the on-board CH343P. The native
//     USB-Serial-JTAG at 0x6000_F000 does not reliably deliver output once the app takes
//     over: it is gated on the host draining CDC and it re-enumerates on reset. UART0 has
//     neither behaviour.
void arch_console_write(char const* buf, size_t n)
{
    console_tx_write(buf, n); // buffered ring; the routing guard (console.cc) keeps this thread-only
}

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
            if (++spin > 200000u)
            {
                return; // FIFO not draining -> drop (never block the kernel)
            }
        }
        r32(reg::uart::FIFO) = static_cast<uint8_t>(buf[i]);
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
    return mtime_ticks_to_ns(t);
}

// --- One-shot next-event timer: CLINT MTIMECMP (fires when MTIME >= MTIMECMP) ----
void arch_timer_arm(uint64_t deadline_ns)
{
    uint64_t ticks = mtime_ns_to_ticks(deadline_ns);
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
int c6_tx_slot_free(void)
{
    if (((r32(reg::uart::STATUS) >> reg::uart::TXFIFO_CNT_S) & reg::uart::TXFIFO_CNT_MASK) <
        reg::uart::TXFIFO_LIMIT)
    {
        return 1;
    }
    return 0;
}
void c6_tx_push(uint8_t b)
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
void c6_tx_irq_enable(void)
{
    r32(reg::uart::INT_CLR) = reg::uart::TXFIFO_EMPTY_INT;                              // clear any stale latch
    r32(reg::uart::INT_ENA) = r32(reg::uart::INT_ENA) | reg::uart::TXFIFO_EMPTY_INT;   // enable TX-empty
}
void c6_tx_irq_disable(void)
{
    r32(reg::uart::INT_ENA) = r32(reg::uart::INT_ENA) & ~reg::uart::TXFIFO_EMPTY_INT;
}

char console_tx_buf[CONSOLE_TX_SIZE];
console_tx_backend const c6_console_backend = {
    c6_tx_slot_free, c6_tx_push, c6_tx_irq_enable, c6_tx_irq_disable};

console_tx_backend const* arch_console_tx_backend(char** storage, uint32_t* size, int* irq_line)
{
    *storage = console_tx_buf;
    *size = CONSOLE_TX_SIZE;
    *irq_line = irq::UART0_TX_LINE;
    return &c6_console_backend;
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

// --- Kernel diagnostic LED: onboard WS2812B on GPIO8, driven by RMT channel 0.
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

    // Route RMT ch-0 TX -> GPIO8: GPIO matrix out-sel = signal 71, output enable,
    // IO_MUX pad on the GPIO function with a driver.
    r32(reg::gpio::func_out_sel_cfg(8)) = reg::gpio::RMT_SIG_OUT0_IDX;
    r32(reg::gpio::ENABLE_W1TS) = 1u << 8;
    r32(reg::io_mux::GPIO8) = reg::io_mux::MCU_SEL_GPIO | reg::io_mux::FUN_DRV_2;

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

// Kernel-owned pins arch_pinmux_set refuses so a board map cannot steal the console
// or the diag LED. GPIO16/17 = UART0 TX/RX (CH343P bridge); GPIO8 = WS2812 diag LED.
static bool c6_pin_kernel_owned(uint32_t pin)
{
    return pin == 8u or pin == 16u or pin == 17u;
}

// One-shot pin-function config (KOS_SYS_PINMUX_SET), covering BOTH permission stages a
// pad passes on this family: the IO_MUX pad function and the GPIO matrix out-sel that
// picks which internal signal drives it. Leaving the matrix stage out would make the
// kernel-owned refusal below bypassable: a caller could aim a peripheral signal at
// GPIO16/17 or GPIO8 without ever touching their IO_MUX word. func packs both stages;
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

// HP_APM background permit for security mode REE0, which is what U-mode is by reset
// (HP_TEE_M0_MODE_CTRL = 0), so no HP_TEE write is needed. The reset posture DENIES
// every REE access to every HP peripheral (region 0 catch-all: START=0, END=0xFFFFFFFF,
// ATTR=0), which would make even a granted MMIO window unreachable from an
// unprivileged thread. Region 0 stays, and regions 1..3 re-permit its complement
// outside the HP-bus blocks of the Rule 7 set (INTMTX, then the contiguous
// PCR..HP_APM span); an overlap resolves to the permit (TRM 16.3.2.3). PMP is the
// per-thread authority; APM cannot be, it is per security mode and its denial does
// not trap (regs/apm.h).
static void apm_open_ree0(void)
{
    constexpr uint32_t BLOCK = 0x1000u;
    struct permit
    {
        uint32_t start;
        uint32_t end;
    };
    static permit const permits[] = {
        {0x00000000u, mmap::INTMTX_BASE - 1u},
        {mmap::INTMTX_BASE + BLOCK, mmap::PCR_BASE - 1u},
        {mmap::HP_APM_BASE + BLOCK, 0xFFFFFFFFu},
    };
    uint32_t en = r32(reg::apm::FILTER_EN);
    for (uint32_t i = 0; i < sizeof(permits) / sizeof(permits[0]); i++)
    {
        uint32_t const n = i + 1u;
        r32(reg::apm::region_addr_start(n)) = permits[i].start;
        r32(reg::apm::region_addr_end(n)) = permits[i].end;
        r32(reg::apm::region_attr(n)) = reg::apm::R0_R | reg::apm::R0_W;
        en |= reg::apm::region_en(n);
    }
    r32(reg::apm::FILTER_EN) = en;
    __asm volatile("fence" ::: "memory");
}

void arch_init(void)
{
    wdt_disable_all(); // or the ROM-armed watchdogs reset the part in seconds
    c6_early_mark('E'); // watchdogs disabled

    g_clint_msip = r32p(reg::clint::MSIP);   // the deferred-switch software interrupt
#if KICKOS_BENCH
    // The C6 traps on `rdcycle`; give the bench the core-clocked CLINT MTIME low word
    // (== CPU cycles at this PLL) as its free-running counter. Set before any switch.
    extern volatile uint32_t* g_bench_cycle_src;
    g_bench_cycle_src = r32p(reg::clint::MTIME);
#endif
    arch_timer_disarm();               // MTIMECMP = max: no timer fire until armed
    r32(reg::clint::MTIMECTL) = reg::clint::MTIMECTL_MTCE | reg::clint::MTIMECTL_MTIE; // start the counter + enable

    kickos_rv32_init();  // vectored mtvec + mie(MSIE|MTIE|SSIE) + mcounteren + PMP
    apm_open_ree0();     // bus-side gate: REE0 permit outside the Rule 7 HP blocks
    c6_early_mark('F');  // mtvec + mie + permissive bootstrap PMP installed
    inject_doorbell_init(); // wire the interrupt matrix FROM_CPU doorbell (device IRQs)
    c6_early_mark('G');  // inject doorbell wired
}

#if KICKOS_HAVE_MPU
// Rule 7 reserved set (ESP32-C6 TRM). One 4 KB page at 0x20001000 covers both the
// undocumented CPU-interrupt-controller window (@0x20001000, regs/plic.h) and the
// core-local CLINT (@0x20001800: MSIP switch doorbell + MTIME/MTIMECMP tickless timer
// @0x20001808/0x20001810, TRM section 1.7.5), so one entry covers timebase + IRQ enable.
// INTMTX (interrupt matrix routing) and PCR (the clock/reset gate block the MTIME rate
// depends on) are owns-for-life too, as are the two bus-side permission controllers:
// HP_TEE (per-master security mode) and HP_APM (the REE permission regions arch_init
// programs once). Granting HP_APM is total escalation, and its region registers sit on
// a 0xC stride, so even a minimal window reaches several regions.
size_t arch_reserved_blocks(struct arch_reserved_block* out, size_t max)
{
    static struct arch_reserved_block const blocks[] = {
        {0x20001000u, 0x1000u}, // int-controller window (@+0x000) + CLINT MSIP/MTIME/MTIMECMP (@+0x800..)
        {0x60010000u, 0x1000u}, // INTMTX: interrupt matrix (TRM, memory map Table 5.3-2)
        {0x60096000u, 0x1000u}, // PCR: clock + reset gate controller (TRM PCR ch.; regs @+0x2c/+0x30)
        {0x60098000u, 0x1000u}, // HP_TEE: per-master security mode (TRM ch.16)
        {0x60099000u, 0x1000u}, // HP_APM: bus-side access-permission regions (TRM ch.16)
    };
    size_t n = sizeof(blocks) / sizeof(blocks[0]);
    if (n > max)
    {
        n = max;
    }
    for (size_t i = 0; i < n; i++)
    {
        out[i] = blocks[i];
    }
    return n;
}
#endif

void arch_shutdown(int status)
{
    (void)status; // no exit on bare metal
    __asm volatile("csrci mstatus, 0x8" ::: "memory"); // mask interrupts (clear MIE)
    while (true)
    {
        __asm volatile("wfi");
    }
}

// --- C-runtime bring-up (the reset entry, called by _start in startup.S) ------
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
    c6_early_mark('B'); // .data copied + .bss zeroed
#if KICKOS_HAVE_MPU
    uint32_t* asrc = &_appdata_lma;
    uint32_t* adst = &__kickos_appdata_start;
    while (adst < &__kickos_appbss_start) // .appdata: LMA != VMA (see decl)
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
