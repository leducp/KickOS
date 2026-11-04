/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * NOT a C header: GNU ld statements. arch/CMakeLists.txt preprocesses it AFTER every chip
 * linker script, which it enters as the forced include, so it binds each script whatever that
 * script includes, and reads the KICKOS_LD_C_SYM spelling the script set.
 */
#ifndef KICKOS_ARCH_COMMON_APP_WINDOW_LD_H
#define KICKOS_ARCH_COMMON_APP_WINDOW_LD_H

/* A script that declares no reserve (kernel_data_reserve.ld.h) bases the first window on kernel
 * .bss's end. */
PROVIDE(_kernel_data_top = KICKOS_LD_C_SYM(_ebss));

#endif
