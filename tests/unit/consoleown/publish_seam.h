// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Host seam for kernel/init/console.cc AND kernel/init/console_tx.cc compiled TOGETHER: a
// counted interrupt mask with a gap hook, a mock TX edge, and a drain ISR that runs only
// while the mask is open. Neither ownership state nor ring state is stubbed here, so the
// handover protocol runs as it ships.

#ifndef KICKOS_TESTS_UNIT_CONSOLEOWN_PUBLISH_SEAM_H
#define KICKOS_TESTS_UNIT_CONSOLEOWN_PUBLISH_SEAM_H

#include <stddef.h>
#include <stdint.h>

#include <string>

namespace consolepub
{
    // Bytes the mock TX edge accepted, in wire order, over both transports.
    std::string const& wire();

    // Of those, the ones pushed after note_commit(): a byte here landed on a UART a
    // userspace driver already owns.
    uint32_t pushes_after_commit();

    // The largest number of bytes pushed inside ONE contiguous masked span.
    uint32_t max_masked_pushes();

    // Times the mask fell to zero while a producer was running.
    uint32_t gap_count();

    // Attach the mock to a ring of `ring_size` bytes and clear every counter.
    void reset(uint32_t ring_size);

    // Whether the drain ISR may run in a mask gap. False models a handler the NVIC can no
    // longer reach, which is what irq_detach plus the line mask leaves behind.
    void set_isr_runs_in_gap(bool runs);

    // Run `fn` once, in the `ordinal`th mask gap of the next producer call. Ordinal 1 is
    // the gap that closes the state-read-plus-count bracket, before the ring is consulted.
    void run_in_gap(uint32_t ordinal, void (*fn)(void));

    // Whether that seated function ran.
    bool seat_fired();

    // Called by the fixture's publish replica at the instant it flips USER_OWNED.
    void note_commit();
}

#endif
