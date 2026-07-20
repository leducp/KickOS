// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// MK64FN1M0 (FRDM-K64F) chip backend. Register addresses/fields are from the K64
// Sub-Family Reference Manual (K64P144M120SF5RM); hand-rolled (no vendor CMSIS
// pack), consistent with the arch layer's clean-room regs.h.
//
// M1 scope: privilege + SVC only, no hardware MPU. clock_init() brings the core
// from the MCG FEI reset clock (~20.97 MHz) up to 120 MHz via the PLL off the
// FRDM's 50 MHz external clock (FEI->FBE->PBE->PEE), falling back to FEI if that
// clock is absent. Drives the OpenSDA virtual UART (UART0 on PTB16/PTB17); the
// baud tracks whichever clock won. Console is polled TX. NOT run in this
// environment (no board/QEMU model); verified by build + image inspection.
// Flash to the board to confirm. Silicon-risk points to check against the K64
// RM if bring-up misbehaves: the 50 MHz source is an external CLOCK (EREFS0=0,
// RANGE0=2), not a crystal; PRDIV/VDIV encodings; and the FRDIV /1536 mapping.

#include <kickos/arch/arch.h>
#include <kickos/console_tx.h>

#include <kickos/sys/abi.h> // kos_pstate_t / KOS_PSTATE_* (clock-select)

#include <stdint.h>

namespace kickos
{
    int kmain(int argc, char** argv);
    void kprintf(char const* fmt, ...);
}

extern "C"
{
    void kickos_armv7m_init(void);

    extern void (*__init_array_start[])();
    extern void (*__init_array_end[])();

    // FEI reset clock (MCGOUTCLK = 32.768 kHz internal ref x 640 FLL). This is the
    // initial + fallback value; clock_init() raises it to 120 MHz on success.
    // UART0 and SysTick are clocked by this system clock.
    uint32_t SystemCoreClock = 20971520u;
}

namespace
{
    inline volatile uint32_t& r32(uintptr_t a) { return *reinterpret_cast<volatile uint32_t*>(a); }
    inline volatile uint16_t& r16(uintptr_t a) { return *reinterpret_cast<volatile uint16_t*>(a); }
    inline volatile uint8_t& r8(uintptr_t a) { return *reinterpret_cast<volatile uint8_t*>(a); }

    // --- Peripheral registers (refman memory map) ---
    constexpr uintptr_t WDOG_STCTRLH = 0x40052000; // 16-bit
    constexpr uintptr_t WDOG_UNLOCK = 0x4005200E;  // 16-bit
    constexpr uint16_t WDOG_UNLOCK_1 = 0xC520;
    constexpr uint16_t WDOG_UNLOCK_2 = 0xD928;

    constexpr uintptr_t SIM_SCGC4 = 0x40048034; // UART0 = bit 10
    constexpr uintptr_t SIM_SCGC5 = 0x40048038; // PORTB = bit 10
    constexpr uintptr_t SIM_CLKDIV1 = 0x40048044;
    constexpr uint32_t SCGC4_UART0 = 1u << 10;
    constexpr uint32_t SCGC5_PORTB = 1u << 10;

    // --- Clock (MCG + OSC0 + SIM_CLKDIV1), K64 RM chapters 24/25/26 ---
    constexpr uintptr_t OSC0_CR = 0x40065000; // 8-bit
    constexpr uintptr_t MCG_C1 = 0x40064000;  // 8-bit each
    constexpr uintptr_t MCG_C2 = 0x40064001;
    constexpr uintptr_t MCG_C5 = 0x40064004;
    constexpr uintptr_t MCG_C6 = 0x40064005;
    constexpr uintptr_t MCG_S = 0x40064006;

    // Bus clock = core / BUS_DIV. Used by BOTH the OUTDIV2 field below AND the PIT
    // clock rate in arch_clock_now, so retuning the divider cannot silently rescale
    // kernel time.
    constexpr uint32_t BUS_DIV = 2;
    // SIM_CLKDIV1: core /1 (120), bus /2 (60), FlexBus /2 (60), flash /5 (24 MHz
    // -- FLASHCLK must stay <= 25 MHz). Field = divide-1, in nibbles [31:16].
    constexpr uint32_t CLKDIV1_120MHZ =
        (0u << 28) | ((BUS_DIV - 1u) << 24) | (1u << 20) | (4u << 16);

    constexpr uint8_t OSC0_CR_ERCLKEN = 1u << 7;
    constexpr uint8_t C2_RANGE_VHF = 2u << 4; // RANGE0=2; EREFS0=0 => external clock (not xtal)
    constexpr uint8_t C1_CLKS_EXT = 2u << 6;  // CLKS=2 external reference
    constexpr uint8_t C1_CLKS_PLL = 0u << 6;  // CLKS=0 FLL/PLL output (PLL, since PLLS=1)
    constexpr uint8_t C1_IREFS_INT = 1u << 2; // IREFS=1 internal ref (FEI posture, CLKS=0)
    constexpr uint8_t C1_FRDIV_1536 = 7u << 3; // RANGE!=0: /1536 -> 50 MHz FLL ref = 32.6 kHz
    constexpr uint8_t C5_PRDIV_20 = 19u;       // (PRDIV0+1)=20 -> PLL ref 50/20 = 2.5 MHz
    constexpr uint8_t C6_PLLS = 1u << 6;
    constexpr uint8_t C6_VDIV_48 = 24u; // (VDIV0+24)=48 -> VCO 2.5*48 = 120 MHz

    constexpr uint8_t S_IREFST = 1u << 4;    // 0 = external ref selected
    constexpr uint8_t S_CLKST_MASK = 3u << 2;
    constexpr uint8_t S_CLKST_EXT = 2u << 2; // MCGOUTCLK = external ref
    constexpr uint8_t S_CLKST_PLL = 3u << 2; // MCGOUTCLK = PLL
    constexpr uint8_t S_PLLST = 1u << 5;     // PLL (not FLL) is the PLLS source
    constexpr uint8_t S_LOCK0 = 1u << 6;

    // Bounded like the xmc clock_wait / rp2040 wait_mask: a missing external clock
    // degrades (returns false -> FEI fallback) instead of hanging the boot.
    constexpr uint32_t MCG_POLL_TIMEOUT = 1000000u;

    // OpenSDA VCOM is PTB16/PTB17. Per the K64 signal-mux table these pins are
    // UART0_RX/UART0_TX at ALT3 (PTB16 has no UART1 option) -- the FRDM-K64F user
    // guide's "UART1" label is a doc typo; UART0 is what the silicon exposes.
    constexpr uintptr_t PORTB_PCR16 = 0x4004A040; // UART0_RX (ALT3)
    constexpr uintptr_t PORTB_PCR17 = 0x4004A044; // UART0_TX (ALT3)
    constexpr uint32_t PCR_MUX_ALT3 = 3u << 8;

    // Kernel diagnostic LED: FRDM-K64F onboard RGB, RED = PTB22, ACTIVE-LOW (pin
    // low = lit). PORTB is already clocked by uart0_init (SCGC5 bit 10); the LED
    // init re-enables it so it stands alone. GPIO module offsets are K64 RM ch.55:
    // PSOR 0x04 (set -> high), PCOR 0x08 (clear -> low), PDDR 0x14 (1 = output).
    constexpr uintptr_t PORTB_PCR22 = 0x4004A058; // PCRn = base + n*4 (PTB22)
    constexpr uint32_t PCR_MUX_GPIO = 1u << 8;    // MUX[10:8]=001 = GPIO (ALT1)
    constexpr uintptr_t GPIOB_PSOR = 0x400FF044;
    constexpr uintptr_t GPIOB_PCOR = 0x400FF048;
    constexpr uintptr_t GPIOB_PDDR = 0x400FF054;
    constexpr uint32_t LED_RED_BIT = 1u << 22;

    constexpr uintptr_t UART0_BASE = 0x4006A000;
    constexpr uintptr_t UART0_BDH = UART0_BASE + 0x00; // 8-bit
    constexpr uintptr_t UART0_BDL = UART0_BASE + 0x01;
    constexpr uintptr_t UART0_C1 = UART0_BASE + 0x02;
    constexpr uintptr_t UART0_C2 = UART0_BASE + 0x03;
    constexpr uintptr_t UART0_S1 = UART0_BASE + 0x04;
    constexpr uintptr_t UART0_S2 = UART0_BASE + 0x05;
    constexpr uintptr_t UART0_C3 = UART0_BASE + 0x06;
    constexpr uintptr_t UART0_D = UART0_BASE + 0x07;
    constexpr uintptr_t UART0_C4 = UART0_BASE + 0x0A;
    constexpr uintptr_t UART0_C5 = UART0_BASE + 0x0B;
    constexpr uintptr_t UART0_MODEM = UART0_BASE + 0x0D;
    constexpr uintptr_t UART0_IR = UART0_BASE + 0x0E;
    constexpr uintptr_t UART0_PFIFO = UART0_BASE + 0x10;
    constexpr uintptr_t UART0_CFIFO = UART0_BASE + 0x11;
    constexpr uintptr_t UART0_C7816 = UART0_BASE + 0x18;
    constexpr uint8_t C2_TE = 1u << 3;
    constexpr uint8_t C2_RE = 1u << 2;
    constexpr uint8_t C2_TIE = 1u << 7; // transmit-interrupt enable: IRQ while S1.TDRE
    constexpr uint8_t S1_TDRE = 1u << 7;
    constexpr uint8_t S1_TC = 1u << 6;  // transmit complete: shifter idle (not just TDRE)
    // CFIFO command bits (K64 RM 60.3.9): write 1 to flush the TX / RX FIFO buffer.
    constexpr uint8_t CFIFO_RXFLUSH = 1u << 6;
    constexpr uint8_t CFIFO_TXFLUSH = 1u << 7;
    // MODEM.TXCTSE (K64 RM 60.3.13): CTS hardware flow control -- if a driver sets it,
    // the polled writer waits forever on an absent CTS and drops every byte (SILENT
    // LOSS). Named for the scramble test; reclaim clears the whole register (MODEM=0).
    constexpr uint8_t MODEM_TXCTSE = 1u << 0;

    // NVIC: UART0 status sources (RX/TX combined) = IRQ 31 (UART0 error = 32).
    // Confirm against the K64 RM interrupt-vector-assignments table.
    constexpr int UART0_RXTX_IRQ = 31;

    // --- PIT: the monotonic time base (K64 RM ch.44) ----------------------------
    // The v7-M default clock is the DWT cycle counter, but on this part DWT_CYCCNT
    // reads are unreliable (it lives in the core debug power domain and returns
    // garbage -- observed 0x40000001 == DWT_CTRL -- intermittently), which the
    // software wrap-extension turns into a phantom 2^32 clock jump and strands every
    // timed wait. The PIT is a plain peripheral on the bus clock: chain two 32-bit
    // channels into a free-running 64-bit down-counter and use it as arch_clock_now.
    // ONLY the monotonic clock moves off DWT; arch_trace_now + the KICKOS_BENCH
    // switch.S timestamps intentionally stay on raw DWT_CYCCNT (an intermittent glitch
    // there skews a telemetry/bench sample -- tolerable -- but is fatal to the
    // scheduler's monotonic clock, which is why only this one source moved).
    // CEILING: the kernel time base (ch0/ch1) and PIT_MCR share ONE AIPS peripheral
    // slot. A userspace PIT driver that opens that slot to U-mode (k64drv clears
    // PACR55.SP) thereby reaches ch0/ch1 and MCR -- a rogue MCR=MDIS write would
    // freeze the kernel clock. This is the accepted K64F coarse-peripheral (per-AIPS-
    // slot) protection ceiling; see reference/architecture.md.
    constexpr uintptr_t SIM_SCGC6 = 0x4004803C;
    constexpr uint32_t SCGC6_PIT = 1u << 23;
    constexpr uintptr_t PIT_MCR = 0x40037000;
    constexpr uintptr_t PIT_LDVAL0 = 0x40037100;
    constexpr uintptr_t PIT_CVAL0 = 0x40037104;
    constexpr uintptr_t PIT_TCTRL0 = 0x40037108;
    constexpr uintptr_t PIT_LDVAL1 = 0x40037110;
    constexpr uintptr_t PIT_CVAL1 = 0x40037114;
    constexpr uintptr_t PIT_TCTRL1 = 0x40037118;
    constexpr uint32_t PIT_TCTRL_TEN = 1u << 0; // timer enable
    constexpr uint32_t PIT_TCTRL_CHN = 1u << 2; // chain to the previous channel

    void pit_clock_init()
    {
        // Boot-order constraint: arch_clock_now MUST NOT run before this. The PIT is
        // clock-gated out of reset and an ungated AIPS read BusFaults; ktime/clock
        // reads only start after arch_init calls this.
        r32(SIM_SCGC6) |= SCGC6_PIT; // clock the PIT module
        r32(PIT_MCR) = 0;            // MDIS=0 (enable), FRZ=0
        // Free-running: both channels reload from all-ones; ch1 (MSW) decrements
        // when ch0 (LSW) rolls under. Program reloads, chain ch1 to ch0, then
        // enable ch0 last so the 64-bit counter starts coherently.
        r32(PIT_LDVAL0) = 0xFFFFFFFFu;
        r32(PIT_LDVAL1) = 0xFFFFFFFFu;
        r32(PIT_TCTRL1) = PIT_TCTRL_CHN | PIT_TCTRL_TEN;
        r32(PIT_TCTRL0) = PIT_TCTRL_TEN;
    }

    // Elapsed PIT ticks since start (the counter runs DOWN from all-ones). Read the
    // MSW LAST and retry until it is stable across the LSW read: the final read is
    // the one that validates the pair, so a preemption straddling a ch0 roll-under
    // cannot return a torn (backward-stepped) value. Concurrency-safe with no IRQ
    // save -- every load re-reads hardware, nothing is cached across the window.
    uint64_t pit_ticks()
    {
        uint32_t hi;
        uint32_t lo;
        do
        {
            hi = r32(PIT_CVAL1);
            lo = r32(PIT_CVAL0);
        } while (r32(PIT_CVAL1) != hi);
        uint64_t down = (static_cast<uint64_t>(hi) << 32) | lo;
        return ~down; // 0xFFFF... - down == elapsed
    }

    // --- arch_clock_now epoch anchor + clock-select re-anchor (B2/S2) -----------
    // ns = base_ns + (raw_ticks - base_ticks)*mult, mult = reciprocal of the PIT (bus)
    // clock. The SOLE writer of `mult` is clock_anchor_init (boot) and arch_cpu_clock_set
    // (the re-anchor at the rate edge); arch_clock_now only READS. WHY sole-writer: the
    // old lazy `if (hz != cached_hz) recompute` inside arch_clock_now, if it survived,
    // would let a now() called between the SystemCoreClock write and the re-anchor
    // recompute mult against the new bus clock and bake the phantom jump into base_ns.
    uint64_t g_clk_base_ns = 0;
    uint64_t g_clk_base_ticks = 0;
    uint64_t g_clk_mult = 0;

    uint64_t clock_recip(uint32_t hz)
    {
        return ((static_cast<uint64_t>(1000000000ull) << 32) + (hz >> 1)) / hz;
    }

    uint64_t clock_ns_from(uint64_t ticks)
    {
        uint64_t delta = ticks - g_clk_base_ticks;
        uint64_t a = delta >> 32, b = delta & 0xFFFFFFFFull;
        uint64_t c = g_clk_mult >> 32, d = g_clk_mult & 0xFFFFFFFFull;
        return g_clk_base_ns + ((a * c) << 32) + a * d + b * c + ((b * d) >> 32);
    }

    void clock_anchor_init()
    {
        uint32_t const hz = SystemCoreClock / BUS_DIV; // PIT is clocked by the bus clock
        if (hz == 0)
        {
            return;
        }
        g_clk_mult = clock_recip(hz);
        g_clk_base_ticks = 0; // BOOT-IDENTICAL: now = raw_ticks*mult (the old first read)
        g_clk_base_ns = 0;
    }

    void wdog_disable()
    {
        // WDOG resets the part ~238 ms after reset if left enabled, so this runs
        // first (RM 24.3.2: the unlock must also complete within 256 bus cycles
        // of reset). The two unlock keys must land within 20 bus cycles of each
        // other (RM 24.3.1) -- emit both stores back-to-back in ONE asm block so
        // an unoptimized (-O0) build cannot insert a helper call between them
        // (a non-inlined store helper would blow the 20-cycle budget).
        volatile uint16_t* unlock = reinterpret_cast<volatile uint16_t*>(WDOG_UNLOCK);
        uint32_t k1 = WDOG_UNLOCK_1;
        uint32_t k2 = WDOG_UNLOCK_2;
        __asm volatile("strh %1, [%0]\n\t"
                       "strh %2, [%0]"
                       ::"r"(unlock), "r"(k1), "r"(k2) : "memory");
        // STCTRLH := reset value 0x01D3 with WDOGEN cleared, keeping ALLOWUPDATE
        // and the reset-1 reserved bit 8 (matches NXP SystemInit; 0x0010 would
        // clear that reserved bit -- pointless risk on never-run silicon).
        r16(WDOG_STCTRLH) = 0x01D2;
    }

    void enable_fpu()
    {
        volatile uint32_t* cpacr = reinterpret_cast<volatile uint32_t*>(0xE000ED88);
        *cpacr |= (0xFu << 20); // CP10/CP11 full access
        __asm volatile("dsb" ::: "memory");
        __asm volatile("isb" ::: "memory");
    }

    bool mcg_wait(uint8_t mask, uint8_t want)
    {
        for (uint32_t i = 0; i < MCG_POLL_TIMEOUT; i++)
        {
            if ((r8(MCG_S) & mask) == want)
            {
                return true;
            }
        }
        return false;
    }

    // Undo any external/PLL commit and return the MCG to the FEI reset posture that
    // SystemCoreClock still reflects, then return false. Without this a partial
    // bring-up (e.g. the EXT mux switched but PLL LOCK timed out) would leave the
    // core running at 50/120 MHz while all software believes 20.97 MHz -- and would
    // leave CLKS=EXT armed so a late-arriving reference completes the switch mid-run.
    bool fail_to_fei()
    {
        r8(MCG_C6) = 0;             // clear PLLS + VDIV
        r8(MCG_C5) = 0;             // clear PRDIV
        r8(MCG_C1) = C1_IREFS_INT;  // CLKS=0 (FLL output) + internal ref -> FEI
        mcg_wait(S_IREFST, S_IREFST); // best-effort: internal ref reselected
        mcg_wait(S_CLKST_MASK, 0);    // CLKST=0 (FEI/FLL output)
        return false;
    }

    // FEI -> FBE -> PBE -> PEE off the FRDM's 50 MHz external clock. On any failure
    // after the external switch is requested, restores the FEI posture (fail_to_fei)
    // so SystemCoreClock stays the truth. Does NOT touch SIM_CLKDIV1 or SystemCoreClock
    // -- the caller widens the dividers before the rise and records the landed Hz, so
    // this walk is reused by boot (clock_init) and the runtime up-retune (cpu_clock_set).
    bool mcg_to_pee()
    {
        // 50 MHz external clock into EXTAL0 (EREFS0=0 = bypass the crystal osc).
        r8(OSC0_CR) = OSC0_CR_ERCLKEN;
        r8(MCG_C2) = C2_RANGE_VHF;

        // FEI -> FBE: take the external reference; /1536 keeps the (PEE-unused) FLL
        // input inside its 31.25-39.0625 kHz window.
        r8(MCG_C1) = C1_CLKS_EXT | C1_FRDIV_1536;
        if (not mcg_wait(S_IREFST, 0))
        {
            return fail_to_fei();
        }
        if (not mcg_wait(S_CLKST_MASK, S_CLKST_EXT))
        {
            return fail_to_fei();
        }

        // FBE -> PBE: PLL ref 2.5 MHz, VCO 120 MHz. Wait for PLL-selected + lock.
        r8(MCG_C5) = C5_PRDIV_20;
        r8(MCG_C6) = C6_PLLS | C6_VDIV_48;
        if (not mcg_wait(S_PLLST, S_PLLST))
        {
            return fail_to_fei();
        }
        if (not mcg_wait(S_LOCK0, S_LOCK0))
        {
            return fail_to_fei();
        }

        // PBE -> PEE: select the PLL output as MCGOUTCLK.
        r8(MCG_C1) = C1_CLKS_PLL | C1_FRDIV_1536;
        if (not mcg_wait(S_CLKST_MASK, S_CLKST_PLL))
        {
            return fail_to_fei();
        }
        return true;
    }

    bool clock_init()
    {
        // Set the bus dividers BEFORE the core scales up (RM 26: widen dividers
        // first, else bus/flash overrun when MCGOUTCLK jumps to 120 MHz). Safe now
        // because we are still on the ~20.97 MHz FEI clock.
        r32(SIM_CLKDIV1) = CLKDIV1_120MHZ;
        if (mcg_to_pee())
        {
            SystemCoreClock = 120000000u;
            return true;
        }
        return false; // fail_to_fei parked us on FEI; SystemCoreClock stays 20.97 MHz
    }

    void uart0_init()
    {
        r32(SIM_SCGC5) |= SCGC5_PORTB; // clock PORTB
        r32(SIM_SCGC4) |= SCGC4_UART0; // clock UART0
        r32(PORTB_PCR16) = PCR_MUX_ALT3;
        r32(PORTB_PCR17) = PCR_MUX_ALT3;

        r8(UART0_C2) = 0; // disable TX/RX while configuring
        // baud = clk / (16 x (SBR + BRFA/32)); UART0 is system-clocked, so derive
        // SBR + the 1/32 fine-adjust from the live clock (tracks 120 MHz or the
        // 20.97 MHz FEI fallback). 20.97 MHz -> SBR 11/BRFA 12; 120 MHz -> 65/3.
        uint32_t const baud = 115200u;
        uint32_t sbr = SystemCoreClock / (16u * baud);
        uint32_t brfa = (SystemCoreClock * 2u) / baud - sbr * 32u;
        r8(UART0_BDH) = static_cast<uint8_t>((sbr >> 8) & 0x1F);
        r8(UART0_BDL) = static_cast<uint8_t>(sbr & 0xFF);
        r8(UART0_C4) = static_cast<uint8_t>(brfa & 0x1F); // BRFA fine-adjust (low 5 bits)
        r8(UART0_C2) = C2_TE | C2_RE; // TIE stays clear; the console ring primes it
    }

    // --- Buffered console TX backend (console_tx.h). The ring drains via the
    // UART0 TX-empty interrupt; slot_free/push touch one data register. ---
    int k64_tx_slot_free(void) { return (r8(UART0_S1) & S1_TDRE) != 0; }
    void k64_tx_push(uint8_t b) { r8(UART0_D) = b; }
    void k64_tx_irq_enable(void) { r8(UART0_C2) = static_cast<uint8_t>(r8(UART0_C2) | C2_TIE); }
    void k64_tx_irq_disable(void) { r8(UART0_C2) = static_cast<uint8_t>(r8(UART0_C2) & ~C2_TIE); }

    constexpr uint32_t CONSOLE_TX_SIZE = 512; // power of two; > kprintf's 256B buffer
    char console_tx_buf[CONSOLE_TX_SIZE];
    console_tx_backend const k64_console_backend = {
        k64_tx_slot_free, k64_tx_push, k64_tx_irq_enable, k64_tx_irq_disable};

    // --- SYSMPU (K64 RM section 19); base 0x4000_D000 -------------------------
    // NXP's byte/32-granular bus-master protection -- NOT the ARM core MPU
    // (__MPU_PRESENT=0 here), so K64F overrides the weak ARM PMSA arch_mpu_apply.
    // The Cortex-M4 core is TWO crossbar masters (RM 3.3.6.1): M0 = code bus
    // (instruction fetch + flash literal/rodata reads), M1 = system bus (SRAM +
    // peripheral data). RGD0 is the supervisor background; RGD1..11 are per-thread
    // USER grants. An access is allowed if ANY valid descriptor grants it (union),
    // so RGD0 (supervisor rwx everywhere) always covers privileged code.
    constexpr uintptr_t SYSMPU_CESR = 0x4000D000;
    constexpr uintptr_t SYSMPU_RGD = 0x4000D400;    // RGDn word k = RGD + n*0x10 + k*4
    constexpr uintptr_t SYSMPU_RGDAAC0 = 0x4000D800; // WORD2 alt view (keeps VLD)
    constexpr uint32_t SYSMPU_CESR_VLD = 1u << 0;    // global MPU enable
    constexpr size_t SYSMPU_RGD_COUNT = 12;
    // Error capture (K64 RM 19.3): EARn 0xD010+n*8, EDRn 0xD014+n*8, 5 slave ports.
    // CESR[31:27] SPERR: bit 31 -> slave port 0 ... bit 27 -> slave port 4.
    constexpr uintptr_t SYSMPU_EAR0 = 0x4000D010; // EARn = EAR0 + n*8
    constexpr uintptr_t SYSMPU_EDR0 = 0x4000D014; // EDRn = EDR0 + n*8
    constexpr size_t SYSMPU_SLAVE_PORTS = 5;

#if KICKOS_HAVE_MPU
    // WORD2 for the core's two crossbar masters (attr = the UNPRIVILEGED rights).
    // The Cortex-M4 core reaches memory as M0 (code bus) OR M1 (system bus), chosen
    // by ADDRESS: M0 serves flash AND SRAM_L (both < 0x2000_0000), M1 serves SRAM_U
    // + peripherals. A thread's stack/data can sit in EITHER SRAM bank (this chip's
    // RAM pool starts in SRAM_L @ 0x1FFF_0000), and an exception (un)stack to a
    // SRAM_L stack is an M0 data access -- so granting data only on M1 denies it.
    // Grant the rights on BOTH masters: the RGD is address-bounded, so this widens
    // only the bus a thread may use, not the range it reaches. M0UM[2:0] @ bits 2:0
    // (r=bit2,w=bit1,x=bit0); M1UM[2:0] @ bits 8:6 (r=8,w=7,x=6). Supervisor SM left
    // 0 (=r/w/x) -> RGD0 background covers privileged; execute stays code-bus only.
    uint32_t sysmpu_word2(uint32_t attr)
    {
        uint32_t w = (1u << 2) | (1u << 8); // read: M0 + M1
        if (attr & ARCH_MPU_W)
        {
            w |= (1u << 1) | (1u << 7); // write: M0 + M1
        }
        if (attr & ARCH_MPU_X)
        {
            w |= (1u << 0); // execute: code bus (M0) only
        }
        return w;
    }
#endif
}

extern "C"
{

void arch_init(void)
{
    // FPU is enabled earlier (Reset_Handler, before C++ ctors). Raise the core to
    // 120 MHz BEFORE UART (baud) + SysTick are programmed; on failure both fall
    // back cleanly to the 20.97 MHz FEI clock (SystemCoreClock unchanged).
    clock_init();
    pit_clock_init();    // monotonic time base (see pit_clock_init note; replaces DWT)
    clock_anchor_init(); // set the arch_clock_now mult ONCE from the final clock (B2)
    uart0_init();
    kickos_armv7m_init();
}

// Monotonic clock override: convert free-running PIT ticks (bus clock = core/2) to
// ns, replacing the weak DWT-backed arch_clock_now (unreliable on this silicon).
// ns = ticks * 1e9 / pit_hz via a cached reciprocal multiply (mult = (1e9<<32)/hz,
// ns = (ticks*mult)>>32, split 64x64 to avoid overflow) -- the one 64-bit divide
// runs only when the clock changes at boot.
uint64_t arch_clock_now(void)
{
    // Pure epoch read (B2): the mult is written ONLY by clock_anchor_init (boot) and the
    // arch_cpu_clock_set re-anchor -- never recomputed here -- so a read in the window
    // around a retune can never bake the phantom rate jump into the anchor.
    return clock_ns_from(pit_ticks());
}

// Clock-select MECHANISM (arch.h): a staged fixed set. MAX/MID land on PEE (120 MHz);
// LOW parks on the FEI internal clock (~20.97 MHz). Returns the LANDED core Hz -- a
// failed relock (fail_to_fei) still MOVED the clock and returns the truthful ~20.97 MHz
// (never 0), so cpu_clock_set runs the coherence tail (B1). The generic tail re-derives
// the baud + re-arms SysTick. Called privileged, IRQs already masked. PIT is bus =
// core/BUS_DIV, so the monotonic clock's rate moves and MUST be re-anchored here.
uint32_t arch_cpu_clock_set(uint32_t target)
{
    uint32_t const previous = SystemCoreClock;
    // Achievable set is {120 MHz PEE, 20.97 MHz FEI}; MID rounds UP to MAX (no distinct
    // mid point on this staged chip). The truthful landed Hz is the return value.
    uint32_t want = 120000000u;
    if (static_cast<kos_pstate_t>(target) == KOS_PSTATE_LOW)
    {
        want = 20971520u;
    }
    if (want == previous)
    {
        return previous;
    }

    // Re-anchor capture AT the edge: history priced at the OLD mult before it moves.
    uint64_t const t0 = pit_ticks();
    uint64_t const ns0 = clock_ns_from(t0);

    uint32_t landed;
    if (want > previous)
    {
        // RISE (FEI -> PEE): widen the bus/flash dividers BEFORE MCGOUTCLK climbs (RM
        // 26; the flash divider /5 keeps FLASHCLK <= 25 MHz) -- the flash-WS-before-rise
        // discipline for this part. Then walk FEI->FBE->PBE->PEE.
        r32(SIM_CLKDIV1) = CLKDIV1_120MHZ;
        if (mcg_to_pee())
        {
            landed = 120000000u;
        }
        else
        {
            landed = 20971520u; // relock failed -> parked on FEI (B1: truthful, tail runs)
        }
    }
    else
    {
        // FALL (PEE -> FEI): drop the MCG first (fail_to_fei walks it back to the FEI
        // reset posture). The /5 flash divider is safe at any lower core clock, so no
        // post-fall divider relax is needed (leaving it wider never faults).
        fail_to_fei();
        landed = 20971520u;
    }

    SystemCoreClock = landed;

    // Commit the NEW pricing -- the SOLE writer of mult (B2). base_ns holds history at
    // old pricing; the staged MCG walk (the worst-case masked span, ~1 ms) is the only
    // mispriced interval (frozen skew), bounded by folding the anchor to the edge.
    uint32_t const bus = landed / BUS_DIV;
    g_clk_base_ns = ns0;
    g_clk_base_ticks = t0;
    g_clk_mult = clock_recip(bus);
    __asm volatile("" ::: "memory"); // pin the triple write order vs a later now() read
    return landed;
}

// Clock-select console coherence (arch.h). UART0 is system-clocked, so its baud moves
// with a clock-select and must be re-derived; and no byte may be mid-shift at the old
// baud when the clock moves. Both run under the caller's IrqLock (see cpu_clock_set).
void arch_console_flush_sync(void)
{
    // Wait for the shift register to fully empty (S1.TC), not merely the data register
    // (S1.TDRE): TDRE leaves one byte still clocking out. Bounded like the sync writer.
    uint32_t spin = 0;
    while ((r8(UART0_S1) & S1_TC) == 0)
    {
        if (++spin > 1000000u)
        {
            return; // a wedged UART must not hang the retune (matches the sync writer)
        }
    }
}

void arch_console_retune(void)
{
    // Re-derive SBR + the 1/32 fine-adjust from the landed SystemCoreClock (120 MHz or
    // the 20.97 MHz FEI point), as uart0_init does. The divisor write needs TX/RX
    // disabled; TIE stays clear (the console ring re-primes it on the next write).
    r8(UART0_C2) = 0;
    uint32_t const baud = 115200u;
    uint32_t const sbr = SystemCoreClock / (16u * baud);
    uint32_t const brfa = (SystemCoreClock * 2u) / baud - sbr * 32u;
    r8(UART0_BDH) = static_cast<uint8_t>((sbr >> 8) & 0x1F);
    r8(UART0_BDL) = static_cast<uint8_t>(sbr & 0xFF);
    r8(UART0_C4) = static_cast<uint8_t>(brfa & 0x1F);
    r8(UART0_C2) = C2_TE | C2_RE;
}

// SYSMPU backend: overrides the weak ARM PMSA arch_mpu_apply (K64F has no ARM
// core MPU). Reloads the running thread's per-thread USER grants (RGD1..) on every
// switch-in; supervisor + DMA stay covered by RGD0. Gated on KICKOS_HAVE_MPU.
void arch_mpu_apply(struct arch_mpu_region const* regions, size_t n)
{
#if KICKOS_HAVE_MPU
    // One-time: make RGD0 a supervisor-only background. At reset RGD0 grants ALL
    // masters rwx over all memory; strip the core's USER access on BOTH masters so
    // a U-mode access then needs a per-thread RGD, while supervisor (the privileged
    // kernel) keeps full rwx. RGDAAC0 is the WORD2 alt view (does not clear VLD).
    static bool rgd0_ready = false;
    if (not rgd0_ready)
    {
        // CRITICAL: RGD0 resets with the SUPERVISOR fields M0SM/M1SM = 0b11 = "same
        // as user mode" (K64 RM 19.6.1). Clearing only the user fields (M0UM/M1UM)
        // therefore drops SUPERVISOR access too -- it defers to the now-zero user
        // field -- and the privileged kernel faults on its very next instruction
        // fetch, double-faults while stacking, and the core locks up -> reset with
        // no dump (this was the task #12 symptom). So ALSO clear M0SM/M1SM to 0b00
        // (= supervisor r/w/x), pinning supervisor full-access independent of UM.
        // Bit fields (both core masters): M0UM[2:0] M0SM[4:3], M1UM[8:6] M1SM[10:9].
        constexpr uint32_t core_user_and_sm =
            (0x7u << 0) | (0x3u << 3) | (0x7u << 6) | (0x3u << 9);
        r32(SYSMPU_RGDAAC0) &= ~core_user_and_sm;
        r32(SYSMPU_CESR) |= SYSMPU_CESR_VLD; // (already enabled at reset)
        rgd0_ready = true;
    }
    // Program RGD1..(n) from the region set; invalidate the rest. RGD0 stays the
    // background. Writing WORD2 clears VLD, so set WORD0/1/2 then WORD3=VLD last.
    for (size_t i = 0; i + 1 < SYSMPU_RGD_COUNT; i++)
    {
        uintptr_t const rgd = SYSMPU_RGD + (i + 1) * 0x10;
        if (i < n and regions[i].size >= 32)
        {
            uintptr_t const base = regions[i].base;
            uintptr_t const end = base + regions[i].size - 1;
            r32(rgd + 0x0) = static_cast<uint32_t>(base);          // WORD0 SRTADDR[31:5]
            r32(rgd + 0x4) = static_cast<uint32_t>(end);           // WORD1 ENDADDR[31:5]
            r32(rgd + 0x8) = sysmpu_word2(regions[i].attr);        // WORD2 (clears VLD)
            r32(rgd + 0xC) = 1u;                                   // WORD3 VLD=1
        }
        else
        {
            r32(rgd + 0xC) = 0u; // invalidate the descriptor
        }
    }
    __asm volatile("dsb" ::: "memory");
    __asm volatile("isb" ::: "memory");
#else
    (void)regions;
    (void)n;
#endif
}

// SYSMPU is byte-granular on a 32-byte page (SRTADDR/ENDADDR are addr[31:5]); a
// window is exact iff base and base+size both land on a 32-byte boundary. Overrides
// the weak PMSA (pow2) encodability -- SYSMPU needs NO power-of-two size.
bool arch_mpu_region_encodable(uintptr_t base, size_t size)
{
    if (size < 32u)
    {
        return false;
    }
    return (base & 31u) == 0 and (size & 31u) == 0;
}

// Chip fault-decode hook (arch.h): a SYSMPU protection error reaches the core as a
// BUS error (escalates to HardFault; the CFSR MMFSR is 0, so the shared reporter
// cannot name it). Read the SYSMPU error capture and print the faulting address +
// master + access type for whichever slave port latched. IMPORTANT (K64 RM 3.3.6.2
// / 3.3.7.1): the MPU slave ports cover flash, SRAM_L/U and FlexBus ONLY -- the AIPS
// peripheral bridges and the GPIO controller are NOT MPU slave ports ("protection
// built into the bridge"), so a peripheral-window violation does NOT set SPERR here;
// that no-SPERR case is itself the diagnostic (peripheral MMIO is not SYSMPU-gated).
// Runs privileged (RGD0 full access), so it cannot itself fault.
void arch_fault_report_extra(void)
{
    uint32_t cesr = r32(SYSMPU_CESR);
    uint32_t sperr = cesr >> 27; // CESR[31:27]; bit 31 -> port 0
    if (sperr == 0)
    {
        kickos::kprintf("  SYSMPU: no protection error latched (CESR=0x%x) -- a bus "
                        "fault outside an MPU slave port (peripheral bridge?)\n", cesr);
        return;
    }
    for (size_t port = 0; port < SYSMPU_SLAVE_PORTS; port++)
    {
        if ((sperr & (1u << (4 - port))) == 0)
        {
            continue;
        }
        uint32_t ear = r32(SYSMPU_EAR0 + port * 8u);
        uint32_t edr = r32(SYSMPU_EDR0 + port * 8u);
        uint32_t master = (edr >> 4) & 0xFu;
        char const* rw = "R";
        if (edr & 1u)
        {
            rw = "W";
        }
        kickos::kprintf("  SYSMPU ISOLATION FAULT: port=%u addr=0x%x master=%u %s "
                        "EDR=0x%x\n", static_cast<unsigned>(port), ear, master, rw, edr);
        // W1C this port's SPERR, but PRESERVE VLD (bit 0, plain R/W): a bare
        // `= 1u<<(31-port)` writes VLD=0 and disables the whole SYSMPU.
        r32(SYSMPU_CESR) = (cesr & SYSMPU_CESR_VLD) | (1u << (31 - port));
    }
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
        while ((r8(UART0_S1) & S1_TDRE) == 0)
        {
            if (++spin > 1000000u)
            {
                return; // bounded: a wedged UART must not hang the panic path (drop)
            }
        }
        r8(UART0_D) = static_cast<uint8_t>(buf[i]);
    }
}

console_tx_backend const* arch_console_tx_backend(char** storage, uint32_t* size, int* irq_line)
{
    *storage = console_tx_buf;
    *size = CONSOLE_TX_SIZE;
    *irq_line = UART0_RXTX_IRQ;
    return &k64_console_backend;
}

// Panic-path reclaim (console.cc D6): force UART0 back to a known polled-ready 8N1
// TX channel after a userspace driver may have garbled EVERY writable register in its
// granted window. Runs with IRQs masked, privileged; MUST be idempotent + re-entrant,
// so it is straight-line ABSOLUTE stores only -- NO read-modify-write (an RMW on a
// garbled value is not safe to repeat from a nested-fault re-entry; note this drops
// the C2 |= TIE style used on the non-panic backend paths). Overrides the weak no-op
// in console.cc.
//
// Reclaim depth = rewrite what uart0_init sets (BDH/BDL/C4/C1/C2) PLUS the registers
// init leaves at reset default that a hostile driver can set to cause SILENT LOSS --
// each cleared to 0 below with the failure it prevents. The clock gates (SIM_SCGC4
// UART0 / SCGC5 PORTB) and pin mux (PORTB_PCR16/17) sit in SEPARATE privileged
// peripherals OUTSIDE the UART0 window -> unreachable by the driver -> intact -> not
// touched.
void arch_console_reclaim(void)
{
    r8(UART0_C2) = 0;    // disable TX/RX/TIE so the driver stops; also lets the config
                         // registers below (which require TE/RE clear) be rewritten
    r8(UART0_C5) = 0;    // TDMAS/RDMAS: disable the peripheral's own DMA request enable
    r8(UART0_MODEM) = 0; // TXCTSE=0: else the bounded polled writer waits forever on an
                         // absent CTS and drops every byte -- the true silent-loss case
    r8(UART0_C3) = 0;    // TXINV=0: an inverted TX line corrupts every framed byte
    r8(UART0_S2) = 0;    // clear any driver-set line-polarity / config status bits
    r8(UART0_IR) = 0;    // IREN=0: infrared modulation on the TX pin
    r8(UART0_C7816) = 0; // ISO-7816 smartcard framing off
    r8(UART0_PFIFO) = 0; // TX/RX FIFO disabled (single-datum mode, as at reset)
    r8(UART0_CFIFO) = CFIFO_TXFLUSH | CFIFO_RXFLUSH; // flush any queued FIFO contents

    // Re-derive baud from the live SystemCoreClock (as uart0_init): 120 MHz or the
    // 20.97 MHz FEI fallback. BDH before BDL (BDL write latches the divisor).
    uint32_t const baud = 115200u;
    uint32_t const sbr = SystemCoreClock / (16u * baud);
    uint32_t const brfa = (SystemCoreClock * 2u) / baud - sbr * 32u;
    r8(UART0_BDH) = static_cast<uint8_t>((sbr >> 8) & 0x1F);
    r8(UART0_BDL) = static_cast<uint8_t>(sbr & 0xFF);
    r8(UART0_C4) = static_cast<uint8_t>(brfa & 0x1F);

    r8(UART0_C1) = 0;       // 8N1, no loops/parity/wake
    r8(UART0_C2) = C2_TE;   // TX enable only (the polled banner needs no RX)
}

// Kernel diagnostic LED = FRDM-K64F onboard RED (PTB22), active-low.
void arch_diag_led_init(void)
{
    r32(SIM_SCGC5) |= SCGC5_PORTB; // clock PORTB (idempotent; uart0_init also sets it)
    r32(PORTB_PCR22) = PCR_MUX_GPIO;
    r32(GPIOB_PDDR) |= LED_RED_BIT; // PTB22 output
    r32(GPIOB_PSOR) = LED_RED_BIT;  // start OFF: drive high (active-low)
}

void arch_diag_led_set(int on)
{
    if (on != 0)
    {
        r32(GPIOB_PCOR) = LED_RED_BIT; // lit: drive low
    }
    else
    {
        r32(GPIOB_PSOR) = LED_RED_BIT; // off: drive high
    }
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

void Reset_Handler(void)
{
    wdog_disable(); // first: the watchdog would reset the part mid-bring-up
    enable_fpu();   // before ANY later code (the copy loops are integer, but a
                    // hard-float ABI could emit FP anywhere; CPACR-off FP faults)

    kickos_ranges_init(); // init .data + the pow2 app-data block; zero .bss + app-bss
    for (void (**fn)() = __init_array_start; fn != __init_array_end; fn++)
    {
        (*fn)();
    }
    arch_init();
    kickos::kmain(0, nullptr);
    arch_shutdown(0);
}

}
