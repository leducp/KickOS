// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Minimal out-of-tree BARE-METAL KickOS application. The sibling example
// (examples/oot-app) proves the packaging surface on the host sim, where there
// is no linker script, no reset vector and no flashable image -- so none of the
// MCU-specific machinery is exercised. This one is built against an installed
// MCU package with the shipped cross toolchain, and never runs on the host.
//
// Nothing here is board-specific: the LED is the kernel's single diagnostic pin,
// driven through a syscall, so this same source builds for any KickOS MCU
// package. On a board with no known LED the toggle is a harmless no-op.

#include <kickos/kos.h>

#include <stdint.h>

namespace
{
    constexpr uint64_t BLINK_NS = 200000000ull; // 0.2 s per edge -> ~2.5 Hz

    void blinker(void*)
    {
        while (true)
        {
            kos_kernel_diag_led_toggle();
            kos_sleep_ns(BLINK_NS);
        }
    }
}

// A plain, OS-agnostic entry: the KickOS package renames it to the kernel entry.
int main(int, char**)
{
    kos::print("[oot-mcu] hello from an out-of-tree bare-metal KickOS app\n");

    kos::thread::spawn(blinker, nullptr, "blink", 10);

    // Bare metal has nowhere to return to, so the root thread parks forever.
    int idle = kos_sem_create(0);
    while (true)
    {
        kos_sem_wait(idle);
    }
}
