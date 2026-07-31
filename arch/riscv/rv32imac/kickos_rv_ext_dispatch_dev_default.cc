// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

// Lone-TU fallback (arch/CMakeLists.txt states the rule): exactly one symbol, so a
// backend definition keeps this archive member unextracted.
//
// qemu-virt routes no real device to KICKOS_RV_DEV_CPU_INT. The C6 defines its own to
// ack its level source and run the device line's ISR. Runs in ISR context (switch.S
// .Lextdev).

#include <kickos/arch/arch.h>

extern "C" void kickos_rv_ext_dispatch_dev(void)
{
}
