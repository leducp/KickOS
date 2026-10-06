// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The RP2350 partition's ACCESSCTRL witness (docs/design-m10-fleet.md, section 9.5): node 0 holds
// UART0, which node 0's core alone may reach, and node 1's kernel reads the same register.

#include <iso646.h> // and / or / not are macros in C, not keywords
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "book.h"

#include <kickos/amp.h>
#include <kickos/sys.h>
#include <kickos/sys/abi_probe.h>

// UARTPERIPHID0, 0x11 on a PL011.
#define GATE_PERIPHID0 0xFE0u
// Set on every stored read, so a read of zero is told from none.
#define GATE_READ 0x100u
// Node 1's kernel's verdict on its read, as KOS_AMP_OP_GATE_PROBE answers it.
#define GATE_RETURNED 1u
#define GATE_FAULTED 2u
#define GATE_UNPROBED 3u
#define GATE_HOLDER 0u
#define GATE_PROBER 1u
#define GATE_POLL_NS (20ull * 1000ull * 1000ull)
#define GATE_PROBE_TRIES 50
// Long enough for node 0's alive sweep and its own read, which the probe waits on.
#define GATE_HELD_TRIES 500

static uint32_t gate_read32(uintptr_t at)
{
    return *(volatile uint32_t const*)at;
}

static void gate_park(void)
{
    while (true)
    {
        kos_sleep_ns(GATE_POLL_NS * 50u);
    }
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
    atomic_store_explicit(&book[KOS_AMP_SELF_NODE].gate_held, held | GATE_READ, memory_order_release);

    struct ampbook_row* const prober = &book[GATE_PROBER];
    uint32_t probing = AMPBOOK_UNSET;
    uint32_t foreign = AMPBOOK_UNSET;
    int tries;
    for (tries = 0; tries < GATE_PROBE_TRIES; tries++)
    {
        probing = atomic_load_explicit(&prober->gate_probing, memory_order_acquire);
        foreign = atomic_load_explicit(&prober->gate_foreign, memory_order_acquire);
        if (foreign != AMPBOOK_UNSET)
        {
            break;
        }
        kos_sleep_ns(GATE_POLL_NS);
    }
    char const* verdict = "a kernel with no gate probe";
    if (foreign == GATE_FAULTED)
    {
        verdict = "a fault";
    }
    else if (foreign == GATE_RETURNED)
    {
        verdict = "a value";
    }
    if (foreign != AMPBOOK_UNSET)
    {
        printf("ampping: gate: node %u read 0x%x from its uart0, node %u's kernel's read of it took %s\n",
               (unsigned)KOS_AMP_SELF_NODE, (unsigned)held, GATE_PROBER, verdict);
    }
    else if (probing != AMPBOOK_UNSET)
    {
        printf("ampping: gate: node %u read 0x%x from its uart0, node %u's read of it never returned\n",
               (unsigned)KOS_AMP_SELF_NODE, (unsigned)held, GATE_PROBER);
    }
    else
    {
        printf("ampping: gate: node %u read 0x%x from its uart0, node %u never probed it\n",
               (unsigned)KOS_AMP_SELF_NODE, (unsigned)held, GATE_PROBER);
    }
}

void ampprobe_main(kos_self_t const* self)
{
    kos_window_t const region = kos_grant_mem(self, AMPBOOK_NAME);
    struct ampbook_row* const book = (struct ampbook_row*)kos_window_addr(region);
    if (book == NULL or kos_window_size(region) < KICKOS_AMP_NODES * sizeof(struct ampbook_row))
    {
        printf("ampping: gate: node %u's probe holds no partition region\n", (unsigned)KOS_AMP_SELF_NODE);
        gate_park();
    }
    // UART0 stays in reset until node 0 enables it, and a read of a block in reset proves
    // nothing about the gate.
    int tries;
    for (tries = 0; tries < GATE_HELD_TRIES; tries++)
    {
        if (atomic_load_explicit(&book[GATE_HOLDER].gate_held, memory_order_acquire) != AMPBOOK_UNSET)
        {
            break;
        }
        kos_sleep_ns(GATE_POLL_NS);
    }
    if (tries == GATE_HELD_TRIES)
    {
        gate_park();
    }
    struct ampbook_row* const row = &book[KOS_AMP_SELF_NODE];
    atomic_store_explicit(&row->gate_probing, 1u, memory_order_release);
    uint32_t verdict = GATE_UNPROBED;
#if defined(KICKOS_ENABLE_SELFTEST)
    intptr_t const rc = (intptr_t)kos_amp_probe(KOS_AMP_OP_GATE_PROBE, 0);
    if (rc == 1)
    {
        verdict = GATE_FAULTED;
    }
    else if (rc == 0)
    {
        verdict = GATE_RETURNED;
    }
#endif
    atomic_store_explicit(&row->gate_foreign, verdict, memory_order_release);
    gate_park();
}
