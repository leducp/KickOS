// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Well-known capability-table index convention. Lives in the kickos_system
// library; it is shared verbatim by the kernel and every userspace TU.

#ifndef KICKOS_SYS_CAP_INDEX_H
#define KICKOS_SYS_CAP_INDEX_H

// KICKOS_CAP_FIRST_DYNAMIC is a term of the table width the build sums, so the build owns
// it (cmake/cap_geometry.cmake) and emits it into this generated header. The header is
// installed beside the others, so a missing one is an include failure, not a silent 0.
#include <kickos/config/cap_width.h>

// The reserved (well-known) capability indices are kernel policy. An own-create
// (sem/mutex/endpoint create) NEVER lands below KICKOS_CAP_FIRST_DYNAMIC (enforced in
// cap_install), and cap_install_at NEVER writes the reserved stdout slot. The kernel seats
// stdout; userspace only NAMES it by this constant, it does not choose the index. A service
// is reached by path, never by a well-known index.
//
// The range is not frozen, but a renumber may only go DOWNWARD, only for a slot NOTHING
// seats, and is an ABI break. It is one edit, in cmake/cap_geometry.cmake. Appending a
// well-known slot RAISES KICKOS_CAP_FIRST_DYNAMIC and costs one slot on every table in the
// fleet; the floor static_assert in cap.h guarantees at least one dynamic slot remains in
// the narrowest table.
//
// Under DEFAULT placement delegated cap i lands at child index KOS_SPAWN_DELEGATED_CAP0 + i
// (abi.h), which is KOS_CAP_FIRST_DYNAMIC + i: a fresh child's delegations are the first
// dynamic slots, and its own-creates follow them.

enum kos_cap_index
{
    KOS_CAP_STDOUT = 0, // send-only console endpoint; cap_install_defaults seats it
    KOS_CAP_FIRST_DYNAMIC = KICKOS_CAP_FIRST_DYNAMIC // first index an own-create may take
};

// "No capability". No table can mint this word, nor KOS_CAP_AUTHORITY, which shares its
// index field. Written to a minting call's out-parameter on EVERY failure, and carried by
// kos_recv_info.reply_cap for a plain send.
#define KOS_CAP_NONE 0xFFFFFFFFu

// The thread's authority word is NOT a capability-table entry; it lives in the TCB
// beside `privileged`. This pseudo-handle is the name kos_cap_narrow takes for it.
//
// Its low KCAP_INDEX_BITS are all ones, and the codec's capacity rule (cap.h) reserves that
// index and never seats a slot on it, so no table can mint this word at any width.
#define KOS_CAP_AUTHORITY 0x7FFFFFFFu

#endif
