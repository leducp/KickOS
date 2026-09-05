// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

// Lone-TU fallback (arch/CMakeLists.txt states the rule): exactly one symbol, so a
// backend definition keeps this archive member unextracted.
//
// Raw 32-bit CCOUNT; the host reconstructs absolute time from the SESSION clock_hz
// anchors. Read raw, so it is callable from the switch path.
//
// PER CORE: each CPU counts from its own reset and nothing synchronises them, so above one
// core a chip owes its own override off a counter both cores read (esp32 TIMG).

#include <kickos/arch/arch.h>

#include <stdint.h>

extern "C"
{
    uint32_t arch_trace_now(void)
    {
        uint32_t v;
        __asm volatile("rsr.ccount %0" : "=a"(v));
        return v;
    }
}
