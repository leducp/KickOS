// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

// Lone-TU fallback (arch/CMakeLists.txt states the rule): exactly one symbol, so a
// backend definition keeps this archive member unextracted.
//
// Telemetry trace clock = the raw DWT cycle counter (32-bit, wraps), cycle-accurate on
// real silicon and already running (kickos_armv7m_init enabled it). A part whose DWT is
// frozen or absent (QEMU mps2) defines its own.
//
// Runs on the PendSV-tail emit path, so this TU must stay FP-register-free
// (-mgeneral-regs-only under telemetry; arch/CMakeLists.txt applies it).

#include <kickos/arch/arch.h>

#include "regs.h"

#include <stdint.h>

extern "C" uint32_t arch_trace_now(void)
{
    return kickos::arm::reg32(kickos::armv7m::DWT_CYCCNT);
}
