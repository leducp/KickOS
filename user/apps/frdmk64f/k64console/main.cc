// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// K64F console demo. The composition names the packaged k64uartirq as stdout, so the init hands
// UART0 to that unprivileged driver before main runs, and main and a worker print through it.
// A constructor runs before the handover and prints through the
// kernel console, so the wire shows both paths in order.
//
// K64CONSOLE_SCRAMBLE_TEST instead garbles UART0 from a thread holding no grant and panics: the
// panic must still reach the wire, the kernel reclaiming the console out of the published state.

#include <kickos/kos.h>
#include <kickos/sys.h>

#include <stdint.h>
#include <stdio.h>

#if K64CONSOLE_SCRAMBLE_TEST
#include <kickos/chip_mmap.h>
#endif

#if !KICKOS_HAVE_MPU
#error "k64console requires enforcement: build the board's base variant, not its flat one"
#endif

namespace
{
    // Runs on root before the init hands the console over, so it prints through the kernel.
    __attribute__((constructor)) void prepublish_ctor()
    {
        printf("[init] pre-publish ctor line\n");
        fflush(stdout);
    }

#if !K64CONSOLE_SCRAMBLE_TEST
    constexpr uint8_t WORKER_PRIO = 10;

    void worker(void*)
    {
        for (int i = 0; i < 5; i++)
        {
            printf("[worker] line %d via the userspace console driver\n", i);
            fflush(stdout);
            kos_sleep_ns(100000000ull);
        }
        printf("[worker] done\n");
        fflush(stdout);
    }
#else
    // Above main so it runs at once, and below the driver as every stdout writer must be.
    constexpr uint8_t SCRAMBLER_PRIO = 11;
    // UART0 byte registers (RM ch.52): exactly the ones arch_console_reclaim undoes.
    constexpr uintptr_t OFF_BDH = 0x00u;
    constexpr uintptr_t OFF_BDL = 0x01u;
    constexpr uintptr_t OFF_C2 = 0x03u;
    constexpr uintptr_t OFF_C3 = 0x06u;
    constexpr uintptr_t OFF_MODEM = 0x0Du;
    constexpr uint8_t MODEM_TXCTSE = 1u << 0; // waits forever on an absent CTS
    constexpr uint8_t C3_TXINV = 1u << 4;

    inline volatile uint8_t& r8(uintptr_t a)
    {
        return *reinterpret_cast<volatile uint8_t*>(a);
    }

    void scrambler(void*)
    {
        uintptr_t const uart = kickos::mk64f::mmap::UART0_BASE;
        r8(uart + OFF_MODEM) = MODEM_TXCTSE;
        r8(uart + OFF_BDH) = 0xFFu;
        r8(uart + OFF_BDL) = 0xFFu;
        r8(uart + OFF_C3) = C3_TXINV;
        r8(uart + OFF_C2) = 0; // TX off last
        kos::print("[scramble] UART0 garbled (TXCTSE/baud/TXINV, TX off); ending the system\n");
        // A panic, never a fault: a thread's fault kills it alone and does not reclaim the
        // console, so nothing would reach the wire. Inside user_panic's 64-byte buffer.
        kos_panic("[k64console] PASS: the dead channel came back");
    }
#endif
}

int main(int, char**)
{
#if K64CONSOLE_SCRAMBLE_TEST
    // The driver's up line first, then the garble under it.
    kos_sleep_ns(200000000ull);
    auto const s = kos::thread::create(scrambler, nullptr, "scrambler", SCRAMBLER_PRIO);
    if (not s.valid())
    {
        kos::print("[k64console] ERROR: scrambler spawn failed\n");
        return 1;
    }
    (void)s.join();
    return 1;
#else
    printf("[main] post-publish line via the userspace driver\n");
    fflush(stdout);
    auto const w = kos::thread::create(worker, nullptr, "worker", WORKER_PRIO);
    if (not w.valid())
    {
        printf("[k64console] ERROR: worker spawn refused, errno %d\n", -w.error());
        return 1;
    }
    (void)w.join();
    return 0;
#endif
}
