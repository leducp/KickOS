/* SPDX-License-Identifier: CECILL-C */
/* Copyright (c) 2026 Philippe Leduc */
/*
 * Chip constants: facts of the part. Unconditional #define.
 */
#ifndef KICKOS_CHIP_LIMITS_H
#define KICKOS_CHIP_LIMITS_H

/* The ICH9 I/O APIC carries 24 redirection entries. */
#define KICKOS_MAX_IRQ 24

/* No KICKOS_CHIP_CYCCNT_HZ: the bench counter is the TSC, and the fallback arch_cpu_clock_hz
 * reports the rate apic_init measured for that same counter, so it is exact here. */

#endif /* KICKOS_CHIP_LIMITS_H */
