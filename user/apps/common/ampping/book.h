// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The partition region every node of the ampping partition maps, `/shm/ampbook`: one row per
// node, each written by that node's task alone and read by node 0's.

#ifndef KICKOS_USER_APPS_COMMON_AMPPING_BOOK_H
#define KICKOS_USER_APPS_COMMON_AMPPING_BOOK_H

#include <stdatomic.h>
#include <stdint.h>

#include <kickos/sys.h>

#define AMPBOOK_NAME "/shm/ampbook"

// The region's initial word, which no node stores as a mark.
#define AMPBOOK_UNSET 0u

struct ampbook_row
{
    // The first port the partition names the node, biased by one.
    _Atomic uint32_t alive;
    // The calls its server answered, stored ahead of each reply.
    _Atomic uint32_t served;
    // A gate witness's two reads: a device the node holds, and one another node holds.
    _Atomic uint32_t gate_held;
    _Atomic uint32_t gate_foreign;
    // Set before the foreign read, so a read that never returned is told from one never made.
    _Atomic uint32_t gate_probing;
    uint32_t rsv[11];
};

_Static_assert(sizeof(struct ampbook_row) == 64u, "a row is its own 64 bytes");

// The port of this task's first crossing of `right` (KOS_CAP_WAIT to serve, KOS_CAP_SIGNAL to
// use) whose server is `node`, any node for KOS_AMP_NO_ENTRY, and its capability, looked up by its
// /amp name; KOS_AMP_NO_ENTRY where it holds none.
uint32_t ampping_crossing(kos_self_t const* self, uint8_t right, uint32_t node, kos_cap_t* cap);

// The board's gate witness: the serving node's reads, made before it serves, and node 0's report
// of them. Each is empty on a board whose partition states no gate.
void ampping_gate_serve(kos_self_t const* self, struct ampbook_row* row);
void ampping_gate_report(kos_self_t const* self, struct ampbook_row* book);

#endif
