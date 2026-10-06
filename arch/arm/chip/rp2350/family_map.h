// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The RP2350's membership of the RP2xxx family (../rp2xxx/rp2xxx.h). Read by
// chip_rp2xxx.cc.

#ifndef KICKOS_ARCH_ARM_CHIP_RP2350_FAMILY_MAP_H
#define KICKOS_ARCH_ARM_CHIP_RP2350_FAMILY_MAP_H

// Opened empty so the alias below can name it before any regs/ header has.
namespace kickos::rp2350::reg
{
}

namespace kickos::rp2xxx
{
    namespace reg = kickos::rp2350::reg;
}

#endif
