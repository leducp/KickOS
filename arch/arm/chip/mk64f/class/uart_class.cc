// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include "uart_class.h"

namespace kickos
{
namespace mk64f
{
namespace driver
{
    bool uart0_tx_ready(uintptr_t base)
    {
        // BYTE read (RM 52.3: the UART register file is byte-wide). A wider load here
        // would read S2/C3/D too, and touching D pops a received byte.
        volatile uint8_t const& s1 =
            *reinterpret_cast<volatile uint8_t*>(base + UART_S1_OFFSET);
        if ((s1 & UART_S1_TDRE) != 0)
        {
            return true;
        }
        return false;
    }
}
}
}
