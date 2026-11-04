/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * Read by BOTH the program link script (program.ld) and C++.
 *
 * Every program links at this user address and every space maps its image there: top-level slot
 * 254 of a four-level root, and inside slot 0 of a five-level one, where firmware's identity map
 * is. Where the adopted root maps the slot, each space holds its own copy of firmware's table
 * under it (aspace_x86_64.cc).
 */

#ifndef KICKOS_ARCH_PROGRAM_BASE_H
#define KICKOS_ARCH_PROGRAM_BASE_H

#define KICKOS_X86_64_PROGRAM_BASE 0x00007f0000000000

#endif
