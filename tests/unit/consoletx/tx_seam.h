// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Host seam for kernel/init/console_tx.cc: a counted interrupt mask, a mock TX edge, and
// a drain ISR that runs ONLY while the mask is open. A producer that never opens the mask
// can therefore never be drained, which is what makes the masked-push metric below
// separate a bit-banged transmission from a ring enqueue.

#ifndef KICKOS_TESTS_UNIT_CONSOLETX_TX_SEAM_H
#define KICKOS_TESTS_UNIT_CONSOLETX_TX_SEAM_H

#include <stddef.h>
#include <stdint.h>

#include <string>

namespace consoletxfix
{
    // Bytes the mock TX edge accepted, in wire order.
    std::string const& wire();

    // The largest number of bytes pushed inside ONE contiguous masked span. With IRQs off,
    // every push costs a byte time at the line rate.
    uint32_t max_masked_pushes();

    // Times the mask fell to zero while a producer was running (the drain windows).
    uint32_t gap_count();

    // Attach the mock to a ring of `ring_size` bytes and clear every counter.
    void reset(uint32_t ring_size);

    // Nonzero from the mock's slot_free(). Zero models a wedged TX channel.
    void set_slot_free(int free);

    // Whether the drain ISR is allowed to run in a mask gap. False models an ISR that
    // cannot reach the CPU, which is the only case the synchronous fallback is for.
    void set_isr_runs_in_gap(bool runs);

    // The mock peripheral's TX-interrupt enable, as the last irq_enable/irq_disable left
    // it. An enqueue that runs after console_tx_deinit turns it back on, latching a pend
    // on a line the NVIC has already masked.
    bool tx_irq_enabled();

    // Runs once, in the FIRST mask gap of the next producer call, to seat a concurrent
    // producer exactly where an interleaving can happen.
    void run_in_first_gap(void (*fn)(void));
}

#endif
