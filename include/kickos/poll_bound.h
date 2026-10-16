// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The bound on a raw synchronous MMIO poll, shared by the kernel and the drivers above it: a
// wedged peripheral must degrade to an error and never hang its caller.

#ifndef KICKOS_POLL_BOUND_H
#define KICKOS_POLL_BOUND_H

// Reads. It dwarfs a UART's per-byte wait at any baud; a poll whose unit of progress can take
// longer adds that unit's own bound to it.
#define KICKOS_POLL_SPIN_MAX 1000000u

#endif
