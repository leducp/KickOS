// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// GPIO direct-MMIO demo (M4.3): the kernel does NOT do GPIO. A privileged bring-up main
// (the root thread runs privileged) grants the LED port's register block to an
// UNPRIVILEGED worker as a spawn MMIO window; the worker toggles the pin by writing that
// window DIRECTLY: no syscall per edge. A syscall-per-toggle cannot serve a hot pin (a
// spike measured an SVC round-trip well above a 16-bit chip-select's edge budget), so the
// honest model is direct MMIO with a per-chip isolation ceiling on the granted window.
//
// The pin was already muxed by the default init's board pin-map (the clock->pinmux->gpio
// bring-up DAG). This app never touches a mux register; it only drives + reads back.
//
// PORT/PIN come from compile defs KICKOS_GPIOBLINK_PORT / _PIN. The register layout is per
// chip (KICKOS_GPIOBLINK_XMC / _K64F, set by CMake from KICKOS_CHIP). Register offsets are
// mirrored as local constexprs from the canonical per-chip regs/ headers (cited below); a
// cross-tree include from user/ does not resolve cleanly and would break the sim/qemu
// builds this app must also compile on. A chip with no layout here builds a park-only stub.

#include <kickos/kos.h>
#include <kickos/sys.h>

#include <stdint.h>
#include <stdio.h>

namespace
{
    constexpr uint32_t PORT = KICKOS_GPIOBLINK_PORT;
    constexpr uint32_t PIN = KICKOS_GPIOBLINK_PIN;
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
    // IN) + mmap.h (PORT0_BASE 0x48028000, PORT_STRIDE 0x100). Direction lives in the IOCR
    // mux (init set PC=0x10, output push-pull GP), so there is no direction write here.
    constexpr uintptr_t WINDOW_BASE = 0x48028000u + PORT * 0x100u;
    // Whole port block: OMR is inseparable from IOCR (a sub-region cannot split them), so
    // the grant is a TRUSTED OVER-GRANT. P5.9 (the kernel diag LED) co-resides on P5; this
    // app must never touch it. This shared-port over-grant is the documented XMC limit.
    constexpr uint32_t WINDOW_SIZE = 0x100u;
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

#define KICKOS_GPIOBLINK_HAVE_LAYOUT 1

#elif defined(KICKOS_GPIOBLINK_K64F)

namespace
{
    // MK64F GPIO<port> block. Canonical: arch/arm/chip/mk64f/regs/gpio.h (PSOR/PCOR/PDIR/
    // PDDR) + mmap.h (GPIOA_BASE 0x400FF000, GPIO_STRIDE 0x40). Direction is a SEPARATE
    // PDDR write (unlike XMC): the worker sets output before driving.
    constexpr uintptr_t WINDOW_BASE = 0x400FF000u + PORT * 0x40u;
    // Whole GPIO instance block. Per the spike the K64F GPIO block is unprotectable (the
    // grant is inert here), but it is kept for spawn-signature parity + portability to an
    // enforcing chip.
    constexpr uint32_t WINDOW_SIZE = 0x40u;
    constexpr uintptr_t PSOR_OFF = 0x04u; // set -> high
    constexpr uintptr_t PCOR_OFF = 0x08u; // clear -> low
    constexpr uintptr_t PDIR_OFF = 0x10u; // input data
    constexpr uintptr_t PDDR_OFF = 0x14u; // 1 = output

    void gpio_setup_out(uintptr_t win)
    {
        r32(win + PDDR_OFF) |= (1u << PIN); // sole owner of the block: RMW is safe
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

#define KICKOS_GPIOBLINK_HAVE_LAYOUT 1

#endif

#if defined(KICKOS_GPIOBLINK_HAVE_LAYOUT)

namespace
{
    // UNPRIVILEGED worker: the granted window base arrives as the thread arg VALUE (never a
    // pointer into mutable file-scope state, which the grant would not cover under
    // enforcement). Drives + reads back for a few cycles, then slow-blinks forever.
    void worker(void* arg)
    {
        uintptr_t const win = reinterpret_cast<uintptr_t>(arg);
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

        // Persistent: keep the window and slow-blink forever, mirroring the design. A
        // granted pin is owned for the driver's life.
        while (true)
        {
            gpio_drive(win, bit, 1);
            kos_sleep_ns(EDGE_NS * 2u);
            gpio_drive(win, bit, 0);
            kos_sleep_ns(EDGE_NS * 2u);
        }
    }
}

int main(int, char**)
{
    printf("[gpioblink] driving port %u pin %u via a direct MMIO grant\n",
           static_cast<unsigned>(PORT), static_cast<unsigned>(PIN));
    fflush(stdout);

    int const w = kos::thread::spawn(
        worker, reinterpret_cast<void*>(WINDOW_BASE), "gpioblink", 10,
        KOS_POLICY_FIFO, /*quantum_ns=*/0, /*privileged=*/false,
        /*mem=*/nullptr, /*mem_size=*/0, /*stack=*/nullptr, /*stack_size=*/0,
        /*mmio=*/reinterpret_cast<void*>(WINDOW_BASE), WINDOW_SIZE);
    if (w < 0)
    {
        printf("[gpioblink] ERROR: worker spawn failed rc %d\n", w);
        fflush(stdout);
    }

    // Root parks so the worker owns the CPU; blocking here proves the switch.
    int const idle = kos_sem_create(0);
    while (true)
    {
        if (idle < 0)
        {
            kos_sleep_ns(EDGE_NS);
            continue;
        }
        kos_sem_wait(idle);
    }
}

#else // no known GPIO register layout for this chip

int main(int, char**)
{
    printf("[gpioblink] no GPIO register layout for this board; parking\n");
    fflush(stdout);
    while (true)
    {
        kos_sleep_ns(EDGE_NS);
    }
}

#endif
