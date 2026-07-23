// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include "usic_class.h"

#include <usic.h> // chip register offsets + bit fields (register-header dir on the leaf include path)

namespace kickos
{
namespace xmc
{
namespace classdrv
{
    bool usic_tx_ready(uintptr_t base)
    {
        volatile uint32_t const& tcsr =
            *reinterpret_cast<volatile uint32_t*>(base + usic::off::TCSR);
        return (tcsr & usic::TCSR_TDV) == 0u;
    }
}
}
}
