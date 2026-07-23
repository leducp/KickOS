// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Raspberry Pi RP2350 (Cortex-M33) chip backend. Register addresses/fields are
// clean-room from the RP2350 datasheet (RP-008373-DS-2); hand-rolled, no vendor
// SDK sources, consistent with the arch layer's regs.h. Section numbers in the
// comments cite that datasheet.
//
// First-pass scope: privilege + SVC on the reused armv7m arch, NO hardware MPU
// (the armv8-m/PMSAv8 backend is deferred -- see docs/design-rp2350.md). clk_sys
// is raised to 150 MHz off PLL_SYS (12 MHz XOSC x125 /5 /2, the datasheet default
// max, 8.6); SystemCoreClock tracks it so the SysTick ns<->cycle math
// (arch_arm_common) stays coherent. clk_ref stays on the 12 MHz XOSC and drives
// the TICKS TIMER0 generator (/12 -> 1 MHz), so the 64-bit system TIMER0
// (arch_clock_now / arch_trace_now) is PLL-independent. clk_peri follows clk_sys,
// so the UART baud divisors are recomputed for 150 MHz. If the crystal or the PLL
// never comes up the board degrades to XOSC/ROSC timing instead of hanging.
//
// Key deltas from the RP2040 (all APB peripheral bases relocated; datasheet 2.2.4):
//   - No boot2/CRC stage: the bootrom does XIP setup + reads SP/PC from the vector
//     table (startup.S / rp2350.ld).
//   - The system TIMER tick comes from the new common TICKS block (8.5), not the
//     watchdog.
//   - PADS gained an ISO (isolation) bit that resets SET and must be cleared to use
//     a pad (9.11.3).
//   - 52 NVIC lines; the console is on UART1 (UART1_IRQ = 34, 3.2) -- see the
//     IO_BANK0 block below for why the Pi-Zero header forces UART1, not UART0.
//
// NOT run in this environment (no RP2350 model in mainline QEMU; no bench access);
// verified by build + image inspection. Flash via BOOTSEL/picotool to confirm UART1
// output on GP4 (Waveshare RP2350-Pi-Zero 40-pin header pin 8). The board is always
// BOOTSEL-recoverable, so a wrong
// clock/boot config cannot permanently brick it.

#include <kickos/arch/arch.h>
#include <kickos/config/limits.h>
#include <kickos/console_tx.h>

#include <stdint.h>

namespace kickos
{
    int kmain(int argc, char** argv);
}

extern "C"
{
void kickos_armv7m_init(void);
#if KICKOS_HAVE_MPU
// PMSAv8 MPU backend (arch/arm/common/arch_arm_pmsav8.cc): one-time MAIR + MemManage
// enable. This reference is also the LINK ANCHOR that pulls the PMSAv8 member so its
// strong kickos_arch_mpu_commit / arch_mpu_region_encodable win over the weak v7-M defs.
void kickos_arm_pmsav8_init(void);
#endif

extern void (*__init_array_start[])();
extern void (*__init_array_end[])();

// Pre-init value (12 MHz XOSC, reset). clocks_init() raises this to 150 MHz
// once clk_sys is on PLL_SYS; SysTick (processor clock) reads it live.
uint32_t SystemCoreClock = 12000000u;
}

namespace
{
    inline volatile uint32_t& r32(uintptr_t a)
    {
        return *reinterpret_cast<volatile uint32_t*>(a);
    }

    // Atomic register-access aliases (datasheet 2.1.3): every APB peripheral is
    // mirrored at base+0x2000 (bitmask SET on write) and base+0x3000 (bitmask
    // CLEAR on write), so a single-bit change needs no read-modify-write.
    constexpr uintptr_t ATOMIC_SET = 0x2000;
    constexpr uintptr_t ATOMIC_CLR = 0x3000;

    // --- RESETS (0x40020000, datasheet 7.5): peripherals held in reset at power-up.
    constexpr uintptr_t RESETS_BASE = 0x40020000;
    constexpr uintptr_t RESETS_RESET = RESETS_BASE + 0x0;
    constexpr uintptr_t RESETS_RESET_DONE = RESETS_BASE + 0x8;
    constexpr uint32_t RESET_IO_BANK0 = 1u << 6;
    constexpr uint32_t RESET_PADS_BANK0 = 1u << 9;
    constexpr uint32_t RESET_PLL_SYS = 1u << 14;
    constexpr uint32_t RESET_TIMER0 = 1u << 23;
    constexpr uint32_t RESET_UART1 = 1u << 27; // RP2350 DS Table 535 (RESETS: RESET)

    // --- XOSC (0x40048000, datasheet 8.2): 12 MHz crystal ---------------------
    constexpr uintptr_t XOSC_BASE = 0x40048000;
    constexpr uintptr_t XOSC_CTRL = XOSC_BASE + 0x0;
    constexpr uintptr_t XOSC_STATUS = XOSC_BASE + 0x4;
    constexpr uintptr_t XOSC_STARTUP = XOSC_BASE + 0xc;
    constexpr uint32_t XOSC_FREQ_1_15MHZ = 0xaa0;  // CTRL.FREQ_RANGE (12 MHz)
    constexpr uint32_t XOSC_ENABLE = 0xfabu << 12; // CTRL.ENABLE magic
    constexpr uint32_t XOSC_STATUS_STABLE = 1u << 31;
    // STARTUP.DELAY counts in units of 256 crystal periods; ceil(12e6*1ms/256)=47.
    constexpr uint32_t XOSC_STARTUP_DELAY = 47;

    // --- CLOCKS (0x40010000, datasheet 8.1) -----------------------------------
    constexpr uintptr_t CLOCKS_BASE = 0x40010000;
    constexpr uintptr_t CLK_REF_CTRL = CLOCKS_BASE + 0x30;
    constexpr uintptr_t CLK_REF_SELECTED = CLOCKS_BASE + 0x38;
    constexpr uintptr_t CLK_SYS_CTRL = CLOCKS_BASE + 0x3c;
    constexpr uintptr_t CLK_SYS_SELECTED = CLOCKS_BASE + 0x44;
    constexpr uintptr_t CLK_PERI_CTRL = CLOCKS_BASE + 0x48;
    constexpr uint32_t CLK_REF_SRC_XOSC = 0x2;          // CTRL.SRC glitchless
    constexpr uint32_t CLK_REF_SELECTED_XOSC = 1u << 2; // one-hot readback (1<<SRC)
    // clk_sys glitchless mux: SRC bit0 (0=clk_ref, 1=aux); AUXSRC[7:5]=0 selects
    // clksrc_pll_sys. SELECTED is one-hot on SRC (bit0=ref, bit1=aux).
    constexpr uint32_t CLK_SYS_AUXSRC_PLL = 0x0u << 5;
    constexpr uint32_t CLK_SYS_SRC_REF = 0x0;
    constexpr uint32_t CLK_SYS_SRC_AUX = 0x1;
    constexpr uint32_t CLK_SYS_SELECTED_REF = 1u << 0;
    constexpr uint32_t CLK_SYS_SELECTED_AUX = 1u << 1;
    constexpr uint32_t CLK_SYS_HZ = 150000000u; // clk_sys once on PLL_SYS
    // clk_peri: ENABLE(bit11) | AUXSRC[7:5]. XOSC=0x4 (12 MHz, used if the PLL never
    // locks); CLK_SYS=0x0 tracks clk_sys (150 MHz on PLL, 12 MHz clk_ref fallback).
    constexpr uint32_t CLK_PERI_ENABLE_XOSC = (1u << 11) | (0x4u << 5);
    constexpr uint32_t CLK_PERI_ENABLE_CLK_SYS = (1u << 11) | (0x0u << 5);

    // --- PLL_SYS (0x40050000, datasheet 8.6): 12 MHz XOSC / REFDIV x FBDIV = VCO,
    // then / POSTDIV1 / POSTDIV2. 12 / 1 * 125 = 1500 MHz VCO (in 750..1600), /5 /2
    // = 150 MHz (the datasheet default max clk_sys). ------------------------------
    constexpr uintptr_t PLL_SYS_BASE = 0x40050000;
    constexpr uintptr_t PLL_SYS_CS = PLL_SYS_BASE + 0x0;
    constexpr uintptr_t PLL_SYS_PWR = PLL_SYS_BASE + 0x4;
    constexpr uintptr_t PLL_SYS_FBDIV_INT = PLL_SYS_BASE + 0x8;
    constexpr uintptr_t PLL_SYS_PRIM = PLL_SYS_BASE + 0xc;
    constexpr uint32_t PLL_CS_LOCK = 1u << 31;
    constexpr uint32_t PLL_CS_REFDIV_1 = 1u; // CS.REFDIV[5:0]
    constexpr uint32_t PLL_FBDIV_125 = 125u; // FBDIV_INT[11:0]
    constexpr uint32_t PLL_PWR_PD = 1u << 0;
    constexpr uint32_t PLL_PWR_POSTDIVPD = 1u << 3;
    constexpr uint32_t PLL_PWR_VCOPD = 1u << 5;
    constexpr uint32_t PLL_PRIM_POSTDIV = (5u << 16) | (2u << 12); // POSTDIV1=5, POSTDIV2=2

    // --- TICKS (0x40108000, datasheet 8.5): the new common tick generators. The
    // system TIMER0 tick (1 us) comes from here, not the watchdog (the RP2040 model).
    // TIMER0 generator triplet: CTRL 0x18, CYCLES 0x1c, COUNT 0x20. ---------------
    constexpr uintptr_t TICKS_BASE = 0x40108000;
    constexpr uintptr_t TICKS_TIMER0_CTRL = TICKS_BASE + 0x18;
    constexpr uintptr_t TICKS_TIMER0_CYCLES = TICKS_BASE + 0x1c;
    constexpr uint32_t TICKS_CTRL_ENABLE = 1u << 0;
    // tick = clk_ref / CYCLES. clk_ref is 12 MHz (XOSC) normally, ~6.5 MHz (ROSC) in
    // the fallback -> pick CYCLES to land near 1 MHz either way.
    constexpr uint32_t TICKS_CYCLES_12MHZ = 12u;
    constexpr uint32_t TICKS_CYCLES_ROSC = 7u;
    // ROSC reset frequency is ~6.5 MHz (uncalibrated); used only for approximate
    // SysTick timing if the crystal never comes up.
    constexpr uint32_t ROSC_NOMINAL_HZ = 6500000u;

    // --- TIMER0 (0x400b0000, datasheet 12.8): 64-bit microsecond monotonic counter.
    // RAW halves (no latching) so the 64-bit read stays core-safe without an IRQ
    // guard (see arch_clock_now), unlike the latching TIMELR/TIMEHR pair. Requires
    // the TICKS TIMER0 generator running (SOURCE defaults to that 1 us tick). ------
    constexpr uintptr_t TIMER0_BASE = 0x400b0000;
    constexpr uintptr_t TIMER0_TIMERAWH = TIMER0_BASE + 0x24;
    constexpr uintptr_t TIMER0_TIMERAWL = TIMER0_BASE + 0x28;

    // --- IO_BANK0 (0x40028000, datasheet 9.11.1): pin function select ----------
    // The Waveshare RP2350-Pi-Zero routes the 40-pin header UART (Pi pin 8 "TXD",
    // pin 10 "RXD") to RP2350 GP4/GP5, NOT GP0/GP1 (board schematic, connector J5).
    // GP4/GP5 mux ONLY UART1 (F2); UART0 does not reach these pins on any funcsel
    // (RP2350 DS Table 2, GPIO Bank 0 functions). CTRL = base + 0x04 + n*0x08.
    constexpr uintptr_t IO_BANK0_BASE = 0x40028000;
    constexpr uintptr_t IO_GPIO4_CTRL = IO_BANK0_BASE + 0x24; // GP4 = UART1 TX
    constexpr uintptr_t IO_GPIO5_CTRL = IO_BANK0_BASE + 0x2c; // GP5 = UART1 RX
    constexpr uint32_t IO_FUNCSEL_UART = 2;                   // F2 = UART1 TX/RX

    // --- PADS_BANK0 (0x40038000, datasheet 9.11.3) ----------------------------
    constexpr uintptr_t PADS_BANK0_BASE = 0x40038000;
    constexpr uintptr_t PADS_GPIO4 = PADS_BANK0_BASE + 0x14; // base + 0x04 + n*0x04
    constexpr uintptr_t PADS_GPIO5 = PADS_BANK0_BASE + 0x18;
    constexpr uint32_t PAD_ISO = 1u << 8; // pad isolation (resets SET -- clear to use)
    constexpr uint32_t PAD_OD = 1u << 7;  // output disable
    constexpr uint32_t PAD_IE = 1u << 6;  // input enable

    // --- UART1 (0x40078000, datasheet 12.1): ARM PL011. The console is on UART1,
    // not UART0: the Pi-Zero header TX/RX pins land on GP4/GP5, which mux only UART1.
    constexpr uintptr_t UART1_BASE = 0x40078000;
    constexpr uintptr_t UART1_DR = UART1_BASE + 0x00;
    constexpr uintptr_t UART1_FR = UART1_BASE + 0x18;
    constexpr uintptr_t UART1_IBRD = UART1_BASE + 0x24;
    constexpr uintptr_t UART1_FBRD = UART1_BASE + 0x28;
    constexpr uintptr_t UART1_LCR_H = UART1_BASE + 0x2c;
    constexpr uintptr_t UART1_CR = UART1_BASE + 0x30;
    constexpr uintptr_t UART1_IMSC = UART1_BASE + 0x38; // interrupt mask set/clear
    constexpr uint32_t FR_TXFF = 1u << 5;               // TX (single holding location) full
    constexpr uint32_t IMSC_TXIM = 1u << 5;             // transmit interrupt mask
    // baud = clk_peri / (16 x (IBRD + FBRD/64)), FBRD = round(frac x 64). clk_peri
    // 12 MHz, 115200 -> IBRD 6, FBRD 33; clk_peri 150 MHz (clk_sys on PLL) -> IBRD 81,
    // FBRD 24 (actual 115207 baud, +0.006%).
    constexpr uint32_t UART_IBRD_115200 = 6;
    constexpr uint32_t UART_FBRD_115200 = 33;
    constexpr uint32_t UART_IBRD_150MHZ = 81;
    constexpr uint32_t UART_FBRD_150MHZ = 24;
    // Chosen by clocks_init (which source clk_peri lands on), consumed by uart1_init.
    // Boot is single-threaded and sequential, so no guard is needed.
    uint32_t g_uart_ibrd = UART_IBRD_115200;
    uint32_t g_uart_fbrd = UART_FBRD_115200;
    // WLEN=8, no parity, one stop. FEN is deliberately LEFT OFF: with the TX FIFO
    // enabled the PL011 transmit interrupt fires only as the FIFO descends through
    // the watermark, so a one-byte prime would never re-trigger the drain. With the
    // FIFO disabled the ring's idle->busy prime (console_tx.cc) starts the transfer
    // regardless of level-vs-transition trigger at rest (HW-unverified; build-only).
    constexpr uint32_t LCR_H_8N1 = (0x3u << 5);                       // WLEN=8
    constexpr uint32_t CR_ENABLE = (1u << 0) | (1u << 8) | (1u << 9); // UARTEN,TXE,RXE

    // Bounded so a dead/missing crystal or stuck peripheral degrades instead of
    // hanging the boot forever. The cap is far longer than any legitimate wait.
    constexpr uint32_t POLL_TIMEOUT = 1000000u;

    bool wait_mask(uintptr_t addr, uint32_t mask)
    {
        for (uint32_t i = 0; i < POLL_TIMEOUT; i++)
        {
            if ((r32(addr) & mask) == mask)
            {
                return true;
            }
        }
        return false;
    }

    void unreset(uint32_t mask)
    {
        r32(RESETS_RESET + ATOMIC_CLR) = mask;
        wait_mask(RESETS_RESET_DONE, mask); // bounded; best-effort
    }

    // Bring PLL_SYS up to 150 MHz. Returns false (PLL left powered down) if the VCO
    // never locks, so the caller can stay on the crystal instead of switching
    // clk_sys onto a dead PLL. Datasheet 8.6.4 sequence.
    bool pll_sys_lock()
    {
        // Reset the block first so a warm reboot can't run this off stale dividers.
        r32(RESETS_RESET + ATOMIC_SET) = RESET_PLL_SYS;
        r32(RESETS_RESET + ATOMIC_CLR) = RESET_PLL_SYS;
        wait_mask(RESETS_RESET_DONE, RESET_PLL_SYS);

        // Load REFDIV + FBDIV BEFORE powering the VCO.
        r32(PLL_SYS_CS) = PLL_CS_REFDIV_1;
        r32(PLL_SYS_FBDIV_INT) = PLL_FBDIV_125;
        // Power up main regulator + VCO (clear PD, VCOPD). DSMPD stays set (integer
        // FBDIV, no delta-sigma); POSTDIVPD stays set until after lock.
        r32(PLL_SYS_PWR + ATOMIC_CLR) = PLL_PWR_PD | PLL_PWR_VCOPD;
        if (not wait_mask(PLL_SYS_CS, PLL_CS_LOCK))
        {
            return false;
        }
        r32(PLL_SYS_PRIM) = PLL_PRIM_POSTDIV;
        r32(PLL_SYS_PWR + ATOMIC_CLR) = PLL_PWR_POSTDIVPD; // enable post-dividers
        return true;
    }

    // Start the TICKS TIMER0 generator so the 64-bit system TIMER0 counts. The
    // generator must be stopped before CYCLES is changed (datasheet 8.5.1).
    void ticks_timer0_start(uint32_t cycles)
    {
        r32(TICKS_TIMER0_CTRL) = 0; // disable while reprogramming
        r32(TICKS_TIMER0_CYCLES) = cycles;
        r32(TICKS_TIMER0_CTRL) = TICKS_CTRL_ENABLE;
    }

    void clocks_init()
    {
        // Bring up the 12 MHz crystal and put clk_ref on it. If it never stabilizes,
        // degrade to the ROSC that clk_sys already runs on at reset so the board still
        // boots (approximate timing) instead of hanging.
        r32(XOSC_STARTUP) = XOSC_STARTUP_DELAY;
        // Program the frequency range, THEN start the oscillator (datasheet 8.2.7): a
        // combined write is avoided so ENABLE never latches before FREQ_RANGE is set.
        r32(XOSC_CTRL) = XOSC_FREQ_1_15MHZ;
        r32(XOSC_CTRL + ATOMIC_SET) = XOSC_ENABLE;

        bool xosc_ok = wait_mask(XOSC_STATUS, XOSC_STATUS_STABLE);
        if (xosc_ok)
        {
            // clk_ref <- XOSC (glitchless mux); clk_sys follows to 12 MHz via its
            // SRC=clk_ref reset default. Poll the one-hot SELECTED before proceeding.
            r32(CLK_REF_CTRL) = CLK_REF_SRC_XOSC;
            xosc_ok = wait_mask(CLK_REF_SELECTED, CLK_REF_SELECTED_XOSC);
        }

        if (not xosc_ok)
        {
            SystemCoreClock = ROSC_NOMINAL_HZ;            // clk_sys stayed on ROSC
            r32(CLK_PERI_CTRL) = CLK_PERI_ENABLE_CLK_SYS; // UART clock <- clk_sys
            ticks_timer0_start(TICKS_CYCLES_ROSC);        // ~6.5 MHz / 7 ~= 1 MHz
            return;
        }

        // clk_ref stays on the 12 MHz XOSC: the TICKS TIMER0 /12 tick and thus the
        // 1 MHz system TIMER0 (arch_clock_now / arch_trace_now) derive from clk_ref
        // and MUST NOT track the PLL.
        ticks_timer0_start(TICKS_CYCLES_12MHZ); // 12 MHz / 12 = 1 MHz tick

        if (pll_sys_lock())
        {
            // Switch the clk_sys glitchless mux onto the PLL (datasheet 8.1.3.2): set
            // AUXSRC while still on clk_ref, then flip SRC to aux and poll SELECTED.
            r32(CLK_SYS_CTRL) = CLK_SYS_AUXSRC_PLL | CLK_SYS_SRC_REF;
            wait_mask(CLK_SYS_SELECTED, CLK_SYS_SELECTED_REF);
            r32(CLK_SYS_CTRL) = CLK_SYS_AUXSRC_PLL | CLK_SYS_SRC_AUX;
            wait_mask(CLK_SYS_SELECTED, CLK_SYS_SELECTED_AUX);
            // CLK_SYS_DIV stays at its reset value (/1). Update the core-clock truth in
            // the SAME step (arch_arm_common SysTick reads SystemCoreClock).
            SystemCoreClock = CLK_SYS_HZ;
            g_uart_ibrd = UART_IBRD_150MHZ;
            g_uart_fbrd = UART_FBRD_150MHZ;
            r32(CLK_PERI_CTRL) = CLK_PERI_ENABLE_CLK_SYS; // UART clock <- clk_sys 150 MHz
        }
        else
        {
            // PLL never locked: clk_sys still follows clk_ref (12 MHz). SystemCoreClock
            // and the UART divisors keep their 12 MHz defaults.
            r32(CLK_PERI_CTRL) = CLK_PERI_ENABLE_XOSC; // UART clock <- XOSC 12 MHz
        }
    }

    void uart1_init()
    {
        // Route GP4/GP5 to UART1 and make the pads usable. The RP2350 pads reset
        // ISOLATED (PAD_ISO set) -- clear it or the pad stays disconnected.
        r32(IO_GPIO4_CTRL) = IO_FUNCSEL_UART;
        r32(IO_GPIO5_CTRL) = IO_FUNCSEL_UART;
        r32(PADS_GPIO4 + ATOMIC_CLR) = PAD_ISO | PAD_OD; // TX: connect, drive out
        r32(PADS_GPIO5 + ATOMIC_CLR) = PAD_ISO;          // RX: connect
        r32(PADS_GPIO5 + ATOMIC_SET) = PAD_IE;           // RX: input enable

        // Divisors latch only on the subsequent LCR_H write, so order matters.
        r32(UART1_IBRD) = g_uart_ibrd;
        r32(UART1_FBRD) = g_uart_fbrd;
        r32(UART1_LCR_H) = LCR_H_8N1;
        r32(UART1_IMSC) = 0; // all UART interrupt sources masked; the ring arms TXIM
        r32(UART1_CR) = CR_ENABLE;
    }

    // --- Buffered console TX backend (console_tx.h). The ring drains via the PL011
    // transmit interrupt with the FIFO disabled (see LCR_H_8N1); the idle->busy prime
    // starts the transfer. slot_free/push touch one data register; irq_enable/disable
    // use the RP2350 atomic set/clear aliases so no read-modify-write on IMSC. ---
    int rp_tx_slot_free(void)
    {
        return (r32(UART1_FR) & FR_TXFF) == 0;
    }
    void rp_tx_push(uint8_t b)
    {
        r32(UART1_DR) = b;
    }
    void rp_tx_irq_enable(void)
    {
        r32(UART1_IMSC + ATOMIC_SET) = IMSC_TXIM;
    }
    void rp_tx_irq_disable(void)
    {
        r32(UART1_IMSC + ATOMIC_CLR) = IMSC_TXIM;
    }

    constexpr uint32_t CONSOLE_TX_SIZE = 512; // power of two; > kprintf's 256B buffer
    char console_tx_buf[CONSOLE_TX_SIZE];
    console_tx_backend const rp_console_backend = {
        rp_tx_slot_free, rp_tx_push, rp_tx_irq_enable, rp_tx_irq_disable};

    // NVIC: UART1_IRQ = line 34 (RP2350 datasheet 3.2, Table 95). Only TXIM is armed,
    // so the drain ISR is the sole source on this line.
    constexpr int UART1_IRQ = 34;
}

extern "C"
{

void arch_init(void)
{
    // Reset-release ordering is load-bearing (the RP2040 lesson): a peripheral's
    // RESET_DONE only asserts once it has a running clock. IO_BANK0/PADS_BANK0/TIMER0
    // are clocked by clk_sys/clk_ref (already live off the ROSC at reset), so release
    // them now. UART1 is clocked by clk_peri, which is OFF until clocks_init -- release
    // it BEFORE that and its RESET_DONE never asserts, hanging the boot.
    unreset(RESET_IO_BANK0 | RESET_PADS_BANK0 | RESET_TIMER0);
    clocks_init();
    unreset(RESET_UART1);
    uart1_init();
#if KICKOS_HAVE_MPU
    kickos_arm_pmsav8_init(); // MAIR + MemManage; first switch enables the MPU
#endif
    kickos_armv7m_init();
}

void arch_console_write(char const* buf, size_t n)
{
    console_tx_write(buf, n); // buffered; the routing guard (console.cc) keeps this thread-only
}

void arch_console_write_sync(char const* buf, size_t n)
{
    for (size_t i = 0; i < n; i++)
    {
        uint32_t spin = 0;
        while ((r32(UART1_FR) & FR_TXFF) != 0)
        {
            if (++spin > KICKOS_POLL_SPIN_MAX)
            {
                return; // bounded: a wedged UART must not hang the panic path (drop)
            }
        }
        r32(UART1_DR) = static_cast<uint8_t>(buf[i]);
    }
}

console_tx_backend const* arch_console_tx_backend(char** storage, uint32_t* size, int* irq_line)
{
    *storage = console_tx_buf;
    *size = CONSOLE_TX_SIZE;
    *irq_line = UART1_IRQ;
    return &rp_console_backend;
}

// Monotonic clock from the 64-bit system TIMER0 (microseconds -> ns). Uses the
// non-latching RAW halves with a hi/lo/hi re-read to tolerate a 32-bit rollover
// between the reads (core-safe, unlike the latching TIMELR/TIMEHR pair). Overrides
// the arch's weak DWT default: TIMER0 is a true 64-bit source (no 32-bit wrap).
uint64_t arch_clock_now(void)
{
    uint32_t hi = r32(TIMER0_TIMERAWH);
    uint32_t lo;
    while (true)
    {
        lo = r32(TIMER0_TIMERAWL);
        uint32_t hi2 = r32(TIMER0_TIMERAWH);
        if (hi2 == hi)
        {
            break;
        }
        hi = hi2;
    }
    return ((static_cast<uint64_t>(hi) << 32) | lo) * 1000ull;
}

// Telemetry trace clock: the low 32 bits of the free-running 1 MHz system TIMER0
// (us, wraps ~71 min). Same source as arch_clock_now (a single RAW-low read), so the
// SESSION-anchor rate is exactly 1000 ns/tick.
uint32_t arch_trace_now(void)
{
    return r32(TIMER0_TIMERAWL);
}

void arch_shutdown(int status)
{
    (void)status; // no exit on bare metal
    __asm volatile("cpsid i" ::: "memory");
    while (true)
    {
        __asm volatile("wfi");
    }
}

#if KICKOS_HAVE_MPU
// Rule 7 reserved set (RP2350 datasheet). Owns-for-life: the 64-bit TIMER0 (monotonic
// base), the TICKS block (its TIMER0 generator is the 1 MHz source -- the RP2040
// watchdog role moved here), and the RESETS + CLOCKS control blocks. Full 16 KB
// windows each so the SET/CLR/XOR atomic aliases are covered. M33 (Arm) has no
// bit-band -> weak arch_bitband_present 0.
size_t arch_reserved_blocks(struct arch_reserved_block* out, size_t max)
{
    static struct arch_reserved_block const blocks[] = {
        {0x400B0000u, 0x4000u}, // TIMER0: 64-bit us monotonic (DS 12.8)
        {0x40108000u, 0x4000u}, // TICKS: TIMER0 tick generator, 1 MHz source (DS 8.5)
        {0x40020000u, 0x4000u}, // RESETS: peripheral reset control (DS 7.5)
        {0x40010000u, 0x4000u}, // CLOCKS: clock generators (DS 8.1)
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

void Reset_Handler(void)
{
    // The bootrom sets Secure VTOR before entry (datasheet 5.2.2), but pin it
    // explicitly to the image base for robustness (a warm reboot / debugger entry
    // may not have re-run the bootrom path). SCB->VTOR = 0xE000ED08.
    r32(0xE000ED08) = 0x10000000u;

    // Enable the FPU (CP10/CP11 full access) before any code a hard-float ABI might
    // emit FP into -- Cortex-M33 has an FPv5-SP FPU. SCB->CPACR = 0xE000ED88.
    r32(0xE000ED88) |= (0xFu << 20);
    __asm volatile("dsb" ::: "memory");
    __asm volatile("isb" ::: "memory");

    kickos_ranges_init(); // init .data; zero .bss
    for (void (**fn)() = __init_array_start; fn != __init_array_end; fn++)
    {
        (*fn)();
    }
    arch_init();
    kickos::kmain(0, nullptr);
    arch_shutdown(0);
}
}
