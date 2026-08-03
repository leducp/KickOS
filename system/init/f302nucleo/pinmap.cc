// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// ST Nucleo-F302R8 (STM32F302R8) pin map. PA0 is free: the console is PA2/PA3, LD2 is
// PB13. func is the MODER bits[1:0] field written verbatim. OTYPER/OSPEEDR/PUPDR stay at
// their reset values.

#include <kickos/sys/pinmap.h>

extern "C"
{
    static struct kos_pinmux_entry const entries[] = { { 0, 0, 0x1 } };
    struct kos_board_pinmap const kickos_board_pinmap = { entries, 1 };
}
