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
 * KICKOS_BOOT_* arrive as -D from arch/CMakeLists.txt, which takes the sizes from the
 * resolved configuration (cmake/boot_arena.cmake reads the generated
 * kickos_config.cmake) and the granule and encoding mode from arch_mpu_min_region()'s
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
           "KickOS: the user-RAM arena cannot hold the idle + root boot stacks once MPU natural alignment is paid; kmain would kpanic before root ever runs. Lower KICKOS_ROOT_STACK_SIZE / KICKOS_IDLE_STACK_SIZE in the board's variant defconfig (boards/<board>/configs/<variant>/defconfig), or cut this image's static footprint, which where KICKOS_KERNEL_STACKS is 1 includes KICKOS_THREAD_SLOTS blocks of KICKOS_KERNEL_STACK_SIZE in kernel .bss below the arena.")

/* Same replay carried one step further: past the two boot stacks the arena must also back
 * KICKOS_MAX_THREADS blocks of KICKOS_USER_STACK_SIZE, because that is what the pool
 * bump-allocates on demand (syscall_thread.cc). The count is SLOTS MINUS ROOT and not the
 * slot count: the pool holds KICKOS_THREAD_SLOTS, and root's slot draws its stack from the
 * boot replay above instead.
 *
 * KICKOS_POOL_STACK_* arrive as -D beside the KICKOS_BOOT_* set, from the same source.
 *
 * THE STRIDE IS NOT ALWAYS THE SIZE. Where the alignment exceeds the size, which is what
 * KICKOS_TLS does to every block so the ARM thread pointer can be SP masked down to it,
 * EVERY pool block pays a run-up and not just the first.
 */
#define KICKOS_POOL_STRIDE \
    KICKOS_BOOT_ALIGN_UP(KICKOS_POOL_STACK_SIZE, KICKOS_POOL_STACK_ALIGN)

#define KICKOS_POOL_BASE(ram_start)                                                  \
    KICKOS_BOOT_ALIGN_UP(KICKOS_BOOT_ROOT_BASE(ram_start) + KICKOS_BOOT_ROOT_SIZE,   \
                         KICKOS_POOL_STACK_ALIGN)

#define KICKOS_POOL_TOP(ram_start) \
    (KICKOS_POOL_BASE(ram_start) + KICKOS_POOL_STACK_COUNT * KICKOS_POOL_STRIDE)

/* Invoked by every linker script the fleet links, board-local overrides included, and
 * omitting it is a configure FATAL_ERROR exactly as for KICKOS_BOOT_ARENA_ASSERT. The arena
 * base is a link-time value, so only the linker can do this arithmetic.
 *
 * This binds the FATTEST image on the board, not an average one: where __kickos_ram_start
 * floats with .bss, the heaviest image is what caps KICKOS_MAX_THREADS. Before trimming the
 * demand, price every section carved between .bss and the arena base, because each has a
 * different owner: .userheap (KICKOS_USER_HEAP_SIZE) and, on an enforcing chip, the
 * .appdata window. The per-thread kernel-stack block is a fourth owner and the one this
 * arithmetic cannot see: it is .bss itself rather than a section above it, so it moves
 * ram_start instead of appearing as a term here.
 */
#define KICKOS_POOL_ARENA_ASSERT(ram_start, ram_end)                    \
    ASSERT(KICKOS_POOL_TOP(ram_start) <= (ram_end),                     \
           "KickOS: the user-RAM arena cannot back KICKOS_MAX_THREADS default stacks of KICKOS_USER_STACK_SIZE past the two boot stacks, so this board advertises thread slots it cannot seat; kos_thread_spawn would return -KOS_ENOMEM for a slot the board claims to have, indistinguishable at runtime from a full slot table. Lower KICKOS_MAX_THREADS / KICKOS_USER_STACK_SIZE in the board's variant defconfig (boards/<board>/configs/<variant>/defconfig), or cut this image's static footprint, which where KICKOS_KERNEL_STACKS is 1 includes KICKOS_THREAD_SLOTS blocks of KICKOS_KERNEL_STACK_SIZE in kernel .bss below the arena.")

#endif
