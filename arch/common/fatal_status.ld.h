/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * NOT a C header: one integer constant, read by C++, by a chip's startup assembly and,
 * through a chip's own layout header, by a linker script.
 */
#ifndef KICKOS_ARCH_COMMON_FATAL_STATUS_LD_H
#define KICKOS_ARCH_COMMON_FATAL_STATUS_LD_H

/* The exit status every fatal stop reports, so a harness reads one number whether the kernel
 * died or a boot path refused to start. Distinct from KOS_EXIT_FAULT
 * (user/include/kickos/sys/abi.h), which is one thread's status and not the system's.
 */
#define KICKOS_FATAL_STATUS 132

#endif
