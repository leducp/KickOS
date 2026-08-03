// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

// Lone-TU fallback (arch/CMakeLists.txt states the rule): exactly one symbol, so a
// backend definition keeps this archive member unextracted.
//
// No real device is routed anywhere but the dedicated INTB slots, so there is nothing to
// demux. A chip that routes one defines its own: it reads that source's status register and
// calls kickos_isr_irq ONCE PER ASSERTED SOURCE.

#include <kickos/arch/arch.h>

extern "C" void kickos_rx_dev_dispatch(void)
{
}
