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

    // Attach the mock to a ring of `ring_size` bytes and clear every counter. A negative
    // `irq_line` is a backend with no TX interrupt: no drain ISR, its producers drain it.
    void reset(uint32_t ring_size, int irq_line = 3);

    // The block the ring sits at the start of, STORAGE_SIZE bytes.
    constexpr uint32_t STORAGE_SIZE = 4096u;
    char* storage();

    // Nonzero from the mock's slot_free(). Zero models a wedged TX channel.
    void set_slot_free(int free);

    // Bytes the mock TX edge will take inside ONE drain window, 0 for unlimited. Scoped to the
    // drain ISR, so the prime, flush and synchronous paths are never stalled by it.
    void set_gap_budget(uint32_t bytes);

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

    // Run `fn` once, right after the `ordinal`th byte arch_console_write_sync pushes, counted
    // since reset(): a fault taken in the polled writer.
    void run_in_sync_write(uint32_t ordinal, void (*fn)(void));

    // From now on arch_console_write_sync sends nothing and answers false: a wedged channel,
    // each call one stall window of the polled writer.
    void set_sync_stalls(bool stalls);

    // The most such windows inside ONE contiguous masked span.
    uint32_t max_masked_stalls();

    // Called at the instant USER_OWNED is flipped.
    void note_commit();
}

#endif
