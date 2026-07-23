// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include "port_class.h"

namespace kickos
{
namespace rx
{
namespace classdrv
{
    uint8_t port_odr_read(uintptr_t podr_base, unsigned port)
    {
        volatile uint8_t const& podr =
            *reinterpret_cast<volatile uint8_t*>(podr_base + port);
        return podr;
    }
}
}
}
