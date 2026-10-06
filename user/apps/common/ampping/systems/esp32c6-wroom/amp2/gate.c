// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The ESP32-C6 partition gate's witness (docs/design-m10-fleet.md, section 9.5): the LP node reads
// the timer group version register through its own /dev/timg1 and through node 0's /dev/timg0,
// which the APM denies it, and node 0 reports both beside its own read of /dev/timg0.

#include <iso646.h> // and / or / not are macros in C, not keywords
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>

#include "book.h"

#include <kickos/sys.h>

// TIMG_NTIMERS_DATE_REG, reset 0x2206072 (TRM v1.2 Register 14.25, p.541).
#define AMPPING_TIMG_DATE 0xF8u
// TIMG0 (TRM v1.2 Table 5.3-2, p.175).
#define AMPPING_TIMG0_BASE 0x60008000u
#define AMPPING_GATE_NODE 1u
#define AMPPING_GATE_WAIT_NS (20ull * 1000ull * 1000ull)
#define AMPPING_GATE_TRIES 40

static uint32_t ampping_date(kos_self_t const* self, char const* device)
{
    kos_window_t const window = kos_grant_mmio(self, device);
    if (kos_window_addr(window) == NULL or kos_window_size(window) <= AMPPING_TIMG_DATE)
    {
        return AMPBOOK_UNSET;
    }
    return *(uint32_t const volatile*)((uintptr_t)kos_window_addr(window) + AMPPING_TIMG_DATE);
}

void ampping_gate_serve(kos_self_t const* self, struct ampbook_row* row)
{
    atomic_store_explicit(&row->gate_held, ampping_date(self, "/dev/timg1"), memory_order_release);
    atomic_store_explicit(&row->gate_probing, 1u, memory_order_release);
    // An APM denial reads zero and does not trap (TRM v1.2 section 16.5).
    uint32_t const foreign = *(uint32_t const volatile*)(AMPPING_TIMG0_BASE + AMPPING_TIMG_DATE);
    atomic_store_explicit(&row->gate_foreign, foreign, memory_order_release);
}

void ampping_gate_report(kos_self_t const* self, struct ampbook_row* book)
{
    uint32_t const own = ampping_date(self, "/dev/timg0");
    struct ampbook_row* const lp = &book[AMPPING_GATE_NODE];
    // The serving node marks its row alive only once both of its reads have returned.
    int tries;
    for (tries = 0; tries < AMPPING_GATE_TRIES; tries++)
    {
        if (atomic_load_explicit(&lp->alive, memory_order_acquire) != AMPBOOK_UNSET)
        {
            break;
        }
        kos_sleep_ns(AMPPING_GATE_WAIT_NS);
    }
    uint32_t const alive = atomic_load_explicit(&lp->alive, memory_order_acquire);
    uint32_t const probing = atomic_load_explicit(&lp->gate_probing, memory_order_acquire);
    unsigned long const held = (unsigned long)atomic_load_explicit(&lp->gate_held, memory_order_acquire);
    unsigned long const foreign = (unsigned long)atomic_load_explicit(&lp->gate_foreign, memory_order_acquire);
    if (alive == AMPBOOK_UNSET)
    {
        printf("ampping: gate: node 0 read 0x%lx from its timg0, node %u read 0x%lx from its timg1 and "
               "has not finished its read of node 0's timg0 (probing %u)\n",
               (unsigned long)own, AMPPING_GATE_NODE, held, (unsigned)probing);
        return;
    }
    printf("ampping: gate: node 0 read 0x%lx from its timg0, node %u read 0x%lx from its timg1 and 0x%lx from "
           "node 0's timg0\n",
           (unsigned long)own, AMPPING_GATE_NODE, held, foreign);
}
