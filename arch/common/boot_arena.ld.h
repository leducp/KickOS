/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * NOT a C header: these macros expand to GNU ld expressions. Included from every chip
 * linker script, which arch/CMakeLists.txt runs through cpp before the link.
 */
#ifndef KICKOS_ARCH_COMMON_BOOT_ARENA_LD_H
#define KICKOS_ARCH_COMMON_BOOT_ARENA_LD_H

/* Link-time replay of the first two arch_ram_alloc() calls kmain makes (the idle boot
 * stack, then the root boot stack), so an arena that cannot hold them fails the BUILD
 * instead of kpanicking on hardware. The alignment run-up is the point: on a
 * pow2-descriptor arch each block's base is snapped to its own rounded size, so a fit
 * computed as ram_start + idle + root is optimistic by up to two padding runs. On a
 * base+limit arch the align is the MPU granule, so the run-up is at most a granule.
 *
 * KICKOS_BOOT_* arrive as -D from arch/CMakeLists.txt, which scrapes the sizes from the
 * board's board_config.h and the granule and encoding mode from arch_mpu_min_region()'s
 * and arch_mpu_region_pow2()'s own sources, so this cannot model a geometry the allocator
 * does not produce. KICKOS_BOOT_*_ALIGN is always a power of two, which is what ALIGN_UP
 * needs. The arena symbols are arguments because the RX ABI spells them with an extra
 * leading underscore.
 */
#define KICKOS_BOOT_ALIGN_UP(v, a) (((v) + ((a) - 1)) & ~((a) - 1))

#define KICKOS_BOOT_IDLE_BASE(ram_start) \
    KICKOS_BOOT_ALIGN_UP(ram_start, KICKOS_BOOT_IDLE_ALIGN)

#define KICKOS_BOOT_ROOT_BASE(ram_start)                                       \
    KICKOS_BOOT_ALIGN_UP(KICKOS_BOOT_IDLE_BASE(ram_start) + KICKOS_BOOT_IDLE_SIZE, \
                         KICKOS_BOOT_ROOT_ALIGN)

#define KICKOS_BOOT_ARENA_ASSERT(ram_start, ram_end)                              \
    ASSERT(KICKOS_BOOT_ROOT_BASE(ram_start) + KICKOS_BOOT_ROOT_SIZE <= (ram_end), \
           "KickOS: the user-RAM arena cannot hold the idle + root boot stacks once MPU natural alignment is paid -- kmain would kpanic before root ever runs. Lower KICKOS_ROOT_STACK_SIZE / KICKOS_IDLE_STACK_SIZE in the board's board_config.h, or cut this image's static footprint.")

#endif
