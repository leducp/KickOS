// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

// Lone-TU fallback (arch/CMakeLists.txt states the rule): exactly one symbol, so a
// backend definition keeps this archive member unextracted.
//
// Reuses the ordinary writer. Correct ONLY on a polled-only chip (mps2/virt/nrf51),
// where arch_console_write already IS the polled writer. A chip with a buffered
// console MUST define its own: panic and fault output would otherwise enqueue into a
// ring whose drain ISR is masked and never runs.

#include <kickos/arch/arch.h>

#include <stddef.h>

extern "C" void arch_console_write_sync(char const* buf, size_t n)
{
    arch_console_write(buf, n);
}
