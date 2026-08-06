// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Minimal out-of-tree BARE-METAL KickOS application: built against an installed MCU
// package with the shipped cross toolchain, never run on the host. The host-sim half
// of the packaging surface is the sibling examples/oot-app.
//
// Nothing here is board-specific: the LED is the kernel's single diagnostic pin,
// driven through a syscall. On a board with no known LED the toggle is a no-op.

#include <kickos/kos.h>

// kos.h already reaches config/cap_width.h (kos.h -> sys.h -> sys/abi.h -> sys/cap_index.h);
// named here because the assertion below is what reads it, not because the include is
// otherwise missing.
#include <kickos/config/cap_width.h>
static_assert(KICKOS_CAP_CHILD_WIDTH <= KICKOS_MAX_HANDLES,
              "the installed kickos/config/cap_width.h is the one the libraries were built "
              "with");

// The provisioning half of the same fact, and the only thing that compiles the installed
// kickos/board_config.h and the chip's include directory (config/board.h #errors without
// the chip's KICKOS_MAX_IRQ). Nothing on the kos.h path pulls a kernel config header, so
// both install() rules could be deleted with this gate still green. KICKOS_EXPECT_* are
// the values the KickOS build this links against resolved, passed in by
// tests/check_oot_export_mcu.sh; without board_config.h the knobs fall back to
// config/system.h's fleet defaults and the libraries are sized differently, which nothing
// would report until a spawn failed on the target.
#include <kickos/config.h>
#ifdef KICKOS_EXPECT_MAX_THREADS
static_assert(KICKOS_MAX_THREADS == KICKOS_EXPECT_MAX_THREADS,
              "the installed provisioning is not the one the linked KickOS was built with");
#endif
#ifdef KICKOS_EXPECT_USER_STACK_SIZE
static_assert(KICKOS_USER_STACK_SIZE == KICKOS_EXPECT_USER_STACK_SIZE,
              "the installed provisioning is not the one the linked KickOS was built with");
#endif

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
    kos_cap_t idle = KOS_CAP_NONE;
    (void)kos_sem_create(0, &idle);
    while (true)
    {
        kos_sem_wait(idle);
    }
}
