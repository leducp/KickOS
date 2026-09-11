// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The mock console transport: a counted interrupt mask with a gap hook, a mock TX edge, and
// a drain ISR that runs ONLY while the mask is open. A producer that never opens the mask can
// therefore never be drained, which is what makes the masked-push metric separate a
// bit-banged transmission from a ring enqueue.

#ifndef KICKOS_TESTS_UNIT_CONSOLESEAM_CONSOLE_SEAM_H
#define KICKOS_TESTS_UNIT_CONSOLESEAM_CONSOLE_SEAM_H

#include <stddef.h>
#include <stdint.h>

#include <string>

namespace consoleseam
{
    // Bytes the mock TX edge accepted, in wire order, over every transport.
    std::string const& wire();

    // Of those, the ones pushed after note_commit(): a byte here landed on a UART a
    // userspace driver already owns.
    uint32_t pushes_after_commit();

    // The largest number of bytes pushed inside ONE contiguous masked span. With IRQs off,
    // every push costs a byte time at the line rate.
    uint32_t max_masked_pushes();

    // Times the mask fell to zero while a producer was running (the drain windows).
    uint32_t gap_count();

    // Attach the mock to a ring of `ring_size` bytes and clear every counter.
    void reset(uint32_t ring_size);

    // Nonzero from the mock's slot_free(). Zero models a wedged TX channel.
    void set_slot_free(int free);

    // Whether the drain ISR may run in a mask gap. False models a handler the NVIC can no
    // longer reach, which is what irq_detach plus the line mask leaves behind.
    void set_isr_runs_in_gap(bool runs);

    // The mock peripheral's TX-interrupt enable, as the last irq_enable/irq_disable left
    // it. An enqueue that runs after console_tx_deinit turns it back on, latching a pend
    // on a line the NVIC has already masked.
    bool tx_irq_enabled();

    // Run `fn` once, in the `ordinal`th mask gap counted since reset().
    void run_in_gap(uint32_t ordinal, void (*fn)(void));

    // Whether that seated function ran.
    bool seat_fired();

    // Called at the instant USER_OWNED is flipped.
    void note_commit();
}

#endif
