/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * NOT a C header: plain integer constants, read by BOTH virt_rv64.ld and startup.S. The
 * linker's kernel base, the app's window and every table index the boot tables fill are
 * derived from the same values here, so they cannot disagree.
 */
#ifndef KICKOS_ARCH_RISCV_CHIP_VIRT_RV64_BOOT_LAYOUT_LD_H
#define KICKOS_ARCH_RISCV_CHIP_VIRT_RV64_BOOT_LAYOUT_LD_H

/* QEMU `virt` DRAM. The machine reports 128 MiB from this base; the value below is the
 * image's share of it. */
#define KICKOS_RV64_DRAM_BASE 0x80000000
#define KICKOS_RV64_DRAM_SIZE 0x04000000

/* VA = PA + this, so kernel text links at 0xFFFFFFFF_80000000.
 *
 * NOT A PREFERENCE. medlow on RV64 materialises bits 31:12 sign-extended, so the prebuilt
 * libc and libgcc multilibs reach only [0, 0x7FFFFFFF] and [0xFFFFFFFF80000000,
 * 0xFFFFFFFFFFFFFFFF], and their own references to their own data take R_RISCV_HI20
 * truncations against any other base. The top 2 GiB is the one placement that links, DRAM here
 * starting at 0x80000000 so the bottom range is unusable; 0xFFFFFFC0_00000000, the bottom of
 * Sv39's high half, is refused exactly as 0x8000_0000 is (docs/design-m6-mmu.md R1.4b).
 *
 * AND IT NEEDS NO REWORK PER PAGING MODE, which startup.S checks rather than assumes: this
 * base is canonical at every mode the Kconfig choice offers, the bits above the mode's
 * virtual width being all ones in each (docs/design-m6-mmu.md R3).
 */
#define KICKOS_RV64_VA_BASE 0xFFFFFFFF00000000

/* THE CONSOLE AND THE FINISHER, PHYSICAL. Named here rather than in chip_virt_rv64.cc alone
 * because the pre-translation prologue refuses a paging mode this hart does not implement
 * through both of them, before any C has run (docs/design-m6-mmu.md R3). startup.S reaches
 * them untranslated; the chip reaches the same registers at the kernel's own alias, which is
 * KICKOS_RV64_VA_BASE plus these.
 *
 * The paging mode itself is NOT here: it is a Kconfig choice and the level count is derived
 * from it in <kickos/arch/rv64_paging.h>, which this file may not include because the linker
 * script reads it with no arch include directory.
 */
#define KICKOS_RV64_UART0_PA         0x10000000
#define KICKOS_RV64_UART_POLL_BOUND  100000
#define KICKOS_RV64_TEST_FINISHER_PA 0x00100000
#define KICKOS_RV64_FINISHER_PASS    0x5555
#define KICKOS_RV64_FINISHER_FAIL    0x3333

/* The finisher's FAIL word carries the process exit status in its top half, and 0 there exits
 * 0, which reads as a pass. This is the status every fatal stop on this board uses, so a
 * harness sees one number whether the kernel died or the boot path refused to start.
 */
#define KICKOS_RV64_FATAL_STATUS 132

/* THE IMAGE IS SPLIT IN TWO AND THIS IS THE APP'S HALF (docs/design-m6-mmu.md R2.2). Kernel
 * text and kernel writable state link into the high range above; the app's text, rodata, data
 * and bss link at KICKOS_RV64_APP_VA_BASE, a virtual address a per-space root names and the
 * root leaves empty, so an unprivileged thread reaches the app's half and no other.
 *
 * VMA IS NOT LMA HERE AND CANNOT BE. A per-space mapping needs the app's link addresses to be
 * ones a level-2 slot of its own can cover, and the prebuilt libc and libgcc multilibs are
 * medlow, which reaches [0, 0x7FFFFFFF] and [0xFFFFFFFF80000000, 0xFFFFFFFFFFFFFFFF] and
 * nothing between. Every byte of DRAM on this machine is at or above 0x80000000, so no
 * identity-linked app window exists at all: the app links low and loads high, one uniform
 * delta, which is what __kickos_app_load_delta carries to aspace_image_seed.
 *
 * KICKOS_RV64_APP_VA_BASE IS ONE WHOLE LEVEL-2 SLOT, index 1, immediately above the first
 * gigabyte of the low half. The boot tables leave it empty at every level, so a space built by
 * copying the boot root's entries owns the subtree under it outright and no per-space edit
 * touches a table another space shares.
 */
#define KICKOS_RV64_APP_VA_BASE  0x40000000
#define KICKOS_RV64_APPTEXT_SIZE 0x00100000
#define KICKOS_RV64_APPDATA_SIZE 0x00100000

/* The kernel's share of DRAM below the app's load window: text, rodata, the ctor arrays, the
 * TLS template and the boot page tables. ld reports an overflow by region name.
 *
 * ALSO THE WRITE-EXECUTE BOUNDARY (docs/design-m6-mmu.md R2.3): startup.S maps exactly this
 * much of the kernel window read-execute and everything above it read-write, so the value must
 * be a whole number of 2 MiB level-1 leaves, which that file refuses at assembly time.
 */
#define KICKOS_RV64_KTEXT_SIZE 0x00200000

/* The app's load window sits directly above the kernel's, so ONE delta covers both app regions:
 * added to an app virtual address it names the frame the loader put those bytes in, and added
 * again to KICKOS_RV64_VA_BASE it names the kernel's own alias of that frame.
 */
#define KICKOS_RV64_APP_LMA_BASE   (KICKOS_RV64_DRAM_BASE + KICKOS_RV64_KTEXT_SIZE)
#define KICKOS_RV64_APP_LOAD_DELTA (KICKOS_RV64_APP_LMA_BASE - KICKOS_RV64_APP_VA_BASE)

/* THE KERNEL WINDOW: the span of OUTPUT addresses that the kernel-window level-2 slot
 * covers, so an address in it is reached by adding KICKOS_RV64_VA_BASE (startup.S, which checks
 * this against the slot's own span). The frame pool is carved inside it, which is what lets the
 * map editor read a table it allocated without spending a transient window slot.
 */
#define KICKOS_RV64_KERNEL_WINDOW_SIZE 0x40000000

/* THE TRANSIENT WINDOW the map editor reaches a frame through when no kernel mapping already
 * covers it (arch/riscv/rv64imac/aspace_rv64imac.cc, arch_aspace_acquire). One level-2 slot,
 * with a level-1 and a level-0 table under it in .mmu_boot; startup.S fills the two non-leaf
 * entries and the map editor writes the leaves. Every leaf is U-clear, so no
 * unprivileged thread reaches a slot whichever space is live.
 *
 * The slot must be one the boot tables do not otherwise use, and the address must sit at a
 * level-1 boundary so both tables are reached at index 0. startup.S checks both.
 */
#define KICKOS_RV64_WINDOW_VA 0xFFFFFFFFC0000000

#endif
