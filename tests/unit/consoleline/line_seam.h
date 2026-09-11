// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The console transport as a LINE producer sees it: a counted interrupt mask, a mock TX edge
// whose completion event is TRANSITION triggered, a drain the arm delivers by hand, and the
// chip's own console route so a refused line takes the fallback the contract owes it.
//
// The arm chooses the arming: a negative IRQ line is a backend the producer drains, a
// non-negative one a backend whose TX-empty ISR does.

#ifndef KICKOS_TESTS_UNIT_CONSOLELINE_LINE_SEAM_H
#define KICKOS_TESTS_UNIT_CONSOLELINE_LINE_SEAM_H

#include <stddef.h>
#include <stdint.h>

#include <string>

namespace consoleline
{
    // Attach the mock to a ring of `ring_size` bytes armed at `irq_line`, and clear every
    // record and every seat.
    void reset(uint32_t ring_size, int irq_line);

    // Bytes the mock TX edge accepted, in wire order, over every transport.
    std::string const& wire();

    // Nonzero from the mock's slot_free(). Zero is a wedged TX channel.
    void set_slot_free(int free);

    // Deliver the TX-empty ISR while the channel holds a completion event and the peripheral
    // TX interrupt is enabled. A push raises the next event, so an idle channel stays dark
    // until something primes it.
    void pump_tx_isr();

    // Times pump_tx_isr entered console_tx_isr since reset.
    uint32_t isr_entries();

    // Run `fn` once, inside the `ordinal`th push to the TX edge, after the byte is taken.
    // The drain's push runs with interrupts open, so this is where a preempting thread lands.
    void run_in_push(uint32_t ordinal, void (*fn)(void));
    bool push_seat_fired();

    // Pushes taken while another push was on the stack, and the deepest such nesting.
    uint32_t nested_pushes();
    uint32_t max_push_depth();

    // Run `fn` once at the next ring copy's publish barrier.
    void run_at_publish_barrier(void (*fn)(void));
    bool barrier_seat_fired();

    // One line through a chip's console route: the ring takes it whole, or the refusal goes
    // straight at the device under one mask, with '\n' lowered as `crlf` asks. Returns what
    // console_tx_insert_line returned.
    int write_line(char const* buf, size_t n, int crlf);
}

#endif
