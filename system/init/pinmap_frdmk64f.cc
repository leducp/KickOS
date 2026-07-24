// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// FRDM-K64F pin map. PTB21 = blue LED muxed as GPIO (MUX ALT1); direction/drive is
// the GPIO step's job. (PTB22 = red is the kernel diag LED, off-limits.)

#include <kickos/sys/pinmap.h>

extern "C"
{
    static struct kos_pinmux_entry const entries[] = { { 1, 21, 0x100 } };
    struct kos_board_pinmap const kickos_board_pinmap = { entries, 1 };
}
