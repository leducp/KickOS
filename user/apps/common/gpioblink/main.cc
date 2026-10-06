// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// GPIO direct-MMIO demo: userspace owns GPIO. The composition grants the task the LED port's
// block as a window and `pinmux`; the task's entry muxes its own pin and toggles it by
// writing that window DIRECTLY, with no syscall per edge. A syscall-per-toggle cannot serve a
// hot pin, so the model is direct MMIO with a per-chip isolation ceiling on the granted
// window.
//
// PORT/PIN/MUX and the window's path come from compile defs KICKOS_GPIOBLINK_PORT / _PIN /
// _MUX / _DEVICE. The register layout is per chip (KICKOS_GPIOBLINK_XMC / _K64F, set by
// CMake from KICKOS_CHIP). Register offsets are mirrored as local constexprs from the
// canonical per-chip regs/ headers (cited below).

#include <kickos/kos.h>
#include <kickos/sys.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#if !defined(KICKOS_GPIOBLINK_XMC) && !defined(KICKOS_GPIOBLINK_K64F)
#error "gpioblink drives the XMC4800's or the K64F's GPIO layout; not for this chip"
#endif

namespace
{
    constexpr uint32_t PORT = KICKOS_GPIOBLINK_PORT;
    constexpr uint32_t PIN = KICKOS_GPIOBLINK_PIN;
    constexpr uint32_t MUX = KICKOS_GPIOBLINK_MUX;
    constexpr int CYCLES = 10;
    constexpr uint64_t EDGE_NS = 200000000ull; // 0.2 s per edge -> ~2.5 Hz

    inline volatile uint32_t& r32(uintptr_t a)
    {
        return *reinterpret_cast<volatile uint32_t*>(a);
    }
}

#if defined(KICKOS_GPIOBLINK_XMC)

namespace
{
    // XMC4800 P<port> block. Canonical: arch/arm/chip/xmc4800/regs/port.h (OMR set/reset,
    // IN). Direction lives in the IOCR mux (MUX is PC=0x10, output push-pull GP), so driving
    // needs no direction write. P5.9 (the kernel diag LED) co-resides on P5, which is why the
    // composition accepts coarse_gate; this app must never touch it.
    constexpr uintptr_t OMR_OFF = 0x04u; // write 1<<pin = set high, 1<<(pin+16) = set low
    constexpr uintptr_t IN_OFF = 0x24u;  // read-only pad input (readable even as PP output)

    void gpio_setup_out(uintptr_t) {} // mux already configured the pin as output

    void gpio_drive(uintptr_t win, uint32_t bit, int high)
    {
        if (high != 0)
        {
            r32(win + OMR_OFF) = bit;
        }
        else
        {
            r32(win + OMR_OFF) = bit << 16;
        }
    }

    int gpio_read(uintptr_t win, uint32_t pin)
    {
        return static_cast<int>((r32(win + IN_OFF) >> pin) & 1u);
    }
}

#elif defined(KICKOS_GPIOBLINK_K64F)

namespace
{
    // MK64F GPIO<port> block. Canonical: arch/arm/chip/mk64f/regs/gpio.h (PSOR/PCOR/PDIR/
    // PDDR). Direction is a SEPARATE PDDR write: the task sets output before driving. The K64F
    // GPIO block is unprotectable, which the composition accepts as device_not_isolated.
    constexpr uintptr_t PSOR_OFF = 0x04u; // set -> high
    constexpr uintptr_t PCOR_OFF = 0x08u; // clear -> low
    constexpr uintptr_t PDIR_OFF = 0x10u; // input data
    constexpr uintptr_t PDDR_OFF = 0x14u; // 1 = output

    void gpio_setup_out(uintptr_t win)
    {
        r32(win + PDDR_OFF) |= (1u << PIN); // sole holder of the block: RMW is safe
    }

    void gpio_drive(uintptr_t win, uint32_t bit, int high)
    {
        if (high != 0)
        {
            r32(win + PSOR_OFF) = bit;
        }
        else
        {
            r32(win + PCOR_OFF) = bit;
        }
    }

    int gpio_read(uintptr_t win, uint32_t pin)
    {
        return static_cast<int>((r32(win + PDIR_OFF) >> pin) & 1u);
    }
}

#endif

extern "C" void gpioblink_main(kos_self_t const* self)
{
    printf("[gpioblink] driving port %u pin %u via a direct MMIO grant\n",
           static_cast<unsigned>(PORT), static_cast<unsigned>(PIN));
    fflush(stdout);

    int const mux_rc = kos_pinmux_set(PORT, PIN, MUX);
    uintptr_t const win =
        reinterpret_cast<uintptr_t>(kos_window_addr(kos_grant_mmio(self, KICKOS_GPIOBLINK_DEVICE)));
    if (mux_rc != 0 or win == 0u)
    {
        printf("[gpioblink] ERROR: pinmux rc %d, window %s\n", mux_rc,
               KICKOS_GPIOBLINK_DEVICE);
        fflush(stdout);
        exit(1);
    }
    uint32_t const bit = 1u << PIN;

    gpio_setup_out(win);

    bool ok = true;
    for (int i = 0; i < CYCLES; i++)
    {
        gpio_drive(win, bit, 1);
        kos_sleep_ns(EDGE_NS);
        int const r1 = gpio_read(win, PIN);
        gpio_drive(win, bit, 0);
        kos_sleep_ns(EDGE_NS);
        int const r0 = gpio_read(win, PIN);
        printf("[gpioblink] cycle %d led=1 readback=%d / led=0 readback=%d\n", i, r1, r0);
        fflush(stdout);
        if (r1 != 1 or r0 != 0)
        {
            ok = false;
        }
    }

    if (ok)
    {
        printf("[gpioblink] PASS (%d cycles, readback ok)\n", CYCLES);
    }
    else
    {
        printf("[gpioblink] FAIL (readback did not track the drive)\n");
    }
    fflush(stdout);

    // A granted pin is owned for the task's life, so it keeps the window and slow-blinks on.
    while (true)
    {
        gpio_drive(win, bit, 1);
        kos_sleep_ns(EDGE_NS * 2u);
        gpio_drive(win, bit, 0);
        kos_sleep_ns(EDGE_NS * 2u);
    }
}
