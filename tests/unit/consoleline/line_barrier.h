// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The publish-barrier seam kickos/console_tx.h opens, bound to this gate's hook by
// -DKICKOS_CONSOLE_TX_BARRIER=consoleline_publish_barrier. Force-included into every
// translation unit of this target so the kernel one can name the hook.

#ifndef KICKOS_TESTS_UNIT_CONSOLELINE_LINE_BARRIER_H
#define KICKOS_TESTS_UNIT_CONSOLELINE_LINE_BARRIER_H

// The compiler fence the default expansion carries, plus one seated call. Runs with the ring
// payload written, the head still at the value the copy started from, and the insert holding
// its re-entry flag.
extern "C" void consoleline_publish_barrier(void);

#endif
