// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// XMC4800 Relax Kit pin map. P5.8 = LED2 muxed as output push-pull GP; the GPIO
// step drives it. (P5.9 = LED1 is the kernel diag LED, off-limits.)

#include <kickos/sys/pinmap.h>

extern "C"
{
    static struct kos_pinmux_entry const entries[] = { { 5, 8, 0x10 } };
    struct kos_board_pinmap const kickos_board_pinmap = { entries, 1 };
}
