// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#ifndef KICKOS_CHIP_Q35_H
#define KICKOS_CHIP_Q35_H

#include <stddef.h>
#include <stdint.h>

namespace kickos::q35
{
    // The largest conventional-memory range the firmware's memory map named. Published by the
    // handover before boot services are left; arch_ram_base and arch_ram_size answer from it.
    void ram_publish(uintptr_t base, size_t size);
}

#endif
