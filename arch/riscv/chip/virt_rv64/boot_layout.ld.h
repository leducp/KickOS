/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * Plain integer constants only: read by both virt_rv64.ld and startup.S.
 */
#ifndef KICKOS_ARCH_RISCV_CHIP_VIRT_RV64_BOOT_LAYOUT_LD_H
#define KICKOS_ARCH_RISCV_CHIP_VIRT_RV64_BOOT_LAYOUT_LD_H

/* QEMU `virt` DRAM. The machine reports 128 MiB from this base; the value below is the
 * image's share of it. */
#define KICKOS_RV64_DRAM_BASE 0x80000000
#define KICKOS_RV64_DRAM_SIZE 0x04000000

/* The physical extent this platform implements, in bits, and a BOARD fact: RISC-V publishes no
 * identity register reporting it, and the PTE's PPN field is 44 bits in every RV64 mode.
 * The map editor is handed this to refuse a range the machine does
 * not implement before the leaf is written.
 */
#define KICKOS_RV64_PHYS_ADDR_BITS 32

/* VA = PA + this, so kernel text links at 0xFFFFFFFF_80000000. The top 2 GiB is the one base
 * the prebuilt medlow libc and libgcc multilibs link against, and it is canonical at every
 * paging mode the Kconfig choice offers.
 */
#define KICKOS_RV64_VA_BASE 0xFFFFFFFF00000000

/* The console and the finisher, physical: reached untranslated before paging is on, to
 * refuse a paging mode this hart does not implement, and at KICKOS_RV64_VA_BASE plus these
 * once it is.
 *
 * The paging mode is a Kconfig choice and lives in <kickos/arch/rv64_paging.h>, which this file
 * may not include: the linker script reads it with no arch include directory.
 */
#define KICKOS_RV64_UART0_PA         0x10000000
#define KICKOS_RV64_UART_POLL_BOUND  100000
#define KICKOS_RV64_TEST_FINISHER_PA 0x00100000
#define KICKOS_RV64_FINISHER_PASS    0x5555
#define KICKOS_RV64_FINISHER_FAIL    0x3333

/* The finisher's FAIL word carries the process exit status in its top half, and 0 there exits
 * 0, which reads as a pass, so the pre-translation prologue shifts KICKOS_FATAL_STATUS in.
 */
#include <fatal_status.ld.h>

/* The app's half. Its text, rodata, data and bss link at KICKOS_RV64_APP_VA_BASE, which a
 * per-space root names and the boot root leaves empty. The app links low and loads high over
 * one uniform delta, which is what __kickos_app_load_delta carries to aspace_image_seed.
 *
 * The base is one whole level-2 slot, index 1, empty at every level in the boot tables, so a
 * space built by copying the boot root owns the subtree under it outright.
 */
#define KICKOS_RV64_APP_VA_BASE  0x40000000
#define KICKOS_RV64_APPTEXT_SIZE 0x00100000
#define KICKOS_RV64_APPDATA_SIZE 0x00100000

/* The kernel's share of DRAM below the app's load window: text, rodata, the ctor arrays, the
 * TLS template and the boot page tables. Also the write-execute boundary: startup.S maps
 * exactly this much read-execute and everything above it read-write, so the value must be a
 * whole number of 2 MiB level-1 leaves, which that file refuses at assembly time.
 */
#define KICKOS_RV64_KTEXT_SIZE 0x00200000

/* The app's load window sits directly above the kernel's, so one delta covers both app regions;
 * added again to KICKOS_RV64_VA_BASE it names the kernel's own alias of that frame.
 */
#define KICKOS_RV64_APP_LMA_BASE   (KICKOS_RV64_DRAM_BASE + KICKOS_RV64_KTEXT_SIZE)
#define KICKOS_RV64_APP_LOAD_DELTA (KICKOS_RV64_APP_LMA_BASE - KICKOS_RV64_APP_VA_BASE)

/* The span of output addresses the kernel-window level-2 slot covers, so an address in it is
 * reached by adding KICKOS_RV64_VA_BASE. startup.S checks this against the slot's own span.
 */
#define KICKOS_RV64_KERNEL_WINDOW_SIZE 0x40000000

/* The transient window the map editor reaches a frame through when no kernel mapping already
 * covers it (aspace_rv64imac.cc, arch_aspace_acquire). One level-2 slot, with a level-1 and a
 * level-0 table under it in .mmu_boot. Every leaf is U-clear. The slot must be one the boot
 * tables do not otherwise use and must sit at a level-1 boundary; startup.S checks both.
 */
#define KICKOS_RV64_WINDOW_VA 0xFFFFFFFFC0000000

#endif
