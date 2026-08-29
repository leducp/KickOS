// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#ifndef KICKOS_CHIP_COM1_H
#define KICKOS_CHIP_COM1_H

#include <stdint.h>

namespace kickos::q35
{
    void com1_init(void);
    void com1_putc(char c);

    // com1_putc polls only the holding register, so a 16550 with its transmit FIFO on can
    // outrun a shutdown; this waits for the shift register too, bounded.
    void com1_drain(void);

    void com1_puts(char const* s);
    void com1_hex64(uint64_t v);
    void com1_dec(uint64_t v);
}

#endif
