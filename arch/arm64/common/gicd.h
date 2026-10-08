// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The distributor registers both GIC backends program at the same displacement (IHI 0048B.b
// 4.1.2, IHI 0069H.b 12.9). For the backends alone: gic.h, which chips include, names none.

#ifndef KICKOS_ARCH_ARM64_COMMON_GICD_H
#define KICKOS_ARCH_ARM64_COMMON_GICD_H

#include <stdint.h>

namespace kickos::arm64
{
    constexpr uintptr_t GICD_CTLR = 0x000;
    constexpr uintptr_t GICD_ISENABLER = 0x100;
    constexpr uintptr_t GICD_ICENABLER = 0x180;
    constexpr uintptr_t GICD_ISPENDR = 0x200;
    constexpr uintptr_t GICD_ICPENDR = 0x280;
    constexpr uintptr_t GICD_IPRIORITYR = 0x400;

    // The kind, as a value range rather than a separate field. Below 32 an INTID is the calling
    // core's own: GICv2 banks it in the distributor, GICv3 under affinity routing reaches it in
    // that core's redistributor, the distributor's first word being RES0 there. At or above 32
    // an interrupt is global and reaches no core until it is routed. Every arch_irq_* body
    // branches on this boundary and nothing else does.
    constexpr int GIC_BANKED_INTIDS = 32;
}

#endif
