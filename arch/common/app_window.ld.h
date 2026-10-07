/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * NOT a C header: GNU ld statements. arch/CMakeLists.txt preprocesses it AFTER every chip
 * linker script, which it enters as the forced include, so it binds each script whatever that
 * script includes, and reads the KICKOS_LD_C_SYM spelling the script set.
 */
#ifndef KICKOS_ARCH_COMMON_APP_WINDOW_LD_H
#define KICKOS_ARCH_COMMON_APP_WINDOW_LD_H

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
 * the first ASSERT compares the address the body produced against the one the body claims. An
 * assert on the low bits alone would not: ten of the twelve f411disco images were 8-aligned
 * by accident and would have satisfied it, and a base written below the aligned end would
 * overlap app data.
 *
 * The second holds the window's base to _kernel_data_top, where kernel_data_reserve.ld.h pins
 * it, or to kernel .bss's end on a script that declares no pin, within the coarsest MPU granule
 * in the fleet (32 bytes, PMSAv8 and SysMPU): an .appdata aligned coarser and not pinned slides
 * a whole window with kernel .bss. A layout without the MPU carves no window and bases its heap
 * at `.`.
 */
#if KICKOS_HAVE_MPU
ASSERT(KICKOS_LD_C_SYM(_kickos_heap_start) == ALIGN(_appdata_used_end, 8),
       "KickOS: the app heap base is not ALIGN(_appdata_used_end, 8). ALIGN inside .appbss aligns RELATIVE to that section's start, so .appdata must close on ALIGN(8) to keep .appbss 8-aligned, and the base must be written as that expression (arch/common/app_window.ld.h)");
PROVIDE(_kernel_data_top = KICKOS_LD_C_SYM(_ebss));
ASSERT(ADDR(.appdata) - _kernel_data_top < 32,
       "KickOS: the app window does not start within 32 bytes above _kernel_data_top, which a script declaring no pin takes as kernel .bss's end, so it slides with kernel .bss and the user-RAM arena above it loses that slide. Open .appdata with KICKOS_APPDATA_SECTION (arch/common/kernel_data_reserve.ld.h)");
#endif

#endif
