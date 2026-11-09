// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// KickOS "blink": the bare-metal bring-up smoke test. A spawned thread toggles
// the board's LED and sleeps between edges, so a steady ~2.5 Hz blink is
// end-to-end proof of the whole path on real silicon: reset -> C runtime ->
// scheduler start -> thread spawn + context switch (main blocks, the blinker
// runs) -> SysTick one-shot sleep -> wake. The witness is visual: eyes on the LED.
//
// The LED is the KERNEL's single diagnostic pin, driven through a syscall
// (kos_kernel_diag_led_*); the chip backend (arch_diag_led_*) owns which pin that
// is. A board without a known LED makes the toggle a no-op.

#include <kickos/kos.h>
#include <kickos/sys.h>

#include <stdint.h>

namespace
{
    constexpr uint64_t BLINK_NS = 200000000ull; // 0.2 s per edge -> ~2.5 Hz
    constexpr int EDGES = 50; // 10 s, ending with the LED off

    kos::Semaphore g_done;

    void blinker(void*)
    {
        bool on = false;
        for (int edge = 0; edge < EDGES; edge++)
        {
            on = not on;
            kos_kernel_diag_led_set(on);
            kos_sleep_ns(BLINK_NS);
        }
        g_done.post();
    }
}

int main(int, char**)
{
    kos_print("blink: heartbeat on the kernel diagnostic LED\n");

    kos::thread::create(blinker, nullptr, "blink", 10);

    // Main blocks so the blinker owns the CPU, which proves the switch.
    (void)g_done.wait();
    return 0;
}
