// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Well-known capability-table index convention. Lives in the kickos_system
// library; keep it dependency-free (it is shared verbatim by the kernel and every
// userspace TU).

#ifndef KICKOS_SYS_CAP_INDEX_H
#define KICKOS_SYS_CAP_INDEX_H

// The reserved (well-known) capability indices are kernel policy. An own-create
// (sem/mutex/endpoint create) NEVER lands below KICKOS_CAP_FIRST_DYNAMIC (enforced in
// cap_install), and cap_install_at NEVER writes the reserved stdout slot. So on a board
// that delegates no well-known cap, an app's first create cannot alias a reserved index.
// The kernel seats the reserved slots (stdout) or a parent delegates them at spawn;
// userspace only NAMES them by these constants, it does not choose the index.
//
// The range is not frozen, but a renumber may only go DOWNWARD, only for a slot NOTHING
// seats, and is an ABI break that must land as one commit with the board
// KICKOS_MAX_HANDLES values. Appending a well-known slot RAISES the last reserved index
// and KICKOS_CAP_FIRST_DYNAMIC together and costs one slot on every table in the fleet.
// Keep the range SMALL; the floor static_assert in cap.h guarantees at least one dynamic
// slot remains.
//
// KOS_SPAWN_DELEGATED_CAP0 (abi.h) is INDEPENDENT of this constant: under DEFAULT
// placement delegated cap i lands at child index i+1 whatever the reserved range is, so
// moving KICKOS_CAP_FIRST_DYNAMIC never moves a delegated index. Under that placement the
// first delegated cap lands on index 1, KOS_CAP_CLOCK; a spawn that must not alias a
// well-known name names a destination per grant (kos_thread_params::cap_dest).
#define KICKOS_CAP_FIRST_DYNAMIC 2

enum kos_cap_index
{
    KOS_CAP_STDOUT = 0,    // send-only console endpoint; cap_install_defaults seats it
    KOS_CAP_CLOCK = 1,     // reserved: a board's well-known clock/time service cap
    KOS_CAP_FIRST_DYNAMIC = KICKOS_CAP_FIRST_DYNAMIC // first index an own-create may take
};

// The thread's authority word is NOT a capability-table entry; it lives in the TCB
// beside `privileged`. This pseudo-handle is the name kos_cap_narrow takes for it.
//
// Its low KCAP_INDEX_BITS are all ones, and the codec's capacity rule (cap.h) reserves that
// index and never seats a slot on it, so no table can mint this word at any width.
#define KOS_CAP_AUTHORITY 0x7FFFFFFFu

#endif
