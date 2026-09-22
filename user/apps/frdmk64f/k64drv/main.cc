// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// K64F PIT diagnostic using ch2 (kernel clock owns ch0/ch1). The worker gets
// a 32-byte window at 0x40037120 covering ch2/ch3 and a WAIT cap for ch2 IRQ.
// K64F AIPS, not SYSMPU, controls peripheral access (PIT slot 55, PACRG).
// The final PIT_MCR read is outside the window and is expected to succeed,
// demonstrating the lack of per-thread MMIO isolation. Validate on hardware.

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
    // ch0+ch1 are the kernel monotonic clock's 64-bit time base (arch_clock_now);
    // writing either from here breaks kernel time.
    constexpr uintptr_t PIT_CH2 = 0x40037120u;    // ch2 window base (LDVAL/CVAL/TCTRL/TFLG)
    constexpr uintptr_t PIT_LDVAL2 = 0x40037120u; // 41.3.2
    constexpr uintptr_t PIT_TCTRL2 = 0x40037128u; // 41.3.4: CHN=b2, TIE=b1, TEN=b0
    constexpr uint32_t PIT_CH2_WINDOW = 32u;      // SYSMPU 32B-granular; 0x120 is 32-aligned
    // K64 RM Table 4-2 + 20.2.2/20.2.3: PIT @ 0x4003_7000 is AIPS0 slot 55 -> PACR55,
    // field 7 (bits [3:0]) of PACRG at 0x4000_0048. Nibble = reserved[3]/SP[2]/WP[1]/TP[0];
    // AIPS0_PACRG resets to 0x4444_4444 (3.3.8.4) so SP=1 => supervisor-only at reset.
    constexpr uintptr_t AIPS0_PACRG = 0x40000048u; // 20.2.3/456
    constexpr uint32_t PACR_PIT_SP = 1u << 2;      // PACR55 SP7 (supervisor-protect)
    constexpr uint32_t TFLG_OFFSET = 0x0Cu;        // TFLG at +0x0C (41.3.5, TIF=b0 w1c)
    constexpr uint32_t TCTRL_TEN = 1u << 0;
    constexpr uint32_t TCTRL_TIE = 1u << 1;
    constexpr int PIT2_IRQ = 50; // K64 RM Table 3-5: PIT ch2 = IRQ 50
    constexpr int DRIVER_TICKS = 10;

    inline volatile uint32_t& r32(uintptr_t a)
    {
        return *reinterpret_cast<volatile uint32_t*>(a);
    }

    // Unprivileged, granted only app code+data, the PIT ch2 window and a WAIT-only cap
    // on the PIT line. It must touch no file-scope mutable state: the window base
    // arrives as the thread arg VALUE (never dereferenced as memory) and the format
    // buffer lives on its granted stack.
    void pit_driver(void* arg)
    {
        uintptr_t const win = reinterpret_cast<uintptr_t>(arg); // PIT ch2 window base
        volatile uint32_t* tflg2 = reinterpret_cast<volatile uint32_t*>(win + TFLG_OFFSET);

        // The line is attached to this object by root, on BIT 0; this thread only binds it.
        int const n = KOS_SPAWN_DELEGATED_CAP0 + 1; // claimed by root, delegated at spawn
        if (kos_notify_bind(n) != 0)
        {
            kos_panic("[k64drv] notify_bind refused the delegated object");
        }

        // The wait auto-re-arms the consumed line on return, so no explicit kernel ack. The
        // peripheral W1C must still clear the TIF level BEFORE the next wait re-arms the
        // line, else the level re-fires at once.
        for (int tick = 0; tick < DRIVER_TICKS; tick++)
        {
            (void)kos_notify_wait(n, 1u, KOS_TIMEOUT_NONE, nullptr);
            *tflg2 = 1u; // W1C TIF
            kos::kernel_diag_led_toggle();
            char s[48];
            ksnprintf(s, sizeof(s), "[k64drv] tick %d\n", tick + 1);
            kos::print(s);
        }

        // PIT_MCR is outside the granted SYSMPU 32 B window but inside the same 4 KB AIPS
        // slot, so this read is EXPECTED to succeed: AIPS gates per slot, not per window.
        kos::print("[k64drv] reading PIT_MCR (same AIPS slot, outside SYSMPU window)\n");
        uint32_t mcr = r32(PIT_MCR);
        char s[64];
        ksnprintf(s, sizeof(s), "[k64drv] AIPS slot open: MCR read OK (MCR=0x%x)\n",
                  static_cast<unsigned>(mcr));
        kos::print(s);

        while (true)
        {
            kos_sleep_ns(1000000000ull);
        }
    }
}

KICKOS_APP_AUTHORITY(KOS_AUTH_MEMORY | KOS_AUTH_SYSTEM | KOS_AUTH_IRQ);

int main(int, char**)
{
    // The kernel clock already clock-gated the PIT and enabled MCR at boot, so SCGC6/MCR
    // here are idempotent; nothing in this bring-up may disturb ch0/ch1 (the kernel time
    // base).
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
    r32(PIT_LDVAL2) = ldval;
    r32(PIT_TCTRL2) = TCTRL_TEN | TCTRL_TIE; // TFLG untouched (reset 0); driver owns it

    // TIF is a level source, so EDGE is safe only because the driver W1Cs TFLG before
    // the next wait re-arms the line.
    kos_cap_t irq = KOS_CAP_NONE;
    if (kos_irq_claim(PIT2_IRQ, KOS_IRQ_EDGE, &irq) != 0)
    {
        kos::print("[k64drv] ERROR: irq_claim(PIT2) failed\n");
    }
    // The object the line raises into, attached with an UNBADGED copy so it lands on bit 0.
    kos_cap_t note = KOS_CAP_NONE;
    if (kos_notify_create(&note) != 0 or kos_irq_bind_notify(irq, note) != 0)
    {
        kos::print("[k64drv] ERROR: the line could not be attached to a notification\n");
    }
    kos_cap_grant const caps[2] = {{irq, KOS_CAP_WAIT}, {note, KOS_CAP_WAIT}};

    auto drv = kos::thread::create(pit_driver, reinterpret_cast<void*>(PIT_CH2), "k64drv", 10,
                                   KOS_POLICY_FIFO, 0, /*privileged=*/false,
                                   /*mem=*/nullptr, /*mem_size=*/0,
                                   /*stack=*/nullptr, /*stack_size=*/0,
                                   /*mmio=*/reinterpret_cast<void*>(PIT_CH2), PIT_CH2_WINDOW,
                                   caps, 2);
    if (not drv.valid())
    {
        // The console is the only oracle at the bench: without this line a failed spawn
        // and a dead board read the same.
        kos::print("[k64drv] ERROR: driver spawn failed\n");
    }
    if (note != KOS_CAP_NONE)
    {
        kos_handle_close(note); // the driver's bind and the line's attach hold their own
    }
    if (irq != KOS_CAP_NONE)
    {
        kos_handle_close(irq); // the driver is the sole holder from here
    }

    // Sleep park when the semaphore could not be created: an unmintable handle would spin
    // a hot loop of failing sem_wait syscalls.
    kos_cap_t idle = KOS_CAP_NONE;
    (void)kos_sem_create(0, &idle);
    while (true)
    {
        if (idle == KOS_CAP_NONE)
        {
            kos_sleep_ns(1000000000ull);
            continue;
        }
        kos_sem_wait(idle);
    }
}
