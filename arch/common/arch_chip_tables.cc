// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include <kickos/arch/arch.h>

#include "chip_tables.h"

struct arch_reserved_span arch_reserved_blocks(void)
{
    return kickos::chip::reserved_blocks;
}

struct arch_reserved_span arch_window_apertures(void)
{
    return kickos::chip::window_apertures;
}

struct arch_reserved_span arch_bus_master_apertures(void)
{
    return kickos::chip::bus_master_apertures;
}

struct arch_reserved_span arch_port_apertures(void)
{
    return kickos::chip::port_apertures;
}
