// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// FRDM-K64F pin map. PTB21 = blue LED muxed as GPIO (MUX ALT1); direction/drive is
// the GPIO step's job. (PTB22 = red is the kernel diag LED, off-limits.)
//
// PTD1/PTD2/PTD3 = DSPI0 SCK/SOUT/SIN (Arduino D13/D11/D12) at MUX ALT2, and PTC4 =
// the k64dspi software chip select (Arduino D9) as GPIO at MUX ALT1. Direction and
// idle level for PTC4 are the driver's job. Muxing DSPI0's pins is glitch-free ahead
// of the DSPI config only because CTAR0 resets to CPOL=0, matching the pin idle level;
// a CPOL=1 profile only takes effect inside a transfer, after its own CS asserts.

#include <kickos/sys/pinmap.h>

extern "C"
{
    static struct kos_pinmux_entry const entries[] = {
        { 1, 21, 0x100 }, // PTB21 blue LED, GPIO
        { 3, 1, 0x200 },  // PTD1 DSPI0 SCK
        { 3, 2, 0x200 },  // PTD2 DSPI0 SOUT
        { 3, 3, 0x200 },  // PTD3 DSPI0 SIN
        { 2, 4, 0x100 },  // PTC4 DSPI0 software CS, GPIO
    };
    struct kos_board_pinmap const kickos_board_pinmap = { entries, 5 };
}
