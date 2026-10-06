// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include <kickos/arch/arch.h>

#include "chip_tables.h"

struct arch_reserved_span arch_window_apertures(void)
{
    return kickos::chip::window_apertures;
}
