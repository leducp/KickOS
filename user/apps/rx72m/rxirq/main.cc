// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// RX72M IRQ15 (vector 79, UM r01uh0804ej0120 Table 15.5 p.522), a plain INTB vector, claimed and
// raised from its own pin: P07 is muxed as general I/O with PmnPFS.ISEL set (UM sec.23.2.2) and
// driven as an output, so its pad feeds the IRQ15 input. IRQCR15 keeps its reset low-level
// detection (UM sec.15.2.9 p.487), so a low drive is a standing request: each cycle drives the pin
// low, takes exactly one wake, sleeps with the request still standing, then drives it high and
// waits for a wake that must not come. The sleep ending is the proof that the line was masked on
// fire: an unmasked level source at IPL 4 would keep the IPL 1 switcher out for good.
//
// P07 is not connected on the CPU card (board UM r12uz0098ej0110 Table 6-1, pin E5).

#include <kickos/kos.h>
#include <kickos/sys.h>
#include <kickos/sys/errno.h>
#include <kickos/libc/fmt.h>

#include <stdint.h>
#include <stdlib.h>

namespace
{
    constexpr int IRQ15_LINE = 79;

    // UM sec.22.3: one byte per port in each register row.
    constexpr uint32_t PDR_OFFSET = 0x00u;
    constexpr uint32_t PODR_OFFSET = 0x20u;
    constexpr uint32_t PIDR_OFFSET = 0x40u;
    constexpr uint32_t WINDOW_BYTES = 0x50u;
    constexpr uint32_t PORT0 = 0u;
    constexpr uint32_t P07 = 7u;
    constexpr uint8_t PIN = 1u << P07;

    // arch_pinmux_set `func` word, mirrored from arch/rx/chip/rx72m/regs/mpc.h: [7:0] the PmnPFS
    // byte, [9] arm the PFS write; PMR clear keeps the pin general I/O.
    constexpr uint32_t PINMUX_PFS_EN = 1u << 9;
    constexpr uint32_t PFS_ISEL = 1u << 6;

    constexpr int CYCLES = 5;
    constexpr int64_t WAKE_WAIT_MS = 100;
    constexpr int64_t HOLD_MS = 50;
    constexpr int64_t QUIET_WAIT_MS = 50;

    inline volatile uint8_t& r8(uintptr_t a)
    {
        return *reinterpret_cast<volatile uint8_t*>(a);
    }

    void say(char const* m)
    {
        kos::print(m);
    }
}

int main(int, char**)
{
    char m[112];
    kos_window_t const window = kos_grant_mmio("/dev/port");
    uintptr_t const win = reinterpret_cast<uintptr_t>(kos_window_addr(window));
    if (win == 0u or kos_window_size(window) < WINDOW_BYTES)
    {
        say("[rxirq] ERROR: no /dev/port window\n");
        exit(1);
    }
    uintptr_t const pdr = win + PDR_OFFSET + PORT0;
    uintptr_t const podr = win + PODR_OFFSET + PORT0;
    uintptr_t const pidr = win + PIDR_OFFSET + PORT0;

    // High before the direction and before ISEL, so no request stands when the line is armed.
    r8(podr) = static_cast<uint8_t>(r8(podr) | PIN);
    r8(pdr) = static_cast<uint8_t>(r8(pdr) | PIN);
    int const mux = kos_pinmux_set(PORT0, P07, PINMUX_PFS_EN | PFS_ISEL);
    ksnprintf(m, sizeof(m), "[rxirq] pinmux P07 -> IRQ15 input rc %d (want 0)\n", mux);
    say(m);
    if (mux != 0)
    {
        exit(1);
    }

    kos::Irq irq = kos::Irq::claim(IRQ15_LINE);
    ksnprintf(m, sizeof(m), "[rxirq] claim line %d rc %d (want 0)\n", IRQ15_LINE, irq.error());
    say(m);
    if (not irq.valid())
    {
        exit(1);
    }
    kos_cap_t note = KOS_CAP_NONE;
    if (kos_notify_create(&note) != 0 or irq.watch(note) != 0 or kos_notify_bind(note) != 0)
    {
        say("[rxirq] ERROR: the line could not be bound to a notification\n");
        exit(1);
    }

    bool ok = true;
    for (int i = 1; i <= CYCLES; i++)
    {
        r8(podr) = static_cast<uint8_t>(r8(podr) & ~PIN);
        int const pad_lo = static_cast<int>((r8(pidr) >> P07) & 1u);
        uint32_t bits = 0;
        int const woke = kos_notify_wait(1u, kos_after(kos_monotonic_now(), kos_ms(WAKE_WAIT_MS)),
                                         &bits);
        kos_sleep_for(kos_ms(HOLD_MS));

        r8(podr) = static_cast<uint8_t>(r8(podr) | PIN);
        int const pad_hi = static_cast<int>((r8(pidr) >> P07) & 1u);
        int const quiet =
            kos_notify_wait(1u, kos_after(kos_monotonic_now(), kos_ms(QUIET_WAIT_MS)), nullptr);

        ksnprintf(m, sizeof(m),
                  "[rxirq] cycle %d: pad %d/%d, low woke rc %d bits 0x%x, high rc %d\n", i,
                  pad_lo, pad_hi, woke, static_cast<unsigned>(bits), quiet);
        say(m);
        if (woke != 0 or bits != 1u or quiet != -KOS_ETIMEDOUT)
        {
            ok = false;
        }
    }
    if (ok)
    {
        say("[rxirq] PASS (one wake per low drive, none while high)\n");
    }
    else
    {
        say("[rxirq] FAIL\n");
    }
    exit(0);
}
