// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// FROZEN well-known capability-table index convention. Lives in the kickos_system
// library; keep it dependency-free (it is shared verbatim by the kernel and every
// userspace TU).

#ifndef KICKOS_SYS_CAP_INDEX_H
#define KICKOS_SYS_CAP_INDEX_H

// The reserved (well-known) capability indices are FROZEN kernel policy. An own-create
// (sem/mutex/endpoint create) NEVER lands below KICKOS_CAP_FIRST_DYNAMIC (enforced in
// cap_install), and cap_install_at NEVER writes the reserved stdout slot. So on a board
// that delegates no well-known cap, an app's first create cannot alias a reserved index.
// The kernel seats the reserved slots (stdout) or a parent delegates them at spawn;
// userspace only NAMES them by these constants, it does not choose the index.
//
// Frozen means: never renumber an existing entry. Append a new well-known slot by RAISING
// the last reserved index and KICKOS_CAP_FIRST_DYNAMIC together. Keep the range SMALL;
// every reserved slot is one fewer dynamic slot on the tiny boards (the floor static_assert
// in cap.h guarantees at least one dynamic slot remains).
#define KICKOS_CAP_FIRST_DYNAMIC 4

enum kos_cap_index
{
    KOS_CAP_STDOUT = 0,    // send-only console endpoint; cap_install_defaults seats it
    // Reserved range 1..3 is kernel-seated-only by convention today; the delegation-packing
    // enforcement that seats these lands with the clock-service step.
    KOS_CAP_CLOCK = 1,     // reserved: a board's well-known clock/time service cap
    KOS_CAP_SERVICE = 2,   // reserved: a second well-known service cap slot
    KOS_CAP_RESERVED3 = 3, // reserved spare; hold the range, do not repurpose ad hoc
    KOS_CAP_FIRST_DYNAMIC = KICKOS_CAP_FIRST_DYNAMIC // first index an own-create may take
};

#endif
