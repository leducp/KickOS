// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The RP2350 partition's ACCESSCTRL witness (docs/design-m10-fleet.md, section 9.5): node 0 holds
// UART0, which node 0's core alone may reach, and reads it. In the node witness's partition
// (KICKOS_WITNESS_NODE), node 1 then raises UART0's line on its own core and has its kernel read
// UART0, which ACCESSCTRL refuses core 1 and so ends node 1 in a bus fault.

#include <iso646.h> // and / or / not are macros in C, not keywords
#include <stdint.h>
#include <stdio.h>

#include "book.h"

#include <kickos/amp.h>
#include <kickos/sys.h>
#include <kickos/sys/witness.h>

// UARTPERIPHID0, 0x11 on a PL011.
#define GATE_PERIPHID0 0xFE0u

#if KICKOS_WITNESS_NODE
// UART0 and its line, which node 1's composition holds neither of (RP2350 datasheet
// RP-008373-DS-2, Table 13, p.32, and Table 95, p.82).
#define GATE_UART0_BASE 0x40070000u
#define GATE_UART0_IRQ 33u
// Past node 0's last line: the read below ends this node.
#define GATE_WITNESS_SETTLE_NS (500ull * 1000ull * 1000ull)

// The C entry of the trap <kickos/arch/arch.h> declares for C++ alone.
uintptr_t arch_syscall(uintptr_t nr, uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3);
#endif

static uint32_t gate_read32(uintptr_t at)
{
    return *(volatile uint32_t const*)at;
}

void ampping_gate_serve(struct ampbook_row* row)
{
    (void)row;
}

void ampping_gate_report(struct ampbook_row* book)
{
    kos_window_t const uart0 = kos_grant_mmio("/dev/uart0");
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

void ampping_gate_served(struct ampbook_row* row, uint32_t served)
{
    (void)row;
#if KICKOS_WITNESS_NODE
    if (served != AMPPING_ROUNDS)
    {
        return;
    }
    kos_sleep_ns(GATE_WITNESS_SETTLE_NS);
    int32_t const moved = (int32_t)arch_syscall(KOS_SYS_WITNESS_NODE, KOS_WITNESS_NODE_STRAY,
                                                GATE_UART0_IRQ, 0, 0);
    printf("ampping: witness: node %u raised line %u on its own core, its kernel counted %ld spurious\n",
           (unsigned)KOS_AMP_SELF_NODE, (unsigned)GATE_UART0_IRQ, (long)moved);
    uint32_t const seen = (uint32_t)arch_syscall(KOS_SYS_WITNESS_NODE, KOS_WITNESS_NODE_READ,
                                                 GATE_UART0_BASE + GATE_PERIPHID0, 0, 0);
    printf("ampping: witness: ERROR: node %u's kernel read 0x%x from node 0's uart0\n",
           (unsigned)KOS_AMP_SELF_NODE, (unsigned)seen);
#else
    (void)served;
#endif
}
