// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Raspberry Pi Pico (RP2040) pin map. GP2 = plain header pin (no strapping role),
// routed to SIO software-GPIO output. func packs the rp2040 arch_pinmux_set encoding:
// FUNCSEL_SIO(5) in bits[4:0] | bit[9] clear pad OD (drive out) | bit[16] SIO output
// enable (GPIO_OE_SET) = 0x10205.

#include <kickos/sys/pinmap.h>

extern "C"
{
    static struct kos_pinmux_entry const entries[] = { { 0, 2, 0x10205 } };
    struct kos_board_pinmap const kickos_board_pinmap = { entries, 1 };
}
