// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

// Lone-TU fallback (arch/CMakeLists.txt states the rule): exactly one symbol, so a
// backend definition keeps this archive member unextracted.
//
// The qemu-virt SSIP path clears its own pending in kickos_rv_dispatch_soft. A chip with
// a level-triggered source defines its own: this runs at the head of the external-
// doorbell trap, BEFORE the line's ISR, so the source is de-asserted before mret.

#include <kickos/arch/arch.h>

extern "C" void arch_rv_ext_eoi(void)
{
}
