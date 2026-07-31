// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

// Lone-TU fallback (arch/CMakeLists.txt states the rule): exactly one symbol, so a
// backend definition keeps this archive member unextracted.
//
// -1 == no real device pending. The INTB routes EVERY device source to
// kickos_rx_default_irq and the RXv3 core has no cheap current-vector read, so a chip
// that wires a real peripheral defines its own to read that source's IR/status flag and
// return the vector.

#include <kickos/arch/arch.h>

extern "C" int kickos_rx_dev_pending_line(void)
{
    return -1;
}
