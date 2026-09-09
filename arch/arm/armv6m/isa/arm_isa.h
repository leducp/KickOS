// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The ARMv6-M values arch_arm_common.cc compiles against. Reached as <arm_isa.h>, and only
// the archive of THIS profile puts this directory on the include path, so the one spelling
// in arch_arm_common.cc resolves to the v7-M file in the other archive. This directory holds
// nothing else, which is what keeps the sibling regs.h off that path.
//
// Each banner is spelled WHOLE. tests/static/check_panic_banners.sh derives what
// tests/lib/panic.ere must match from the string literals themselves and reads no
// concatenation, so composing one from an arch noun would take it out of that gate's sight.

#ifndef KICKOS_ARCH_ARM_ARMV6M_ISA_ARM_ISA_H
#define KICKOS_ARCH_ARM_ARMV6M_ISA_ARM_ISA_H

#include <stdint.h>

#define KICKOS_ARM_BANNER_WILD_PSP_WHY "\n=== ARMV6M EXCEPTION (wild PSP: %s) ===\n"
#define KICKOS_ARM_BANNER_WILD_PSP_WHY_CONTAINED "\n=== ARMV6M CONTAINED (wild PSP: %s) ===\n"
#define KICKOS_ARM_BANNER_WILD_PSP "\n=== ARMV6M EXCEPTION (wild PSP) ===\n"
#define KICKOS_ARM_BANNER_WILD_PSP_CONTAINED "\n=== ARMV6M CONTAINED (wild PSP) ===\n"
#define KICKOS_ARM_BANNER_NO_KERNEL_STACK "\n=== ARMV6M EXCEPTION (no kernel stack) ===\n"

// The critical section is PRIMASK, which masks every configurable line whatever its
// priority, so a device line's priority does not gate it and arch_irq_unmask enables the
// line alone. NVIC IPR is word-access only on this profile and nothing here writes it.
static inline void kickos_arm_irq_line_prio(unsigned line)
{
    (void)line;
}

#endif
