/* SPDX-License-Identifier: CECILL-C */
/* Copyright (c) 2026 Philippe Leduc */
/*
 * Chip constants: facts of the part, not knobs. Unconditional #define.
 */
#ifndef KICKOS_CHIP_LIMITS_H
#define KICKOS_CHIP_LIMITS_H

/* RISC-V mcause interrupt-code space (single mtvec demux) */
#define KICKOS_MAX_IRQ 32

/* Rate of the counter the bench reads, which is NOT the core clock here: `rdcycle` under
 * this emulator answers the host's own tick source, so no rate converts a reading into
 * guest time. 0 is what drops the nanosecond columns instead of printing them wrong. */
#define KICKOS_CHIP_CYCCNT_HZ 0

#endif /* KICKOS_CHIP_LIMITS_H */
