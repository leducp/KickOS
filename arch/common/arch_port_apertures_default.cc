// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include <kickos/arch/arch.h>

#include "chip_tables.h"

struct arch_reserved_span arch_port_apertures(void)
{
    return kickos::chip::port_apertures;
}
