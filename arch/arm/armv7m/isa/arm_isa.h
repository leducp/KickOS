// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The ARMv7-M values arch_arm_common.cc compiles against. Reached as <arm_isa.h>, and only
// the archive of THIS profile puts this directory on the include path, so the one spelling
// in arch_arm_common.cc resolves to the v6-M file in the other archive. This directory holds
// nothing else, which is what keeps the sibling regs.h off that path.
//
// Each banner is spelled WHOLE. tests/static/check_panic_banners.sh derives what
// tests/lib/panic.ere must match from the string literals themselves and reads no
// concatenation, so composing one from an arch noun would take it out of that gate's sight.

#ifndef KICKOS_ARCH_ARM_ARMV7M_ISA_ARM_ISA_H
#define KICKOS_ARCH_ARM_ARMV7M_ISA_ARM_ISA_H

#include "../regs.h" // NVIC_IPR0, PRIO_DEVICE

#include <stdint.h>

#define KICKOS_ARM_BANNER_WILD_PSP_WHY "\n=== ARMV7M EXCEPTION (wild PSP: %s) ===\n"
#define KICKOS_ARM_BANNER_WILD_PSP_WHY_CONTAINED "\n=== ARMV7M CONTAINED (wild PSP: %s) ===\n"
#define KICKOS_ARM_BANNER_WILD_PSP "\n=== ARMV7M EXCEPTION (wild PSP) ===\n"
#define KICKOS_ARM_BANNER_WILD_PSP_CONTAINED "\n=== ARMV7M CONTAINED (wild PSP) ===\n"
#define KICKOS_ARM_BANNER_NO_KERNEL_STACK "\n=== ARMV7M EXCEPTION (no kernel stack) ===\n"

// Priority into the kernel-maskable band, written by arch_irq_unmask BEFORE the line is
// enabled: NVIC IPR resets to 0x00 and the BASEPRI (0x20) critical section masks only
// priorities numerically >= 0x20, so without this a device IRQ preempts an IrqLock-held
// section (regs.h band, invariants.md device-irq-in-maskable-band). IPR is byte-addressable
// on v7-M. static inline, so the write stays in the caller and adds no call edge.
static inline void kickos_arm_irq_line_prio(unsigned line)
{
    reinterpret_cast<volatile uint8_t*>(kickos::armv7m::NVIC_IPR0)[line] =
        static_cast<uint8_t>(kickos::armv7m::PRIO_DEVICE);
}

#endif
