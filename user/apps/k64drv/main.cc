// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// K64F GPIO/timer FIRST unprivileged userspace driver over the MMIO-grant seam
// (task #9 Stage 2). A privileged bring-up shim clocks + programs PIT channel 0 and
// opens the PIT peripheral slot to user mode; the UNPRIVILEGED driver thread is
// granted the PIT ch0 window (32 B @ 0x4003_7100, spanning ch0+ch1 at the 0x10 stride)
// + the PIT ch0 IRQ (tier-1). It W1C's its own timer flag with a direct unprivileged
// write to the peripheral, toggles the diag LED, heartbeats, then reads PIT_MCR at
// 0x4003_7000 -- OUTSIDE the SYSMPU window but in the same 4 KB slot.
//
// K64F peripheral privilege is gated by the AIPS peripheral bridge (PACR), NOT by
// SYSMPU (K64 RM 3.3.6.2 / 3.3.7.1: MPU slave ports cover flash / SRAM / FlexBus
// only; the AIPS bridges are not MPU slave ports, "protection built into bridge").
// User access is enabled per 4 KB slot by clearing the slot's PACR SP bit (PACRG
// for PIT slot 55; RM 20.2.3). So the SYSMPU MMIO grant is INERT for peripherals here
// (the write reaches PIT via the AIPS-opened slot, not the RGD), and AIPS granularity
// is the whole 4 KB slot: once open, EVERY unprivileged thread reaches it -- an MMIO
// grant is not a per-thread peripheral capability on K64F. The PIT_MCR read
// succeeding (same slot, outside the SYSMPU window) demonstrates that, not a failure.
//
// Diagnostic app (kickos_add_diagnostic_app): never a production image. Build-only;
// the operator flashes + validates.

#include <kickos/kos.h>
#include <kickos/sys.h>
#include <kickos/libc/fmt.h>

#include <stdint.h>

extern "C" uint32_t SystemCoreClock; // live core clock (chip); bus = core/2

namespace
{
    // K64 RM 41.3 (PIT) + 3.8/12.2.13 (SIM). Absolute addresses, no CMSIS pack.
    constexpr uintptr_t SIM_SCGC6 = 0x4004803Cu;  // 12.2.13/325
    constexpr uint32_t SCGC6_PIT = 1u << 23;      // PIT clock gate (bit 23)
    constexpr uintptr_t PIT_MCR = 0x40037000u;    // 41.3.1: MDIS=bit1, FRZ=bit0
    constexpr uintptr_t PIT_CH0 = 0x40037100u;    // ch0 window base (LDVAL/CVAL/TCTRL/TFLG)
    constexpr uintptr_t PIT_LDVAL0 = 0x40037100u; // 41.3.2
    constexpr uintptr_t PIT_TCTRL0 = 0x40037108u; // 41.3.4: CHN=b2, TIE=b1, TEN=b0
    constexpr uint32_t PIT_CH0_WINDOW = 32u;      // SYSMPU 32B-granular; 0x100 is 32-aligned
    // K64 RM Table 4-2 + 20.2.2/20.2.3: PIT @ 0x4003_7000 is AIPS0 slot 55 -> PACR55,
    // field 7 (bits [3:0]) of PACRG at 0x4000_0048. Nibble = reserved[3]/SP[2]/WP[1]/TP[0];
    // AIPS0_PACRG resets to 0x4444_4444 (3.3.8.4) so SP=1 => supervisor-only at reset.
    constexpr uintptr_t AIPS0_PACRG = 0x40000048u; // 20.2.3/456
    constexpr uint32_t PACR_PIT_SP = 1u << 2;      // PACR55 SP7 (supervisor-protect)
    constexpr uint32_t TFLG_OFFSET = 0x0Cu;        // TFLG0 = base + 0x0C (41.3.5, TIF=b0 w1c)
    constexpr uint32_t TCTRL_TEN = 1u << 0;
    constexpr uint32_t TCTRL_TIE = 1u << 1;
    constexpr int PIT0_IRQ = 48; // K64 RM Table 3-5: PIT ch0 = IRQ 48
    constexpr int DRIVER_TICKS = 10;

    inline volatile uint32_t& r32(uintptr_t a)
    {
        return *reinterpret_cast<volatile uint32_t*>(a);
    }

    // UNPRIVILEGED driver: granted app code+data (auto), the PIT ch0 window (via
    // the spawn MMIO grant) and the PIT IRQ (tier-1). It touches no file-scope
    // mutable state -- the window base arrives as the thread arg VALUE (never
    // dereferenced as memory), and the format buffer lives on its granted stack.
    void pit_driver(void* arg)
    {
        uintptr_t const win = reinterpret_cast<uintptr_t>(arg); // PIT ch0 window base
        volatile uint32_t* tflg0 = reinterpret_cast<volatile uint32_t*>(win + TFLG_OFFSET);

        int h = kos_irq_register(PIT0_IRQ);
        if (h < 0)
        {
            kos::print("[k64drv] ERROR: irq_register(PIT0) failed\n");
            while (true)
            {
                kos_sleep_ns(1000000000ull);
            }
        }

        // wait; service shape: kos_irq_wait auto-re-arms the consumed line on return,
        // so no explicit kernel ack. The peripheral W1C stays: it must clear the TIF
        // level BEFORE the next kos_irq_wait re-arms/unmasks, else the level re-fires.
        for (int tick = 0; tick < DRIVER_TICKS; tick++)
        {
            kos_irq_wait(h);
            *tflg0 = 1u; // W1C TIF: direct unprivileged write to the peripheral (reaches
                         // PIT via the AIPS-opened slot; the SYSMPU RGD is inert here).
                         // Clears the level so the line does not storm on the next re-arm.
            kos::kernel_diag_led_toggle();
            char s[48];
            ksnprintf(s, sizeof(s), "[k64drv] tick %d\n", tick + 1);
            kos::print(s);
        }

        // AIPS-granularity demo: read PIT_MCR at 0x4003_7000, outside the SYSMPU 32 B
        // window but in the same 4 KB AIPS slot the shim opened. Expected to succeed --
        // AIPS gates per 4 KB slot, not per SYSMPU window.
        kos::print("[k64drv] reading PIT_MCR (same AIPS slot, outside SYSMPU window)\n");
        uint32_t mcr = r32(PIT_MCR);
        char s[64];
        ksnprintf(s, sizeof(s), "[k64drv] AIPS slot open: MCR read OK (MCR=0x%x)\n",
                  static_cast<unsigned>(mcr));
        kos::print(s);

        // Daemon park (never exit: a non-last-thread exit is unsafe on this arch).
        while (true)
        {
            kos_sleep_ns(1000000000ull);
        }
    }
}

int main(int, char**)
{
    // Privileged bring-up (this main runs privileged): the one-time unsafe setup
    // the unprivileged driver must NOT be able to do -- clock-gate + module enable
    // + timer program. Then hand the driver ONLY the ch0 register window.
    r32(SIM_SCGC6) |= SCGC6_PIT;      // clock the PIT (also enables its AIPS slot)
    r32(AIPS0_PACRG) &= ~PACR_PIT_SP; // open PIT slot 55 to user mode (clear SP; RM 20.2.3)
    r32(PIT_MCR) = 0u;                // MDIS=0 (module on), FRZ=0

    // ~4 Hz on the live bus clock (= core/2, per SIM_CLKDIV1 in clock_init; holds on
    // the 120 MHz PLL and the ~20.97 MHz FEI fallback). LDVAL counts down from N-1.
    uint32_t bus_hz = SystemCoreClock / 2u;
    uint32_t ldval = bus_hz / 4u;
    if (ldval != 0u)
    {
        ldval -= 1u;
    }
    r32(PIT_LDVAL0) = ldval;
    r32(PIT_TCTRL0) = TCTRL_TEN | TCTRL_TIE; // TFLG untouched (reset 0); driver owns it

    int drv = kos::thread::spawn(pit_driver, reinterpret_cast<void*>(PIT_CH0), "k64drv", 10,
                                 KOS_POLICY_FIFO, 0, /*privileged=*/false,
                                 /*mem=*/nullptr, /*mem_size=*/0,
                                 /*stack=*/nullptr, /*stack_size=*/0,
                                 /*mmio=*/reinterpret_cast<void*>(PIT_CH0), PIT_CH0_WINDOW);
    if (drv < 0)
    {
        // Console is the only oracle at the bench: a silent dead board would be
        // indistinguishable from a bring-up failure, so say so.
        kos::print("[k64drv] ERROR: driver spawn failed\n");
    }

    // Park: the driver heartbeats, then demonstrates AIPS-per-slot granularity. Fall
    // back to a sleep park if the semaphore could not be created (else a -1 handle
    // spins a hot loop of failing sem_wait syscalls against the driver).
    int idle = kos_sem_create(0);
    while (true)
    {
        if (idle < 0)
        {
            kos_sleep_ns(1000000000ull);
            continue;
        }
        kos_sem_wait(idle);
    }
}
