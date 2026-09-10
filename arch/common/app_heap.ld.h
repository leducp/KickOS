/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * NOT a C header: this macro expands to a GNU ld statement. Included from every chip linker
 * script that carves an app window, which arch/CMakeLists.txt runs through cpp before the link.
 */
#ifndef KICKOS_ARCH_COMMON_APP_HEAP_LD_H
#define KICKOS_ARCH_COMMON_APP_HEAP_LD_H

/* The newlib heap is the unused pad of the app's granted window, and its base is written
 * inside .appbss as ALIGN(_appdata_used_end, 8). INSIDE AN OUTPUT SECTION BODY ld ALIGNS
 * RELATIVE TO THE SECTION START, the same evaluation rule kernel_data_reserve.ld.h records
 * for an absolute value assigned to `.` in a body, so that expression yields an 8-aligned
 * address only while .appbss itself starts 8-aligned. .appbss is based at the window start
 * plus SIZEOF(.appdata) and the window start is at least 32-aligned, so what decides it is
 * .appdata's closing alignment: it is ALIGN(8) on every script for this reason and nothing
 * else.
 *
 * At FILE SCOPE there is no section to be relative to, so the same expression is absolute and
 * this ASSERT compares the address the body produced against the one the body claims. An
 * assert on the low bits alone would not: ten of the twelve f411disco images were 8-aligned
 * by accident and would have satisfied it.
 */
#define KICKOS_APP_HEAP_ALIGN_ASSERT(heap_start, used_end)                                \
    ASSERT((heap_start) == ALIGN((used_end), 8),                                          \
           "KickOS: the app heap base is not 8-aligned. ALIGN(_appdata_used_end, 8) inside .appbss aligns RELATIVE to that section's start, so .appdata must close on ALIGN(8) to keep .appbss 8-aligned (arch/common/app_heap.ld.h)")

#endif
