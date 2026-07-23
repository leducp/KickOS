// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include "dspi_class.h"

namespace kickos
{
namespace mk64f
{
namespace classdrv
{
    uint32_t dspi_rx_count(uintptr_t base)
    {
        volatile uint32_t const& sr =
            *reinterpret_cast<volatile uint32_t*>(base + DSPI_SR_OFFSET);
        return (sr >> DSPI_SR_RXCTR_SHIFT) & DSPI_SR_RXCTR_MASK;
    }
}
}
}
