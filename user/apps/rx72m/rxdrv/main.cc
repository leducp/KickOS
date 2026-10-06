// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// RX72M (RXv3) LED6 blink: per-thread peripheral isolation on the RX MPU, which is CPU-side and
// checks every user-mode access to the whole address space, the SFR aperture included (UM
// r01uh0804ej0120 sec.17.1, Table 17.1). The task's entry muxes P80 through kos_pinmux_set,
// probes kos_periph_enable as the window's holder, drives and reads back LED6 through the port
// window its composition grants, then a child thread holding no window probes the same call and
// writes the console pin's function select in the MPC, which nothing grants: the fault kills the
// child and ends the task.
//
// LED6 (P80, active-low, board UM r12uz0098ej0110 Table 5-9) is also the kernel's diagnostic LED.

#include <kickos/chip_mmap.h>
#include <kickos/kos.h>
#include <kickos/sys.h>
#include <kickos/sys/errno.h>
#include <kickos/libc/fmt.h>

#include <port_class.h>

#include <stdint.h>
#include <stdlib.h>

// Without enforcement the ungranted write lands and prints an isolation-failure line.
#if !KICKOS_HAVE_MPU
#error "rxdrv requires enforcement: build the board's base variant, not its flat one"
#endif

namespace
{
    // UM sec.22.3: one byte per port in each register row.
    constexpr uint32_t PDR_OFFSET = 0x00u;
    constexpr uint32_t PODR_OFFSET = 0x20u;
    constexpr uint32_t PIDR_OFFSET = 0x40u;
    constexpr uint32_t WINDOW_BYTES = 0x50u;
    constexpr uint32_t PORT8 = 8u;
    constexpr uint32_t P80 = 0u;
    constexpr uint8_t LED6 = 1u << P80;

    // arch_pinmux_set `func` word, mirrored from arch/rx/chip/rx72m/regs/mpc.h: [7:0] the PmnPFS
    // byte, [8] the PORTm.PMR bit value, [9] arm the PFS write.
    constexpr uint32_t PINMUX_PFS_EN = 1u << 9;
    constexpr uint32_t PFS_PSEL_HIZ = 0x00u; // general I/O

    // The console's TXD6 pin, which the kernel owns for life (port index 0x0B).
    constexpr uint32_t PORTB = 0x0Bu;
    constexpr uint32_t PB1 = 1u;
    // PB1PFS (UM sec.23.2): PFS block at MPC + 0x40, one byte per pin, eight per port.
    constexpr uintptr_t PB1PFS = kickos::rx::mmap::MPC + 0x40u + PORTB * 8u + PB1;

    constexpr int DRIVER_BLINKS = 10;
    constexpr uint64_t HALF_PERIOD_NS = 250000000ull;

    inline volatile uint8_t& r8(uintptr_t a)
    {
        return *reinterpret_cast<volatile uint8_t*>(a);
    }

    void verdict(char const* what, int rc, int want)
    {
        char const* v = "FAIL";
        if (rc == want)
        {
            v = "PASS";
        }
        char m[80];
        ksnprintf(m, sizeof(m), "[rxdrv] %s %s rc %d (want %d)\n", v, what, rc, want);
        kos::print(m);
    }

    // Holds no window: the base arrives as a value, never dereferenced as memory.
    void poker(void* arg)
    {
        uintptr_t const win = reinterpret_cast<uintptr_t>(arg);
        // The refusal contract: the call never reaches the chip for a thread not holding the
        // window.
        verdict("periph_enable non-holder", kos_periph_enable(win), -KOS_EPERM);

        // A plain store, so the report names the escalating write: an RMW would fault on its read.
        kos::print("[rxdrv] poking UNGRANTED MPC PB1PFS @ 0x0008C199 (expect MPU FAULT)\n");
        r8(PB1PFS) = PFS_PSEL_HIZ;

        kos::print("[rxdrv] UNGRANTED ACCESS DID NOT FAULT (MPU not enforcing)\n");
    }
}

extern "C" void rxdrv_main(kos_self_t const* self)
{
    kos_window_t const window = kos_grant_mmio(self, "/dev/port");
    uintptr_t const win = reinterpret_cast<uintptr_t>(kos_window_addr(window));
    if (win == 0u or kos_window_size(window) < WINDOW_BYTES)
    {
        kos::print("[rxdrv] ERROR: no /dev/port window\n");
        exit(1);
    }

    // Both mux stages in one mediated call: PmnPFS PSEL=000000b and PORT8.PMR bit 0 clear.
    int const mux = kos_pinmux_set(PORT8, P80, PINMUX_PFS_EN | PFS_PSEL_HIZ);
    char m[64];
    ksnprintf(m, sizeof(m), "[rxdrv] pinmux P80 -> general I/O rc %d\n", mux);
    kos::print(m);
    if (mux != 0)
    {
        exit(1);
    }
    // If this refusal ever broke, this very write would dark the console, so a run stopping here
    // is itself the failure.
    int const owned = kos_pinmux_set(PORTB, PB1, PINMUX_PFS_EN | PFS_PSEL_HIZ);
    if (owned == -KOS_EBUSY)
    {
        kos::print("[rxdrv] pinmux PB1/TXD6 refused (-KOS_EBUSY): console pin is kernel-owned\n");
    }
    else
    {
        ksnprintf(m, sizeof(m), "[rxdrv] ERROR: PB1/TXD6 not refused, rc %d\n", owned);
        kos::print(m);
    }

    // The holder reaches arch_periph_enable, which answers for RIIC0/1/2 only and refuses the port
    // block.
    verdict("periph_enable holder", kos_periph_enable(win), -KOS_EINVAL);

    uintptr_t const pdr = win + PDR_OFFSET + PORT8;
    uintptr_t const podr = win + PODR_OFFSET + PORT8;
    uintptr_t const pidr = win + PIDR_OFFSET + PORT8;
    // Drive high (LED off) before the direction, so the pin does not glitch on.
    r8(podr) = static_cast<uint8_t>(r8(podr) | LED6);
    r8(pdr) = static_cast<uint8_t>(r8(pdr) | LED6);
    uint8_t const odr = kickos::rx::driver::port_odr_read(win + PODR_OFFSET, PORT8);
    char rb[48];
    ksnprintf(rb, sizeof(rb), "[rxdrv] PORT8 PODR readback 0x%x\n", odr);
    kos::print(rb);
    kos::print("[rxdrv] blinking LED6 (P80) via the port window\n");

    // PIDR is the pad, not the latch (UM sec.22.3.3): the console-visible proof the pin moved.
    bool ok = true;
    for (int i = 0; i < DRIVER_BLINKS; i++)
    {
        r8(podr) = static_cast<uint8_t>(r8(podr) & ~LED6);
        kos_sleep_ns(HALF_PERIOD_NS);
        int const lo = static_cast<int>((r8(pidr) >> P80) & 1u);
        r8(podr) = static_cast<uint8_t>(r8(podr) | LED6);
        kos_sleep_ns(HALF_PERIOD_NS);
        int const hi = static_cast<int>((r8(pidr) >> P80) & 1u);

        char s[64];
        ksnprintf(s, sizeof(s), "[rxdrv] blink %d pad=0/%d pad=1/%d\n", i + 1, lo, hi);
        kos::print(s);
        if (lo != 0 or hi != 1)
        {
            ok = false;
        }
    }
    if (ok)
    {
        kos::print("[rxdrv] PASS (pad tracked the drive on every cycle)\n");
    }
    else
    {
        kos::print("[rxdrv] FAIL (pad did not track the drive)\n");
    }

    auto const child = kos::thread::create(poker, reinterpret_cast<void*>(win), "rxpoke", 10);
    if (not child.valid())
    {
        ksnprintf(m, sizeof(m), "[rxdrv] ERROR: poker spawn rc %d\n", child.error());
        kos::print(m);
        exit(1);
    }
    (void)child.join();
    exit(1);
}
