/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * The vector slots switch.S hands kickos_armv8a_exception, and the exception class both sides
 * test. Plain integer #defines: switch.S includes it.
 */

#ifndef KICKOS_ARCH_ARM64_ARMV8A_VECTOR_SLOTS_H
#define KICKOS_ARCH_ARM64_ARMV8A_VECTOR_SLOTS_H

#define KICKOS_ARMV8A_SLOT_EL0_SYNC 8
#define KICKOS_ARMV8A_SLOT_EL0_SERROR 11

/* ESR_ELx.EC of an SError (DDI 0487 M.b, ESR_EL1). */
#define KICKOS_ARMV8A_EC_SERROR 0x2F

#endif
