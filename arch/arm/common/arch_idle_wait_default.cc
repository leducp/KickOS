// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

// Lone-TU fallback (arch/CMakeLists.txt states the rule): exactly one symbol, so a
// backend definition keeps this archive member unextracted.
//
// Real silicon halts to save power. A QEMU semihosting-clock chip (mps2, nrf51) must
// SPIN instead. QEMU <= 10 freezes the semihosting SYS_CLOCK (the monotonic clock
// there) while the core is in WFI, so a timed sleep with every thread idle never wakes.
// QEMU 11 fixed it; the CI runner may ship the older one.

#include <kickos/arch/arch.h>

extern "C" void arch_idle_wait(void)
{
    __asm volatile("wfi");
}
