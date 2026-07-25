// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// ESP32-C6-WROOM-1 (ESP-RISC-V "HP CPU", RV32IMAC) chip backend. Shares the
// rv32imac arch with the qemu-virt board; this layer supplies the hardware edges:
// reset/C-runtime bring-up, watchdog disable, the console, arch_shutdown, and the
// tickless clock/one-shot timer.
//
// The C6 has a memory-mapped core-local CLINT (TRM ch.1.7) that provides the exact
// same seam as the qemu-virt SiFive CLINT: MSIP (machine software interrupt =
// deferred switch, mcause 3) + MTIME/MTIMECMP (machine timer = tickless tick,
// mcause 7). So the scheduler mechanism is identical to virt; only the base
// address + the MTCE counter-enable differ. Console is UART0, bridged to the host
// by the board's CH343P (Waveshare C6-DEV-KIT) -- see arch_console_write for why
// NOT the native USB Serial/JTAG. Device IRQs vector through the PLIC (0x2000_1000),
// not the vestigial INTPRI/INTC block -- see inject_doorbell_init.
//
// Register addresses: ESP32-C6 TRM v1.2 (memory map Table 5.3-2; CLINT ch.1.7;
// watchdogs ch.14/15; UART ch.26; PLIC/INTMTX). Hand-rolled, no ESP-IDF/HAL sources.
// Validated on silicon (selftest incl. the inject-driven IRQ path, fault dumps,
// bounded PMP NAPOT enforcement).

#include <kickos/arch/arch.h>
#include <kickos/arch/rv_trap_ids.h>
#include <kickos/console_tx.h>
#include <kickos/sys/abi.h> // KOS_E* taxonomy (arch_pinmux_set)

#include <stdint.h>

// Hand-rolled register map for this chip (clean-room, no ESP-IDF/HAL sources).
// Bases in mmap.h, CPU-int/kernel IRQ lines in irq.h, per-peripheral offsets/fields
// in regs/. (regs/usb_serial_jtag.h + regs/apm.h exist but are not consumed here:
// the USB console is unused and APM is programmed by the enforcement backend.)
#include "mmap.h"
#include "irq.h"
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

    // DWARF EH frame table (esp32c6.ld) + the libgcc registrar. RISC-V exceptions use
    // DWARF .eh_frame, normally registered by crtbegin's frame_dummy -- but the
    // -nostartfiles link drops crtbegin, so a full-C++ app registers the table by hand
    // at boot. WEAK ref: a freestanding image references no _Unwind_*, so the libgcc
    // registrar object is never pulled, the ref stays null, and the call is skipped.
    extern uint32_t __eh_frame_start;
    void __register_frame(void*) __attribute__((weak));
#if KICKOS_HAVE_MPU
    // App-data NAPOT region (esp32c6.ld). .appdata holds the app + C++-runtime .data and
    // the gp small-data window; its VMA jumps to the pow2 window base above the NOLOAD
    // kernel .bss, so LMA != VMA and it needs a copy (like .data) before .appbss + pad
    // are zeroed. All through the RW grant, so an unprivileged thread never reads stale
    // bytes from its data region.
    extern uint32_t _appdata_lma, __kickos_appdata_start, __kickos_appbss_start,
        __kickos_appdata_end;
#endif

    // Core clock in Hz. MEASURED ~160 MHz on silicon (2026-07-09): the ROM first-stage
    // loader brings up the PLL and leaves the CPU on it -- NOT XTAL/1=40 MHz as first
    // assumed. Derived from the MTIME-rate measurement below (MTIME is core-clocked);
    // the CPU clock is not independently measurable here (the C6 traps on rdcycle).
    // KickOS does not itself configure the clock -- it inherits the ROM's PLL setup.
    // An explicit clock bring-up (+ a user low-power select) is a pending fleet item.
    uint32_t SystemCoreClock = 160000000u;
}

namespace
{
    inline volatile uint32_t* r32p(uintptr_t a) { return reinterpret_cast<volatile uint32_t*>(a); }
    inline volatile uint32_t& r32(uintptr_t a) { return *r32p(a); }

    // --- Core-local CLINT (regs/clint.h): MSIP switch doorbell + MTIME/MTIMECMP.
    // MTIME is core-clocked ~160 MHz (reg::clint::MTIME_HZ): 1e9/160e6 = 6.25 ns/tick =
    // 25/4 exactly. An integer ns-per-tick (=6) would truncate and run every
    // sleep/timestamp 4.17% long, so convert with the exact 25/4 ratio in 64-bit
    // (overflows only past ~1460 yr at 160 MHz). One home for the ratio; call sites stay named.
    inline uint64_t mtime_ticks_to_ns(uint64_t ticks)
    {
        return ticks * 25ull / 4ull;
    }
    inline uint64_t mtime_ns_to_ticks(uint64_t ns)
    {
        return ns * 4ull / 25ull;
    }

    // --- USB Serial/JTAG console (regs/usb_serial_jtag.h; TRM ch.32; base 0x6000_F000).
    //     NOT the KickOS console: the host CDC drain gates output and it re-enumerates on
    //     reset, so boot output is dropped. UART0 (below) is the real console.

    // --- UART0 console (regs/uart.h; TRM ch.26; base 0x6000_0000). The real console on
    //     this board: UART0 (GPIO16/17) is bridged to the host by the CH343P (U4) as a
    //     plain COM port -- unlike the native USB-Serial-JTAG it does NOT re-enumerate on
    //     reset and has no CDC host-connection gating, so boot output is never dropped.
    //     The ROM already sets UART0 up (baud/pins) for its own boot log, so we just push
    //     bytes: poll STATUS.TXFIFO_CNT for room, write the FIFO. FIFO depth 128.

    // --- Early boot markers (raw UART0, pre-console). DEBUG tool, default OFF: build
    //     with -DKICKOS_C6_EARLY_MARK=1 to emit a byte at each boot stage (A..H). The
    //     ROM leaves UART0 up (its boot log printed through it) and _start sets gp/sp
    //     before Reset_Handler, so a byte can be pushed to the TX FIFO before KickOS
    //     inits its own console -- localizes an early boot hang: the LAST marker seen
    //     names the last stage reached. Bounded spin: a wedged FIFO never blocks boot.
    //     Touches no global, so it is safe before .data/.bss are live.
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
    // UART0 TX-empty interrupt (buffered console ring). TXFIFO_EMPTY asserts while the TX FIFO
    // count <= CONF1.TXFIFO_EMPTY_THRHD -- a LEVEL source (TRM section 1.6): cleared from source, by
    // filling the FIFO past the threshold or disabling INT_ENA. We program the threshold (own
    // the register, don't trust ROM) and route it to a real CPU int (arch_rv_hw_unmask).
    constexpr uint32_t CONSOLE_TXFIFO_EMPTY_THRHD = 32;   // re-fire when the FIFO drains to <=32
    constexpr uint32_t CONSOLE_TX_SIZE = 512;             // ring; power of two; > kprintf's 256B buf

    // --- Watchdogs (regs/wdt.h; TRM ch.14 MWDT, ch.15 RWDT/SWD). ALL must be disabled
    //     or the ROM-armed WDTs reset the part within seconds. Common unlock key 0x50D83AA1.

    // --- Interrupt matrix (INTMTX) + local interrupt controller (INTPRI). The C6 has no
    //     S-mode, so the arch's SSIP inject channel is a no-op here; instead we raise a
    //     REAL machine interrupt. A software-settable FROM_CPU source (level) is routed
    //     through the matrix to a dedicated CPU interrupt ID, which the C6 core vectors as
    //     mcause = ID (Espressif's custom scheme, not the standard mcause=11). ONE
    //     doorbell carries every logical inject line (arch keeps g_inject_line).

    // The C6 uses the PLIC (M-mode window) as the CPU interrupt controller -- NOT the
    // INTPRI/INTC block, which is vestigial on this core (esp-idf: "ESP32C6 should use
    // the PLIC ... instead of INTC"). Enable, type, per-int priority, and threshold ALL
    // live in the PLIC. INTPRI keeps only the software-settable FROM_CPU source triggers.

    // Dedicated CPU interrupt ID for the inject doorbell. Must be one of the C6's
    // external IDs (1-2, 5-6, 8-31; local CLINT owns 0/3/4/7) and not collide with
    // the switch.S demux (3=msip, 7=mtip). Shared with switch.S's .Lext arm via
    // rv_trap_ids.h so the two cannot drift.
    constexpr uint32_t DOORBELL_CPU_INT = KICKOS_RV_INJECT_DOORBELL_CPU_INT;
    constexpr uint32_t DOORBELL_PRIO = 7; // 1..15; sole external source, so uncontended

    void timg_mwdt_disable(uintptr_t base)
    {
        r32(base + reg::wdt::TIMG_WDTWPROTECT) = reg::wdt::WKEY;       // unlock
        r32(base + reg::wdt::TIMG_WDTCONFIG0) &= ~(reg::wdt::TIMG_WDT_EN | reg::wdt::TIMG_WDT_FLASHBOOT);
        r32(base + reg::wdt::TIMG_WDTWPROTECT) = 0;                    // re-lock
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

    // One-time inject-doorbell wiring. Called from arch_init with MIE still 0 (the
    // TRM's "configure the interrupt controller with interrupts globally disabled"
    // rule -- boot runs MIE=0 until arch_start mret's the first thread). Route the
    // FROM_CPU_0 source to CPU int 31, level-triggered, priority DOORBELL_PRIO,
    // enabled; then enable the matching mie bit (the C6's mie bit N gates CPU int N,
    // with bits 3/7 = the standard msip/mtip). FROM_CPU_0 is left de-asserted.
    void inject_doorbell_init()
    {
        r32(reg::intmtx::FROM_CPU_0_MAP) = DOORBELL_CPU_INT;     // route the source -> CPU int
        r32(reg::plic::MXINT_PRI_BASE + 4u * DOORBELL_CPU_INT) = DOORBELL_PRIO;
        r32(reg::plic::MXINT_TYPE) &= ~(1u << DOORBELL_CPU_INT); // level
        r32(reg::plic::MXINT_THRESH) = 0;                       // fire for any prio > 0
        r32(reg::plic::MXINT_ENABLE) |= (1u << DOORBELL_CPU_INT); // enable at the PLIC
        __asm volatile("fence" ::: "memory");                    // settle before MIE is enabled
        __asm volatile("csrs mie, %0" ::"r"(1u << DOORBELL_CPU_INT) : "memory");
    }

    // --- Diagnostic LED: onboard WS2812B (board LED2, DI on GPIO8, VDD tied to 3V3,
    //     no enable pin). GPIO bit-bang FAILS here: the register-write latency exceeds
    //     the WS2812B ~400 ns bit high-time even at 160 MHz, so software cannot form
    //     valid bits (LED latched solid white). The RMT peripheral (regs/rmt.h) clocks
    //     the pulse train in hardware, so timing is exact. GRB order, 24 bits MSB
    //     first. Panic path: single frame, polled, no interrupts/DMA.

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

        // Blocking (panic ctx: interrupts masked, no DMA). Bounded so a wedged RMT
        // never hangs the fault path -- ~25 words * 1.25 us + 60 us latch is < 100 us.
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

// --- Console: UART0, bridged to the host by the on-board CH343P. Buffered: thread-context
//     output goes through the IRQ-drained ring (arch_console_write -> console_tx_write);
//     the panic/fault/pre-arm path uses the bounded polled arch_console_write_sync below.
//     (The native USB-Serial-JTAG at 0x6000_F000 does not reliably deliver output once the
//     app takes over -- host-CDC-drain gated + re-enumerates on reset; UART0 has neither.)
void arch_console_write(char const* buf, size_t n)
{
    console_tx_write(buf, n); // buffered ring; the routing guard (console.cc) keeps this thread-only
}

// Synchronous polled writer -- the panic / fault / pre-arm path (console.cc picks it when the
// ring is unarmed or in ISR/panic context). Overrides the weak default that would otherwise
// re-enter the buffered arch_console_write. Bounded so a wedged UART cannot hang the panic path.
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
// generic rv32 bring-up must not write it. Overrides the weak default in
// arch_rv32imac.cc. U-mode counter reads (rdcycle/rdtime) are not needed at M1.
int arch_rv_has_mcounteren(void) { return 0; }

// Inject-delivery override (weak default in arch_rv32imac.cc raises SSIP, which is a
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
// distinct from the software-inject doorbell). Level source: servicing clears it.
int c6_tx_slot_free(void)
{
    if (((r32(reg::uart::STATUS) >> reg::uart::TXFIFO_CNT_S) & reg::uart::TXFIFO_CNT_MASK) <
        reg::uart::TXFIFO_LIMIT)
    {
        return 1;
    }
    return 0;
}
void c6_tx_push(uint8_t b) { r32(reg::uart::FIFO) = b; }
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

// Route + enable the UART0 TX-ring line: aim the interrupt matrix's UART0 source at
// KICKOS_RV_DEV_CPU_INT, then configure that CPU int in the PLIC (level, priority) and
// enable it there and in mie -- the same proven path as the inject doorbell, one CPU int
// over. Other lines stay on the software doorbell (no-op). The UART's own TXFIFO_EMPTY
// enable is toggled per-burst by the ring (c6_tx_irq_enable/disable).
void arch_rv_hw_unmask(int line)
{
    if (line != irq::UART0_TX_LINE)
    {
        return;
    }
    // The console owns UART0: silence EVERY UART source + ack any ROM-left latch BEFORE arming
    // CPU int 30, else a stale ROM-enabled source (RX-full, break, ...) would storm the level-1
    // dispatcher the moment MIE is set. Program the TX-empty threshold too (own the register --
    // do not trust the ROM default). console_tx_write re-enables ONLY TXFIFO_EMPTY, later.
    r32(reg::uart::INT_ENA) = 0;
    r32(reg::uart::INT_CLR) = 0xFFFFFFFFu;
    uint32_t conf1 = r32(reg::uart::CONF1);
    conf1 &= ~(reg::uart::TXFIFO_EMPTY_THRHD_MASK << reg::uart::TXFIFO_EMPTY_THRHD_S);
    conf1 |= (CONSOLE_TXFIFO_EMPTY_THRHD & reg::uart::TXFIFO_EMPTY_THRHD_MASK) << reg::uart::TXFIFO_EMPTY_THRHD_S;
    r32(reg::uart::CONF1) = conf1;
    // Route the UART0 matrix source -> CPU int 30 (level, priority), enable at the PLIC + mie.
    r32(reg::intmtx::UART0_MAP) = KICKOS_RV_DEV_CPU_INT;
    r32(reg::plic::MXINT_PRI_BASE + 4u * KICKOS_RV_DEV_CPU_INT) = DOORBELL_PRIO;
    r32(reg::plic::MXINT_TYPE) &= ~(1u << KICKOS_RV_DEV_CPU_INT);   // level (cleared from source)
    r32(reg::plic::MXINT_ENABLE) |= (1u << KICKOS_RV_DEV_CPU_INT);  // enable at the PLIC
    __asm volatile("fence" ::: "memory"); // let the APB controller writes settle (TRM section 1.6.3.2)
    __asm volatile("csrs mie, %0" ::"r"(1u << KICKOS_RV_DEV_CPU_INT) : "memory");
}

// Real-device dispatch (switch.S .Lextdev): ack the UART's level latch, then run the ring
// drain via the console line's ISR. Servicing (filling the FIFO past THRHD, or the ring
// emptying -> c6_tx_irq_disable) de-asserts the source so it does not re-fire on mret.
void kickos_rv_ext_dispatch_dev(void)
{
    r32(reg::uart::INT_CLR) = reg::uart::TXFIFO_EMPTY_INT;
    __asm volatile("fence" ::: "memory");
    kickos_isr_irq(irq::UART0_TX_LINE);
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

// on -> red; off -> all channels 0. The board's LED2 is RGB-ordered (first byte =
// red), NOT the usual GRB -- confirmed on silicon (0x00FF00 lit green), so red is the
// MSB byte: 0xFF0000.
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

// One-shot pin-function config (KOS_SYS_PINMUX_SET). func is the raw IO_MUX_GPIOn_REG
// word (MCU_SEL | drive | IE), written verbatim via io_mux::gpio(pin). This is the
// IO_MUX layer only: routing a peripheral output through the GPIO matrix (the second
// signal-index write) is deferred.
int arch_pinmux_set(uint32_t port, uint32_t pin, uint32_t func)
{
    if (port != 0u or pin > 30u)
    {
        return -KOS_EINVAL;
    }
    if (c6_pin_kernel_owned(pin))
    {
        return -KOS_EBUSY;
    }
    r32(reg::io_mux::gpio(pin)) = func;
    return 0;
}

void arch_init(void)
{
    wdt_disable_all(); // or the ROM-armed watchdogs reset the part in seconds
    c6_early_mark('E'); // watchdogs disabled -- no more ROM WDT reset past here

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
    c6_early_mark('F');  // mtvec + mie + permissive bootstrap PMP installed
    inject_doorbell_init(); // wire the interrupt matrix FROM_CPU doorbell (device IRQs)
    c6_early_mark('G');  // inject doorbell wired -- arch_init complete
}

#if KICKOS_HAVE_MPU
// Rule 7 reserved set (ESP32-C6 TRM). The CPU sub-system page at 0x20001000 holds BOTH
// the PLIC (real IRQ controller, @0x20001000) and the core-local CLINT (@0x20001800:
// MSIP switch doorbell + MTIME/MTIMECMP tickless timer @0x20001808/0x20001810) -- both
// inside one 4 KB page, so a single entry covers the whole timebase + IRQ controller.
// INTMTX (interrupt matrix routing) and PCR (the system clock/reset gate controller --
// the C6's clock-gate block, which the chip programs and on which the MTIME rate
// depends) are the other two owns-for-life blocks.
size_t arch_reserved_blocks(struct arch_reserved_block* out, size_t max)
{
    static struct arch_reserved_block const blocks[] = {
        {0x20001000u, 0x1000u}, // PLIC (@+0x000) + CLINT MSIP/MTIME/MTIMECMP (@+0x800..) (TRM ch.1.7)
        {0x60010000u, 0x1000u}, // INTMTX: interrupt matrix (TRM, memory map Table 5.3-2)
        {0x60096000u, 0x1000u}, // PCR: clock + reset gate controller (TRM PCR ch.; regs @+0x2c/+0x30)
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
    c6_early_mark('A'); // entry reached: _start set gp/sp/tp and the call landed
    // The ROM loader copies the image segments to SRAM (LMA == VMA), so the copy is
    // a no-op; kept for uniformity with the other ports.
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
    c6_early_mark('H'); // arch_init returned -- kmain next (kbanner is the first console output)
    kickos::kmain(0, nullptr);
    arch_shutdown(0);
}

}
