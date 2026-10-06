// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The gate assignment a partition's node 0 programs (docs/design-m10-fleet.md, section 9.5),
// derived from every node's composition and emitted beside the table of each system target that
// carries one. A row gives one node one region of a region gate, or one peripheral of a
// per-peripheral gate.

#ifndef KICKOS_SYS_PARTITION_GATE_H
#define KICKOS_SYS_PARTITION_GATE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define KICKOS_GATE_X 1u
#define KICKOS_GATE_W 2u
#define KICKOS_GATE_R 4u

struct kickos_gate_row
{
    uint64_t base; // a region gate's first byte, or the peripheral's window
    uint64_t size;
    uint16_t gate; // a region gate's place in the chip file's `gates`, else 0
    uint16_t reg;  // a per-peripheral gate's register offset, else 0
    uint8_t node;
    uint8_t access; // KICKOS_GATE_R, KICKOS_GATE_W and KICKOS_GATE_X
    uint16_t rsv0;
};

extern struct kickos_gate_row const kickos_gate_rows[];
extern uint16_t const kickos_gate_row_count;

#ifdef __cplusplus
}
#endif

#endif
