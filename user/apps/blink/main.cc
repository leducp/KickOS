// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// KickOS "blink": the bare-metal bring-up smoke test. A spawned thread toggles
// the board's LED and sleeps between edges, so a steady ~2.5 Hz blink is
// end-to-end proof of the whole path on real silicon: reset -> C runtime ->
// scheduler start -> thread spawn + context switch (main blocks, the blinker
// runs) -> SysTick one-shot sleep -> wake. It needs no UART adapter -- just eyes
// on the LED.
//
// No board-specific code here: the LED is the KERNEL's single diagnostic pin,
// driven through a syscall (kos_kernel_diag_led_*). Which pin that is lives in
// the chip backend (arch_diag_led_*), not in this app. On a board with no known
// LED the toggle is a harmless no-op.

#include <kickos/kos.h>
#include <kickos/sys.h>

#include <stdint.h>

namespace
{
    constexpr uint64_t kBlinkNs = 200000000ull; // 0.2 s per edge -> ~2.5 Hz

    void blinker(void*)
    {
        while (true)
        {
            kos_kernel_diag_led_toggle();
            kos_sleep_ns(kBlinkNs);
        }
    }
}

int main(int, char**)
{
    kos_print("blink: heartbeat on the kernel diagnostic LED\n");

    kos::thread::spawn(blinker, nullptr, "blink", 10);

    // Root parks so the blinker owns the CPU; blocking here proves the switch.
    int idle = kos_sem_create(0);
    while (true)
    {
        kos_sem_wait(idle);
    }
}
