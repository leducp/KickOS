// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The RP2350 partition's ACCESSCTRL witness (docs/design-m10-fleet.md, section 9.5): node 0 holds
// UART0, which node 0's core alone may reach, and reads it.

#include <iso646.h> // and / or / not are macros in C, not keywords
#include <stdint.h>
#include <stdio.h>

#include "book.h"

#include <kickos/amp.h>
#include <kickos/sys.h>

// UARTPERIPHID0, 0x11 on a PL011.
#define GATE_PERIPHID0 0xFE0u

static uint32_t gate_read32(uintptr_t at)
{
    return *(volatile uint32_t const*)at;
}

void ampping_gate_serve(kos_self_t const* self, struct ampbook_row* row)
{
    (void)self;
    (void)row;
}

void ampping_gate_report(kos_self_t const* self, struct ampbook_row* book)
{
    kos_window_t const uart0 = kos_grant_mmio(self, "/dev/uart0");
    uintptr_t const base = (uintptr_t)kos_window_addr(uart0);
    if (base == 0u)
    {
        printf("ampping: gate: node %u holds no uart0\n", (unsigned)KOS_AMP_SELF_NODE);
        return;
    }
    int const rc = kos_periph_enable(base);
    if (rc != 0)
    {
        printf("ampping: gate: node %u could not enable its uart0, rc %d\n", (unsigned)KOS_AMP_SELF_NODE, rc);
        return;
    }
    uint32_t const held = gate_read32(base + GATE_PERIPHID0);
    (void)book;
    printf("ampping: gate: node %u read 0x%x from its uart0\n", (unsigned)KOS_AMP_SELF_NODE, (unsigned)held);
}
