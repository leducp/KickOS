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

#include <stdint.h>

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

    // --- Core-local CLINT (TRM ch.1.7; CPU sub-system base 0x2000_0000, regs @0x1800).
    constexpr uintptr_t CLINT_MSIP = 0x20001800;     // bit0: machine software int pending (switch)
    constexpr uintptr_t CLINT_MTIMECTL = 0x20001804; // MTCE|MTIE|MTIP|MTOF
    constexpr uintptr_t CLINT_MTIME = 0x20001808;    // 64-bit counter
    constexpr uintptr_t CLINT_MTIMECMP = 0x20001810; // 64-bit compare
    constexpr uint32_t MTIMECTL_MTCE = 1u << 0;      // enable the timer counter
    constexpr uint32_t MTIMECTL_MTIE = 1u << 1;      // enable the timer interrupt
    // MEASURED ~160 MHz on silicon (2026-07-09): a kos_sleep_ns(0.4s) beat ran 9.9x
    // too fast against the host wall clock, i.e. the CLINT MTIME is core-clocked at
    // the ROM's PLL frequency (~160 MHz), NOT the 16 MHz SYSTIMER rate first assumed.
    // The old 16 MHz made every C6 sleep/timeout run ~10x short. If a future clock
    // bring-up sets a different CPU frequency, update this to match.
    constexpr uint64_t MTIME_HZ = 160000000ull;
    constexpr uint64_t NS_PER_TICK = 1000000000ull / MTIME_HZ;

    // --- USB Serial/JTAG console (TRM ch.32; base 0x6000_F000). The ROM leaves it
    //     enumerated (it is the boot/console/flash path on this board), so we just
    //     write bytes: poll EP1_CONF[0] for FIFO room, write to EP1, flush via [1].
    constexpr uintptr_t USB_EP1 = 0x6000F000;             // RDWR_BYTE [7:0]
    constexpr uintptr_t USB_EP1_CONF = 0x6000F004;
    constexpr uint32_t USB_WR_DONE = 1u << 0;             // WT bit0: flush buffered bytes to host
    constexpr uint32_t USB_IN_EP_DATA_FREE = 1u << 1;     // RO bit1: TX FIFO has room

    // --- UART0 console (TRM ch.26; base 0x6000_0000). The real console on this board:
    //     UART0 (GPIO16/17) is bridged to the host by the CH343P (U4) as a plain COM
    //     port -- unlike the native USB-Serial-JTAG it does NOT re-enumerate on reset
    //     and has no CDC host-connection gating, so boot output is never dropped. The
    //     ROM already sets UART0 up (baud/pins) for its own boot log, so we just push
    //     bytes: poll STATUS.TXFIFO_CNT for room, write the FIFO. FIFO depth 128.
    constexpr uintptr_t UART0_FIFO = 0x60000000;          // write = push a TX byte
    constexpr uintptr_t UART0_STATUS = 0x6000001C;        // TXFIFO_CNT [23:16]
    constexpr uint32_t UART_TXFIFO_CNT_S = 16;
    constexpr uint32_t UART_TXFIFO_CNT_MASK = 0xFFu;
    constexpr uint32_t UART0_TXFIFO_LEN = 128;

    // --- Watchdogs (TRM ch.14 MWDT, ch.15 RWDT/SWD). ALL must be disabled or the
    //     ROM-armed WDTs reset the part within seconds. Common unlock key 0x50D83AA1.
    constexpr uint32_t WDT_WKEY = 0x50D83AA1;
    constexpr uintptr_t TIMG0_BASE = 0x60008000;
    constexpr uintptr_t TIMG1_BASE = 0x60009000;
    constexpr uintptr_t TIMG_WDTCONFIG0 = 0x0048;         // EN=bit31, FLASHBOOT_MOD_EN=bit14
    constexpr uintptr_t TIMG_WDTWPROTECT = 0x0064;
    constexpr uint32_t TIMG_WDT_EN = 1u << 31;
    constexpr uint32_t TIMG_WDT_FLASHBOOT = 1u << 14;

    constexpr uintptr_t RTC_WDT_BASE = 0x600B1C00;
    constexpr uintptr_t RTC_WDT_CONFIG0 = 0x0000;         // EN=bit31, FLASHBOOT_MOD_EN=bit12
    constexpr uintptr_t RTC_WDT_WPROTECT = 0x0018;
    constexpr uint32_t RTC_WDT_EN = 1u << 31;
    constexpr uint32_t RTC_WDT_FLASHBOOT = 1u << 12;
    constexpr uintptr_t RTC_SWD_CONFIG = 0x001C;          // SWD_DISABLE=bit30
    constexpr uintptr_t RTC_SWD_WPROTECT = 0x0020;
    constexpr uint32_t RTC_SWD_DISABLE = 1u << 30;

    // --- Interrupt matrix (INTMTX, base 0x6001_0000) + local interrupt controller
    //     (INTPRI, base 0x600C_5000). TRM v1.2 §1.6 + ch.10 (register offsets from
    //     Table 10.4.2). The C6 has no S-mode, so the arch's SSIP inject channel is a
    //     no-op here; instead we raise a REAL machine interrupt. A software-settable
    //     FROM_CPU source (INTPRI_CPU_INTR_FROM_CPU_0, level) is routed through the
    //     matrix to a dedicated CPU interrupt ID, which the C6 core vectors as
    //     mcause = ID (Espressif's custom scheme, not the standard mcause=11). ONE
    //     doorbell carries every logical inject line (arch keeps g_inject_line).
    constexpr uintptr_t INTMTX_BASE = 0x60010000;
    constexpr uintptr_t INTMTX_FROM_CPU_0_MAP = INTMTX_BASE + 0x0058; // [4:0]=target CPU int

    // The C6 uses the PLIC (base 0x2000_1000, M-mode window) as the CPU interrupt
    // controller -- NOT the INTPRI/INTC block (0x600C_5000), which is vestigial on
    // this core (esp-idf: "ESP32C6 should use the PLIC ... instead of INTC"). Enable,
    // type, per-int priority, and threshold ALL live in the PLIC. INTPRI keeps only the
    // software-settable FROM_CPU source triggers.
    constexpr uintptr_t PLIC_MX_BASE = 0x20001000;
    constexpr uintptr_t PLIC_MXINT_ENABLE = PLIC_MX_BASE + 0x0000;    // bit n: enable CPU int n
    constexpr uintptr_t PLIC_MXINT_TYPE = PLIC_MX_BASE + 0x0004;      // bit n: 0=level 1=edge
    constexpr uintptr_t PLIC_MXINT_CLEAR = PLIC_MX_BASE + 0x0008;     // bit n: edge-clear
    constexpr uintptr_t PLIC_MXINT_PRI_BASE = PLIC_MX_BASE + 0x0010;  // PRI_n @ +0x4*n, [3:0]=1..15
    constexpr uintptr_t PLIC_MXINT_THRESH = PLIC_MX_BASE + 0x0090;    // fire when prio > thresh
    constexpr uintptr_t PLIC_MXINT_CLAIM = PLIC_MX_BASE + 0x0094;     // pending/claim

    constexpr uintptr_t INTPRI_BASE = 0x600C5000;
    constexpr uintptr_t INTPRI_FROM_CPU_0 = INTPRI_BASE + 0x0090;     // bit0: assert the source

    // Dedicated CPU interrupt ID for the inject doorbell. Must be one of the C6's
    // external IDs (1-2, 5-6, 8-31; local CLINT owns 0/3/4/7) and not collide with
    // the switch.S demux (3=msip, 7=mtip). Shared with switch.S's .Lext arm via
    // rv_trap_ids.h so the two cannot drift.
    constexpr uint32_t DOORBELL_CPU_INT = KICKOS_RV_INJECT_DOORBELL_CPU_INT;
    constexpr uint32_t DOORBELL_PRIO = 7; // 1..15; sole external source, so uncontended

    void timg_mwdt_disable(uintptr_t base)
    {
        r32(base + TIMG_WDTWPROTECT) = WDT_WKEY;                       // unlock
        r32(base + TIMG_WDTCONFIG0) &= ~(TIMG_WDT_EN | TIMG_WDT_FLASHBOOT);
        r32(base + TIMG_WDTWPROTECT) = 0;                             // re-lock
    }

    void wdt_disable_all()
    {
        timg_mwdt_disable(TIMG0_BASE);
        timg_mwdt_disable(TIMG1_BASE);
        // RTC (LP) watchdog.
        r32(RTC_WDT_BASE + RTC_WDT_WPROTECT) = WDT_WKEY;
        r32(RTC_WDT_BASE + RTC_WDT_CONFIG0) &= ~(RTC_WDT_EN | RTC_WDT_FLASHBOOT);
        r32(RTC_WDT_BASE + RTC_WDT_WPROTECT) = 0;
        // Super watchdog (SWD): set the disable bit (its own write-protect key).
        r32(RTC_WDT_BASE + RTC_SWD_WPROTECT) = WDT_WKEY;
        r32(RTC_WDT_BASE + RTC_SWD_CONFIG) |= RTC_SWD_DISABLE;
        r32(RTC_WDT_BASE + RTC_SWD_WPROTECT) = 0;
    }

    // One-time inject-doorbell wiring. Called from arch_init with MIE still 0 (the
    // TRM's "configure the interrupt controller with interrupts globally disabled"
    // rule -- boot runs MIE=0 until arch_start mret's the first thread). Route the
    // FROM_CPU_0 source to CPU int 31, level-triggered, priority DOORBELL_PRIO,
    // enabled; then enable the matching mie bit (the C6's mie bit N gates CPU int N,
    // with bits 3/7 = the standard msip/mtip). FROM_CPU_0 is left de-asserted.
    void inject_doorbell_init()
    {
        r32(INTMTX_FROM_CPU_0_MAP) = DOORBELL_CPU_INT;            // route the source -> CPU int
        r32(PLIC_MXINT_PRI_BASE + 4u * DOORBELL_CPU_INT) = DOORBELL_PRIO;
        r32(PLIC_MXINT_TYPE) &= ~(1u << DOORBELL_CPU_INT);        // level
        r32(PLIC_MXINT_THRESH) = 0;                              // fire for any prio > 0
        r32(PLIC_MXINT_ENABLE) |= (1u << DOORBELL_CPU_INT);      // enable at the PLIC
        __asm volatile("fence" ::: "memory");                    // settle before MIE is enabled
        __asm volatile("csrs mie, %0" ::"r"(1u << DOORBELL_CPU_INT) : "memory");
    }

    // --- Diagnostic LED: onboard WS2812B (board LED2, DI on GPIO8, VDD tied to 3V3,
    //     no enable pin). GPIO bit-bang FAILS here: the register-write latency exceeds
    //     the WS2812B ~400 ns bit high-time even at 160 MHz, so software cannot form
    //     valid bits (LED latched solid white). The RMT peripheral (TRM ch.30) clocks
    //     the pulse train in hardware, so timing is exact. GRB order, 24 bits MSB
    //     first. Panic path: single frame, polled, no interrupts/DMA.

    // RMT (TRM ch.30; base 0x6000_6000). Two TX channels (0,1) + two RX (2,3); TX
    // channels own their RAM implicitly (no MEM_OWNER). We use channel 0.
    constexpr uintptr_t RMT_BASE = 0x60006000;
    constexpr uintptr_t RMT_CH0CONF0 = RMT_BASE + 0x10;  // one config reg per TX channel
    constexpr uintptr_t RMT_INT_RAW = RMT_BASE + 0x38;
    constexpr uintptr_t RMT_INT_CLR = RMT_BASE + 0x44;
    constexpr uintptr_t RMT_SYS_CONF = RMT_BASE + 0x68;
    constexpr uintptr_t RMT_CH0_RAM = RMT_BASE + 0x400; // 48-word (192 B) channel-0 block
    // CH0CONF0 bits. tx_start/mem_rd_rst/apb_mem_rst/conf_update are write-triggered
    // (self-clearing); div_cnt/mem_size/idle/carrier are R/W and need conf_update to latch.
    constexpr uint32_t RMT_TX_START = 1u << 0;
    constexpr uint32_t RMT_MEM_RD_RST = 1u << 1;
    constexpr uint32_t RMT_APB_MEM_RST = 1u << 2;
    constexpr uint32_t RMT_IDLE_OUT_EN = 1u << 6;   // drive idle level (idle_out_lv=0 -> rest low)
    constexpr uint32_t RMT_CARRIER_EN = 1u << 21;   // default 1 -- MUST clear (no IR carrier)
    constexpr uint32_t RMT_CONF_UPDATE = 1u << 24;
    constexpr uint32_t RMT_DIV_CNT_S = 8;           // [15:8] per-channel clock divider
    constexpr uint32_t RMT_MEM_SIZE_S = 16;         // [18:16] RAM blocks
    constexpr uint32_t RMT_CH0_TX_END = 1u << 0;    // INT_RAW bit0: TX done
    constexpr uint32_t RMT_APB_FIFO_MASK = 1u << 0; // SYS_CONF: 1 = access RAM directly (not FIFO)

    // RMT clock is muxed/divided in PCR on the C6 (not RMT_SYS_CONF). Source select
    // SCLK_SEL: 1=PLL_F80M, 2=RC_FAST, 3=XTAL. Group divisor = SCLK_DIV_NUM+1.
    constexpr uintptr_t PCR_RMT_CONF = 0x60096000 + 0x2c;      // CLK_EN bit0, RST_EN bit1
    constexpr uintptr_t PCR_RMT_SCLK_CONF = 0x60096000 + 0x30;
    constexpr uint32_t PCR_RMT_CLK_EN = 1u << 0;
    constexpr uint32_t PCR_RMT_RST_EN = 1u << 1;
    constexpr uint32_t PCR_RMT_SCLK_EN = 1u << 22;
    constexpr uint32_t PCR_RMT_SCLK_SEL_S = 20;                // [21:20]
    constexpr uint32_t PCR_RMT_SCLK_DIV_NUM_S = 12;            // [19:12]

    // GPIO matrix + IO_MUX (GPIO8). Route RMT ch-0 TX signal (index 71) out to GPIO8:
    // FUNC8_OUT_SEL = signal index (NOT 128), output enable, IO_MUX MCU_SEL=1 (GPIO func).
    constexpr uintptr_t GPIO_ENABLE_W1TS = 0x60091000 + 0x24;
    constexpr uintptr_t GPIO_FUNC8_OUT_SEL_CFG = 0x60091000 + 0x574;
    constexpr uintptr_t IO_MUX_GPIO8 = 0x60090000 + 0x24;
    constexpr uint32_t RMT_SIG_OUT0_IDX = 71;                  // gpio_sig_map.h
    constexpr uint32_t IO_MUX_MCU_SEL_GPIO = 1u << 12;         // MCU_SEL=1 -> GPIO matrix
    constexpr uint32_t IO_MUX_FUN_DRV_2 = 2u << 10;            // ~20 mA drive

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
        (RMT_DIV_CNT << RMT_DIV_CNT_S) | (1u << RMT_MEM_SIZE_S) | RMT_IDLE_OUT_EN;

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
        volatile uint32_t* ram = reinterpret_cast<volatile uint32_t*>(RMT_CH0_RAM);
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

        r32(RMT_INT_CLR) = RMT_CH0_TX_END;                        // clear stale done flag
        r32(RMT_CH0CONF0) = RMT_CH0_CFG | RMT_MEM_RD_RST | RMT_APB_MEM_RST; // reset RAM pointers
        r32(RMT_CH0CONF0) = RMT_CH0_CFG;
        r32(RMT_CH0CONF0) = RMT_CH0_CFG | RMT_CONF_UPDATE;        // latch config
        r32(RMT_CH0CONF0) = RMT_CH0_CFG | RMT_TX_START;           // go

        // Blocking (panic ctx: interrupts masked, no DMA). Bounded so a wedged RMT
        // never hangs the fault path -- ~25 words * 1.25 us + 60 us latch is < 100 us.
        uint32_t spin = 0;
        while ((r32(RMT_INT_RAW) & RMT_CH0_TX_END) == 0)
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

// --- Console: UART0, bridged to the host by the on-board CH343P. Bounded poll on
//     the TX FIFO count so a stalled UART drops bytes instead of hanging the kernel.
//     (The native USB-Serial-JTAG at 0x6000_F000 does not reliably deliver output
//     once the app takes over -- it needs the host CDC port actively draining and
//     re-enumerates on every reset; UART0 has neither problem.)
void arch_console_write(char const* buf, size_t n)
{
    for (size_t i = 0; i < n; i++)
    {
        uint32_t spin = 0;
        while (((r32(UART0_STATUS) >> UART_TXFIFO_CNT_S) & UART_TXFIFO_CNT_MASK) >= UART0_TXFIFO_LEN - 2)
        {
            if (++spin > 200000u)
            {
                return; // FIFO not draining -> drop (never block the kernel)
            }
        }
        r32(UART0_FIFO) = static_cast<uint8_t>(buf[i]);
    }
}

// --- Tickless clock: the 64-bit CLINT MTIME -> ns -------------------------------
uint64_t arch_clock_now(void)
{
    volatile uint32_t* mt = r32p(CLINT_MTIME);
    uint32_t hi, lo, hi2;
    do
    {
        hi = mt[1];
        lo = mt[0];
        hi2 = mt[1];
    } while (hi != hi2);
    uint64_t t = (static_cast<uint64_t>(hi) << 32) | lo;
    return t * NS_PER_TICK;
}

// --- One-shot next-event timer: CLINT MTIMECMP (fires when MTIME >= MTIMECMP) ----
void arch_timer_arm(uint64_t deadline_ns)
{
    uint64_t ticks = deadline_ns / NS_PER_TICK;
    volatile uint32_t* cmp = r32p(CLINT_MTIMECMP);
    cmp[1] = 0xFFFFFFFFu; // park high half so no spurious match between the two stores
    cmp[0] = static_cast<uint32_t>(ticks);
    cmp[1] = static_cast<uint32_t>(ticks >> 32);
}

void arch_timer_disarm(void)
{
    volatile uint32_t* cmp = r32p(CLINT_MTIMECMP);
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
    r32(INTPRI_FROM_CPU_0) = 1;
}

// EOI, run at the head of the .Lext trap: de-assert the level source so it does not
// re-fire on mret, then fence so the de-assert settles (INTPRI is APB, multi-cycle).
void arch_rv_ext_eoi(void)
{
    r32(INTPRI_FROM_CPU_0) = 0;
    __asm volatile("fence" ::: "memory");
}

// --- Kernel diagnostic LED: onboard WS2812B on GPIO8, driven by RMT channel 0.
void arch_diag_led_init(void)
{
    // Ungate + reset the RMT, then select its source clock. PCR owns both on the C6.
    r32(PCR_RMT_CONF) |= PCR_RMT_CLK_EN;                       // APB register clock
    r32(PCR_RMT_CONF) |= PCR_RMT_RST_EN;                       // assert peripheral reset
    r32(PCR_RMT_CONF) &= ~PCR_RMT_RST_EN;                      // deassert
    // XTAL 40 MHz source, group divisor 1 (DIV_NUM field 0), function clock enabled.
    r32(PCR_RMT_SCLK_CONF) = (3u << PCR_RMT_SCLK_SEL_S) | (0u << PCR_RMT_SCLK_DIV_NUM_S) | PCR_RMT_SCLK_EN;

    r32(RMT_SYS_CONF) |= RMT_APB_FIFO_MASK;                    // access channel RAM directly
    // Channel 0: div_cnt=2 (-> 20 MHz tick), 1 RAM block, idle drives low (WS2812 reset),
    // carrier off. Latch it.
    r32(RMT_CH0CONF0) = RMT_CH0_CFG;                           // carrier_en defaults 1 -> cleared here
    r32(RMT_CH0CONF0) = RMT_CH0_CFG | RMT_CONF_UPDATE;

    // Route RMT ch-0 TX -> GPIO8: GPIO matrix out-sel = signal 71, output enable,
    // IO_MUX pad on the GPIO function with a driver.
    r32(GPIO_FUNC8_OUT_SEL_CFG) = RMT_SIG_OUT0_IDX;
    r32(GPIO_ENABLE_W1TS) = 1u << 8;
    r32(IO_MUX_GPIO8) = IO_MUX_MCU_SEL_GPIO | IO_MUX_FUN_DRV_2;

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

void arch_init(void)
{
    wdt_disable_all(); // or the ROM-armed watchdogs reset the part in seconds

    g_clint_msip = r32p(CLINT_MSIP);   // the deferred-switch software interrupt
#if KICKOS_BENCH
    // The C6 traps on `rdcycle`; give the bench the core-clocked CLINT MTIME low word
    // (== CPU cycles at this PLL) as its free-running counter. Set before any switch.
    extern volatile uint32_t* g_bench_cycle_src;
    g_bench_cycle_src = r32p(CLINT_MTIME);
#endif
    arch_timer_disarm();               // MTIMECMP = max: no timer fire until armed
    r32(CLINT_MTIMECTL) = MTIMECTL_MTCE | MTIMECTL_MTIE; // start the counter + enable

    kickos_rv32_init();  // vectored mtvec + mie(MSIE|MTIE|SSIE) + mcounteren + PMP
    inject_doorbell_init(); // wire the interrupt matrix FROM_CPU doorbell (device IRQs)
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

// --- C-runtime bring-up (the reset entry, called by _start in startup.S) ------
void Reset_Handler(void)
{
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
    for (void (**fn)() = __init_array_start; fn != __init_array_end; fn++)
    {
        (*fn)();
    }
    arch_init();
    kickos::kmain(0, nullptr);
    arch_shutdown(0);
}

}
