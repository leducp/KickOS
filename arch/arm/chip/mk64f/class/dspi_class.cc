// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include "dspi_class.h"

#include "../regs/dspi.h"

namespace kickos
{
namespace mk64f
{
namespace driver
{
    uint32_t dspi_rx_count(uintptr_t base)
    {
        volatile uint32_t const& sr =
            *reinterpret_cast<volatile uint32_t*>(base + reg::dspi::SR_OFFSET);
        return (sr >> reg::dspi::SR_RXCTR_SHIFT) & reg::dspi::SR_RXCTR_MASK;
    }
}
}
}
